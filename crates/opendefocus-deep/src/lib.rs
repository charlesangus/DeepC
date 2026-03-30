// opendefocus-deep — deep-image defocus engine
//
// This crate bridges the Nuke DeepCOpenDefocus C++ plugin to the opendefocus
// GPU-accelerated convolution library via a stable `extern "C"` boundary.
//
// Architecture: layer-peeling approach.
//   1. Reconstruct per-pixel deep samples from flat FFI arrays.
//   2. Sort each pixel's samples front-to-back by depth.
//   3. For each "layer" index i = 0..max_layers:
//        a. Extract a flat RGBA image (pixels without sample i → transparent).
//        b. Extract a per-pixel depth map for that layer.
//        c. Construct opendefocus Settings with CameraData (drives internal CoC).
//        d. Dispatch through OpenDefocusRenderer (GPU → CPU fallback).
//        e. Alpha-composite ("over") the rendered layer onto the accumulator.
//   4. Write the accumulator to output_rgba.

use futures::executor::block_on;
use ndarray::{Array2, Array3};
use opendefocus::{
    MipmapBuffer,
    OpenDefocusRenderer,
    datamodel::{
        IVector4, Settings, UVector2,
        circle_of_confusion::{CameraData, Filmback, Resolution, WorldUnit},
        defocus::DefocusMode,
        render::{FilterMode, RenderSpecs, ResultMode},
    },
};
use std::sync::{Arc, Mutex, OnceLock};

// ---------------------------------------------------------------------------
// Static singleton renderer — initialised once on first FFI call.
// ---------------------------------------------------------------------------

/// Global renderer singleton.  Initialised on the first `render_impl` call
/// using the caller's `use_gpu` flag.  All subsequent calls reuse the same
/// underlying `OpenDefocusRenderer` instance, eliminating the per-call
/// async-init overhead.
static RENDERER: OnceLock<Arc<Mutex<OpenDefocusRenderer>>> = OnceLock::new();

/// Acquire (or lazily initialise) the singleton renderer.
///
/// If the `OnceLock` is not yet populated the renderer is created with
/// `prefer_gpu = use_gpu != 0`.  If GPU init fails the function falls back
/// to CPU and still stores the result, so subsequent callers always get a
/// valid renderer regardless of `use_gpu`.
fn get_or_init_renderer(use_gpu: i32) -> Arc<Mutex<OpenDefocusRenderer>> {
    RENDERER
        .get_or_init(|| {
            let mut settings_init = Settings::default();
            let renderer = if use_gpu != 0 {
                match block_on(OpenDefocusRenderer::new(true, &mut settings_init)) {
                    Ok(r) => {
                        if r.is_gpu() {
                            eprintln!("[opendefocus-deep] singleton: GPU backend active");
                        } else {
                            eprintln!(
                                "[opendefocus-deep] singleton: GPU requested but unavailable — CPU fallback"
                            );
                        }
                        r
                    }
                    Err(e) => {
                        eprintln!(
                            "[opendefocus-deep] singleton: GPU init error: {e} — falling back to CPU"
                        );
                        block_on(OpenDefocusRenderer::new(false, &mut settings_init))
                            .expect("OpenDefocusRenderer CPU init must not fail")
                    }
                }
            } else {
                eprintln!("[opendefocus-deep] singleton: use_gpu=0 — CPU-only path");
                block_on(OpenDefocusRenderer::new(false, &mut settings_init))
                    .expect("OpenDefocusRenderer CPU init must not fail")
            };
            Arc::new(Mutex::new(renderer))
        })
        .clone()
}

// ---------------------------------------------------------------------------
// FFI types — must stay in sync with include/opendefocus_deep.h
// ---------------------------------------------------------------------------

/// LensParams — C-compatible struct carrying lens/camera parameters.
///
/// Passed from C++ (Nuke knob values) so the Rust kernel can compute the
/// circle-of-confusion (CoC) for each deep sample in S02.
/// Layout must remain in sync with `include/opendefocus_deep.h`.
#[repr(C)]
pub struct LensParams {
    /// Lens focal length in millimetres (e.g. 50.0).
    pub focal_length_mm: f32,
    /// Aperture f-stop (e.g. 2.8).  Lower = wider aperture = more blur.
    pub fstop: f32,
    /// Distance to the plane of focus in scene units.
    pub focus_distance: f32,
    /// Sensor width in millimetres (full-frame = 36.0).
    pub sensor_size_mm: f32,
}

/// DeepSampleData — C-compatible struct passed from C++ (Nuke deep pixels).
///
/// All pointer fields are owned by the caller; this crate never frees them.
/// Layout must remain in sync with `include/opendefocus_deep.h`.
#[repr(C)]
pub struct DeepSampleData {
    /// Per-pixel sample counts (length: pixel_count).
    pub sample_counts: *const u32,
    /// Interleaved RGBA samples (length: total_samples * 4).
    pub rgba: *const f32,
    /// Per-sample depth values (length: total_samples).
    pub depth: *const f32,
    /// Number of pixels in the image.
    pub pixel_count: u32,
    /// Total number of deep samples across all pixels.
    pub total_samples: u32,
}

/// HoldoutData — holdout transmittance map passed from the caller.
///
/// Each pixel `i` encodes two (depth, T) breakpoints as `data[i*4..i*4+4]` =
/// `[d0, T0, d1, T1]`.  Pixels with no holdout use `[0.0, 1.0, 0.0, 1.0]`
/// (T = 1.0 at all depths).  An empty slice (`len = 0`) means no holdout.
///
/// Layout must remain in sync with `include/opendefocus_deep.h`.
#[repr(C)]
pub struct HoldoutData {
    /// Flat f32 array: `width * height * 4` values encoding `(d0, T0, d1, T1)`
    /// per pixel.  May be null when `len` is 0.
    pub data: *const f32,
    /// Number of `f32` values in `data`; must be `width * height * 4` or 0.
    pub len: u32,
}

// SAFETY: raw pointer.  Callers guarantee validity for the duration of the call.
unsafe impl Send for HoldoutData {}
unsafe impl Sync for HoldoutData {}


// ---------------------------------------------------------------------------
// Public FFI entry point
// ---------------------------------------------------------------------------

/// Render entry point — layer-peeled deep defocus via OpenDefocusRenderer.
///
/// # Safety
/// - `data` must point to a valid `DeepSampleData` with pointer fields valid
///   for `pixel_count` / `total_samples` elements respectively.
/// - `output_rgba` must point to a buffer of at least `width * height * 4`
///   `f32` values.
/// - `lens` must point to a valid `LensParams`.
/// - All pointers must be non-null and properly aligned.
#[no_mangle]
pub extern "C" fn opendefocus_deep_render(
    data: *const DeepSampleData,
    output_rgba: *mut f32,
    width: u32,
    height: u32,
    lens: *const LensParams,
    use_gpu: i32,
    quality: i32,
) {
    render_impl(data, output_rgba, width, height, lens, use_gpu, &[], quality);
}

/// Render entry point with holdout transmittance map.
///
/// Identical to `opendefocus_deep_render` but accepts a holdout map that
/// attenuates gathered samples by per-pixel transmittance, enabling correct
/// depth-aware compositing with holdout mattes.
///
/// # Safety
/// All safety requirements of `opendefocus_deep_render` apply.  Additionally:
/// - If `holdout` is non-null, `holdout->data` must be non-null when
///   `holdout->len > 0`, and `holdout->len` must equal `width * height * 4`.
#[no_mangle]
pub extern "C" fn opendefocus_deep_render_holdout(
    data: *const DeepSampleData,
    output_rgba: *mut f32,
    width: u32,
    height: u32,
    lens: *const LensParams,
    use_gpu: i32,
    holdout: *const HoldoutData,
    quality: i32,
) {
    // Build the holdout slice from the FFI struct (null → empty slice).
    let holdout_slice: &[f32] = if holdout.is_null() {
        &[]
    } else {
        // SAFETY: caller guarantees holdout points to a valid HoldoutData.
        let h = unsafe { &*holdout };
        if h.len == 0 || h.data.is_null() {
            &[]
        } else {
            // SAFETY: data is non-null and len floats are valid per caller contract.
            unsafe { std::slice::from_raw_parts(h.data, h.len as usize) }
        }
    };

    render_impl(data, output_rgba, width, height, lens, use_gpu, holdout_slice, quality);
}

// ---------------------------------------------------------------------------
// Holdout transmittance lookup (scene-depth space)
// ---------------------------------------------------------------------------

/// Look up cumulative transmittance for a sample at scene depth `z` in the
/// holdout map.  Returns 1.0 (no attenuation) when holdout is empty or the
/// pixel is out of bounds.
///
/// Encoding: `holdout[pixel_idx * 4 .. +4]` = `(d0, T0, d1, T1)`.
/// - `z > d1` → T1
/// - `z > d0` → T0
/// - otherwise → 1.0
fn holdout_transmittance(holdout: &[f32], pixel_idx: usize, z: f32) -> f32 {
    if holdout.is_empty() {
        return 1.0;
    }
    let base = pixel_idx * 4;
    if holdout.len() < base + 4 {
        return 1.0;
    }
    let d0 = holdout[base];
    let t0 = holdout[base + 1];
    let d1 = holdout[base + 2];
    let t1 = holdout[base + 3];
    if z > d1 {
        t1
    } else if z > d0 {
        t0
    } else {
        1.0
    }
}

// ---------------------------------------------------------------------------
// Internal render implementation
// ---------------------------------------------------------------------------

/// Shared implementation for both FFI entry points.  `holdout` is a flat
/// `width * height * 4` f32 slice (or empty → no holdout attenuation).
fn render_impl(
    data: *const DeepSampleData,
    output_rgba: *mut f32,
    width: u32,
    height: u32,
    lens: *const LensParams,
    use_gpu: i32,
    holdout: &[f32],
    quality: i32,
) {
    // -----------------------------------------------------------------------
    // 0. Null-guard + early-out for empty images.
    // -----------------------------------------------------------------------
    if data.is_null() || output_rgba.is_null() || lens.is_null() {
        eprintln!("[opendefocus-deep] null pointer passed — skipping render");
        return;
    }

    let pixel_count = width as usize * height as usize;
    if pixel_count == 0 {
        return;
    }

    // SAFETY: all pointer validity is guaranteed by the caller contract above.
    let (sample_counts, rgba_ptr, depth_ptr, total_samples, lens_ref) = unsafe {
        let d = &*data;
        let l = &*lens;
        (
            std::slice::from_raw_parts(d.sample_counts, pixel_count),
            std::slice::from_raw_parts(d.rgba, d.total_samples as usize * 4),
            std::slice::from_raw_parts(d.depth, d.total_samples as usize),
            d.total_samples as usize,
            l,
        )
    };

    // Sanity: if no samples at all, zero-fill and return.
    if total_samples == 0 {
        let out =
            unsafe { std::slice::from_raw_parts_mut(output_rgba, pixel_count * 4) };
        out.fill(0.0);
        return;
    }

    // -----------------------------------------------------------------------
    // 1. Reconstruct per-pixel sample arrays (r, g, b, a, z).
    //    Each pixel owns a Vec of (r,g,b,a,z) tuples.
    // -----------------------------------------------------------------------
    let mut pixels: Vec<Vec<(f32, f32, f32, f32, f32)>> = Vec::with_capacity(pixel_count);
    {
        let mut sample_offset = 0usize;
        for px in 0..pixel_count {
            let count = sample_counts[px] as usize;
            let mut samples = Vec::with_capacity(count);
            for s in 0..count {
                let si = sample_offset + s;
                let ri = si * 4;
                let r = rgba_ptr[ri];
                let g = rgba_ptr[ri + 1];
                let b = rgba_ptr[ri + 2];
                let a = rgba_ptr[ri + 3];
                let z = depth_ptr[si];
                samples.push((r, g, b, a, z));
            }
            sample_offset += count;
            pixels.push(samples);
        }
    }

    // -----------------------------------------------------------------------
    // 2. Sort each pixel's samples front-to-back (ascending depth).
    // -----------------------------------------------------------------------
    for px_samples in &mut pixels {
        px_samples.sort_by(|a, b| a.4.partial_cmp(&b.4).unwrap_or(std::cmp::Ordering::Equal));
    }

    // -----------------------------------------------------------------------
    // 3. Determine max layer count.
    // -----------------------------------------------------------------------
    let max_layers = pixels.iter().map(|s| s.len()).max().unwrap_or(0);
    if max_layers == 0 {
        let out =
            unsafe { std::slice::from_raw_parts_mut(output_rgba, pixel_count * 4) };
        out.fill(0.0);
        return;
    }

    // -----------------------------------------------------------------------
    // 4. Allocate accumulation buffer (H × W × 4, premultiplied RGBA).
    // -----------------------------------------------------------------------
    let h = height as usize;
    let w = width as usize;
    // accum[y][x][c]  — stored as a flat vec for easy slice access
    let mut accum: Vec<f32> = vec![0.0f32; h * w * 4];

    // -----------------------------------------------------------------------
    // 5. Acquire the singleton renderer (init on first call, reuse thereafter).
    // -----------------------------------------------------------------------
    let renderer_arc = get_or_init_renderer(use_gpu);
    let renderer_guard = renderer_arc.lock().expect("renderer mutex must not be poisoned");

    // -----------------------------------------------------------------------
    // 5a. Hoist filter/mipmap preparation outside the layer loop.
    //
    //     The filter shape (FilterMode::Simple with the given resolution) is
    //     constant across all layers — only depth/focal-plane settings differ.
    //     Build a template Settings just for filter prep.
    // -----------------------------------------------------------------------
    let filter_prep_settings = {
        let mut s = Settings::default();
        s.render.resolution = UVector2 { x: w as u32, y: h as u32 };
        s.render.filter.mode = FilterMode::Simple.into();
        s.defocus.defocus_mode = DefocusMode::Depth.into();
        s
    };
    let filter_mipmaps: MipmapBuffer<f32> = block_on(renderer_guard.prepare_filter_mipmaps(
        &filter_prep_settings,
        4, // RGBA
    ))
    .expect("[opendefocus-deep] prepare_filter_mipmaps must not fail");

    // -----------------------------------------------------------------------
    // 6. Layer-peel loop.
    // -----------------------------------------------------------------------
    for layer_idx in 0..max_layers {
        // ── 6a. Build flat layer RGBA (H × W × 4) and depth map (H × W). ──
        let mut layer_rgba = vec![0.0f32; h * w * 4];
        let mut layer_depth = vec![0.0f32; h * w];

        for py in 0..h {
            for px in 0..w {
                let pixel_idx = py * w + px;
                let base = pixel_idx * 4;
                if let Some(&(r, g, b, a, z)) = pixels[pixel_idx].get(layer_idx) {
                    // Apply holdout transmittance in scene-depth space before
                    // the kernel converts depths to CoC.  The holdout slice
                    // encodes (d0, T0, d1, T1) per pixel; T is cumulative
                    // transmittance at the given depth breakpoint.
                    let t = holdout_transmittance(holdout, pixel_idx, z);
                    layer_rgba[base] = r * t;
                    layer_rgba[base + 1] = g * t;
                    layer_rgba[base + 2] = b * t;
                    layer_rgba[base + 3] = a * t;
                    layer_depth[pixel_idx] = z;
                }
                // pixels without this layer stay at (0,0,0,0) / depth 0.0
            }
        }

        // ── 6b. Build opendefocus Settings with CameraData for CoC. ──
        let mut settings = Settings::default();

        // Resolution
        settings.render.resolution = UVector2 {
            x: w as u32,
            y: h as u32,
        };
        settings.render.center = opendefocus::datamodel::Vector2 {
            x: w as f32 / 2.0,
            y: h as f32 / 2.0,
        };
        // Proto-generated enums are stored as i32 — use .into() to convert.
        settings.render.quality = quality.into();
        settings.render.result_mode = ResultMode::Result.into();
        settings.render.filter.mode = FilterMode::Simple.into();
        settings.render.gui = false;

        // Defocus mode: depth-driven CoC.
        // Per proto, camera_data lives under settings.defocus.circle_of_confusion.
        settings.defocus.defocus_mode = DefocusMode::Depth.into();
        settings.defocus.circle_of_confusion.camera_data = Some(CameraData {
            focal_length: lens_ref.focal_length_mm,
            f_stop: lens_ref.fstop,
            filmback: Filmback {
                width: lens_ref.sensor_size_mm,
                // Assume 16:9 aspect; height computed from width
                height: lens_ref.sensor_size_mm * (h as f32) / (w as f32),
            },
            // focus_distance maps to near_field for in-focus zone start.
            near_field: (lens_ref.focus_distance * 0.9).max(0.01),
            far_field: lens_ref.focus_distance * 1.1 + 100.0,
            world_unit: WorldUnit::M.into(),
            resolution: Resolution {
                width: w as u32,
                height: h as u32,
            },
        });
        // focal_plane in CoC settings drives the focus plane position
        settings.defocus.circle_of_confusion.focal_plane = lens_ref.focus_distance;

        // ── 6c. Build ndarray views. ──
        let image_array: Array3<f32> = match Array3::from_shape_vec((h, w, 4), layer_rgba) {
            Ok(a) => a,
            Err(e) => {
                eprintln!("[opendefocus-deep] layer {layer_idx} shape error: {e}");
                continue;
            }
        };

        let depth_array: Array2<f32> = match Array2::from_shape_vec((h, w), layer_depth) {
            Ok(a) => a,
            Err(e) => {
                eprintln!("[opendefocus-deep] layer {layer_idx} depth shape error: {e}");
                continue;
            }
        };

        // ── 6d. Build RenderSpecs (full image = full stripe). ──
        let render_specs = RenderSpecs {
            full_region: IVector4 {
                x: 0,
                y: 0,
                z: w as i32,
                w: h as i32,
            },
            render_region: IVector4 {
                x: 0,
                y: 0,
                z: w as i32,
                w: h as i32,
            },
        };

        // ── 6e. Render layer using pre-built filter mipmaps (no per-layer
        //        filter/mipmap prep or Telea inpaint).
        //        Holdout attenuation was already applied in scene-depth space
        //        (step 6a) — pass empty slice to avoid double-attenuation in
        //        the kernel's CoC-space apply_holdout_attenuation.
        let mut rendered = image_array;
        let render_result = block_on(renderer_guard.render_stripe_prepped(
            render_specs,
            settings,
            rendered.view_mut(),
            depth_array,
            &filter_mipmaps,
            &[],
        ));

        if let Err(e) = render_result {
            eprintln!("[opendefocus-deep] layer {layer_idx} render error: {e}");
            continue;
        }

        // ── 6f. Front-to-back "over" composite: accum = layer over accum. ──
        // "over" in premultiplied alpha:
        //   out_rgb = layer_rgb + accum_rgb * (1 - layer_alpha)
        //   out_a   = layer_a   + accum_a   * (1 - layer_alpha)
        let rendered_slice = rendered.as_slice().expect("rendered array must be contiguous");
        for i in 0..(h * w) {
            let base = i * 4;
            let la = rendered_slice[base + 3]; // layer alpha
            let inv_la = 1.0 - la.min(1.0).max(0.0);

            accum[base]     = rendered_slice[base]     + accum[base]     * inv_la;
            accum[base + 1] = rendered_slice[base + 1] + accum[base + 1] * inv_la;
            accum[base + 2] = rendered_slice[base + 2] + accum[base + 2] * inv_la;
            accum[base + 3] = la                       + accum[base + 3] * inv_la;
        }
    }

    // -----------------------------------------------------------------------
    // 7. Write accumulation buffer to output_rgba.
    // -----------------------------------------------------------------------
    let out = unsafe { std::slice::from_raw_parts_mut(output_rgba, pixel_count * 4) };
    out.copy_from_slice(&accum);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    /// Prove that holdout transmittance attenuates background samples.
    ///
    /// 3×1 image:
    ///   pixel 0: red   at depth 5.0 (behind holdout at z=3.0 → T=0.1)
    ///   pixel 1: green at depth 2.0 (in front of holdout → T=1.0)
    ///   pixel 2: blue  at depth 5.0 (behind holdout at z=3.0 → T=0.1)
    ///
    /// After render, green (unattenuated) must exceed red and blue
    /// (attenuated by T≈0.1).
    #[test]
    fn test_holdout_attenuates_background_samples() {
        let width: u32 = 3;
        let height: u32 = 1;

        // One sample per pixel.
        let sample_counts: [u32; 3] = [1, 1, 1];

        // Premultiplied RGBA: red, green, blue — all fully opaque.
        let rgba: [f32; 12] = [
            1.0, 0.0, 0.0, 1.0, // pixel 0: red
            0.0, 1.0, 0.0, 1.0, // pixel 1: green
            0.0, 0.0, 1.0, 1.0, // pixel 2: blue
        ];

        // Depths: pixel 0 and 2 behind holdout (5.0 > 3.0), pixel 1 in front (2.0 < 3.0).
        let depth: [f32; 3] = [5.0, 2.0, 5.0];

        let lens = LensParams {
            focal_length_mm: 50.0,
            fstop: 8.0,
            focus_distance: 100.0,
            sensor_size_mm: 36.0,
        };

        // Holdout: 4 floats per pixel = (d0, T0, d1, T1).
        // Surface at z=3.0 with T=0.1 for samples behind it.
        let holdout_data: [f32; 12] = [
            3.0, 0.1, 3.0, 0.1, // pixel 0
            3.0, 0.1, 3.0, 0.1, // pixel 1
            3.0, 0.1, 3.0, 0.1, // pixel 2
        ];

        let holdout = HoldoutData {
            data: holdout_data.as_ptr(),
            len: 12,
        };

        let deep_data = DeepSampleData {
            sample_counts: sample_counts.as_ptr(),
            rgba: rgba.as_ptr(),
            depth: depth.as_ptr(),
            pixel_count: 3,
            total_samples: 3,
        };

        let mut output_rgba: [f32; 12] = [0.0; 12];

        // Render via CPU path (use_gpu=0).
        opendefocus_deep_render_holdout(
            &deep_data as *const DeepSampleData,
            output_rgba.as_mut_ptr(),
            width,
            height,
            &lens as *const LensParams,
            0, // CPU only
            &holdout as *const HoldoutData,
            0, // Quality::Low
        );

        // Output layout: [R0 G0 B0 A0  R1 G1 B1 A1  R2 G2 B2 A2]
        let px0_red = output_rgba[0];   // pixel 0, red channel
        let px1_green = output_rgba[5]; // pixel 1, green channel
        let px2_blue = output_rgba[10]; // pixel 2, blue channel

        eprintln!(
            "[test] px0_red={px0_red:.4} px1_green={px1_green:.4} px2_blue={px2_blue:.4}"
        );
        eprintln!("[test] full output: {:?}", output_rgba);

        // Green (in front of holdout, unattenuated) must exceed red and blue
        // (behind holdout, attenuated by T≈0.1).
        assert!(
            px1_green > px0_red,
            "green (front of holdout) {px1_green} should exceed attenuated red {px0_red}"
        );
        assert!(
            px1_green > px2_blue,
            "green (front of holdout) {px1_green} should exceed attenuated blue {px2_blue}"
        );
    }

    /// Document singleton + prep hoisting speedup via wall-clock timing.
    ///
    /// Calls `opendefocus_deep_render` twice on a minimal 4×4 single-sample
    /// image.  The first call is "cold" (may initialise the singleton if this
    /// test runs before all others); the second call is "warm" (singleton and
    /// filter mipmaps are already built).  Both elapsed times are printed with
    /// `eprintln!` (visible under `--nocapture`).
    ///
    /// The only hard assertion is that the warm call completes in under 5 000 ms
    /// — a loose regression guard, not a tight SLA.
    #[test]
    fn test_render_timing() {
        let width: u32 = 4;
        let height: u32 = 4;
        let pixel_count: usize = (width * height) as usize;

        // One sample per pixel.
        let sample_counts: [u32; 16] = [1; 16];

        // Mid-grey, fully opaque.
        let mut rgba = vec![0.0f32; pixel_count * 4];
        for i in 0..pixel_count {
            rgba[i * 4]     = 0.5;
            rgba[i * 4 + 1] = 0.5;
            rgba[i * 4 + 2] = 0.5;
            rgba[i * 4 + 3] = 1.0;
        }

        // Uniform depth.
        let depth: [f32; 16] = [2.0; 16];

        let lens = LensParams {
            focal_length_mm: 50.0,
            fstop: 8.0,
            focus_distance: 5.0,
            sensor_size_mm: 36.0,
        };

        let deep_data = DeepSampleData {
            sample_counts: sample_counts.as_ptr(),
            rgba: rgba.as_ptr(),
            depth: depth.as_ptr(),
            pixel_count: pixel_count as u32,
            total_samples: pixel_count as u32,
        };

        let mut output1 = vec![0.0f32; pixel_count * 4];
        let mut output2 = vec![0.0f32; pixel_count * 4];

        // ── Cold call (may init the singleton). ──
        let t0 = std::time::Instant::now();
        opendefocus_deep_render(
            &deep_data as *const DeepSampleData,
            output1.as_mut_ptr(),
            width,
            height,
            &lens as *const LensParams,
            0, // CPU only
            0, // Quality::Low
        );
        let cold_ms = t0.elapsed().as_millis();

        // ── Warm call (singleton already initialised). ──
        let t1 = std::time::Instant::now();
        opendefocus_deep_render(
            &deep_data as *const DeepSampleData,
            output2.as_mut_ptr(),
            width,
            height,
            &lens as *const LensParams,
            0, // CPU only
            0, // Quality::Low
        );
        let warm_ms = t1.elapsed().as_millis();

        eprintln!(
            "[test_render_timing] cold={cold_ms}ms  warm={warm_ms}ms"
        );

        // Loose regression guard — not a tight SLA.
        assert!(
            warm_ms < 5000,
            "warm render took {warm_ms}ms (> 5000ms regression threshold)"
        );
    }

    /// Document holdout-path timing — singleton + prep hoisting speedup.
    ///
    /// Calls `opendefocus_deep_render_holdout` twice on a minimal 4×4
    /// single-sample image with a trivial holdout slice (T=1.0 everywhere, i.e.
    /// no attenuation).  Cold and warm elapsed times are printed; the warm call
    /// must complete in under 5 000 ms.
    #[test]
    fn test_holdout_timing() {
        let width: u32 = 4;
        let height: u32 = 4;
        let pixel_count: usize = (width * height) as usize;

        // One sample per pixel.
        let sample_counts: [u32; 16] = [1; 16];

        // Mid-grey, fully opaque.
        let mut rgba = vec![0.0f32; pixel_count * 4];
        for i in 0..pixel_count {
            rgba[i * 4]     = 0.5;
            rgba[i * 4 + 1] = 0.5;
            rgba[i * 4 + 2] = 0.5;
            rgba[i * 4 + 3] = 1.0;
        }

        // Uniform depth.
        let depth: [f32; 16] = [2.0; 16];

        let lens = LensParams {
            focal_length_mm: 50.0,
            fstop: 8.0,
            focus_distance: 5.0,
            sensor_size_mm: 36.0,
        };

        // Minimal holdout: T=1.0 at all depths (no attenuation).
        // Encoding: (d0=0.0, T0=1.0, d1=0.0, T1=1.0) per pixel.
        let holdout_raw: Vec<f32> = {
            let mut v = vec![0.0f32; pixel_count * 4];
            for i in 0..pixel_count {
                v[i * 4 + 1] = 1.0; // T0
                v[i * 4 + 3] = 1.0; // T1
            }
            v
        };

        let holdout = HoldoutData {
            data: holdout_raw.as_ptr(),
            len: (pixel_count * 4) as u32,
        };

        let deep_data = DeepSampleData {
            sample_counts: sample_counts.as_ptr(),
            rgba: rgba.as_ptr(),
            depth: depth.as_ptr(),
            pixel_count: pixel_count as u32,
            total_samples: pixel_count as u32,
        };

        let mut output1 = vec![0.0f32; pixel_count * 4];
        let mut output2 = vec![0.0f32; pixel_count * 4];

        // ── Cold call. ──
        let t0 = std::time::Instant::now();
        opendefocus_deep_render_holdout(
            &deep_data as *const DeepSampleData,
            output1.as_mut_ptr(),
            width,
            height,
            &lens as *const LensParams,
            0, // CPU only
            &holdout as *const HoldoutData,
            0, // Quality::Low
        );
        let cold_ms = t0.elapsed().as_millis();

        // ── Warm call. ──
        let t1 = std::time::Instant::now();
        opendefocus_deep_render_holdout(
            &deep_data as *const DeepSampleData,
            output2.as_mut_ptr(),
            width,
            height,
            &lens as *const LensParams,
            0, // CPU only
            &holdout as *const HoldoutData,
            0, // Quality::Low
        );
        let warm_ms = t1.elapsed().as_millis();

        eprintln!(
            "[test_holdout_timing] cold={cold_ms}ms  warm={warm_ms}ms"
        );

        // Loose regression guard.
        assert!(
            warm_ms < 5000,
            "warm holdout render took {warm_ms}ms (> 5000ms regression threshold)"
        );
    }

    /// Verify that `opendefocus_deep_render` can be called twice without panic.
    ///
    /// This proves the singleton is reused correctly: the first call initialises
    /// the static `OnceLock`; the second call must not re-initialise or deadlock.
    /// Uses a 2×2 single-sample image on the CPU path.
    #[test]
    fn test_singleton_reuse() {
        let width: u32 = 2;
        let height: u32 = 2;

        // One sample per pixel, four pixels.
        let sample_counts: [u32; 4] = [1, 1, 1, 1];
        let rgba: [f32; 16] = [
            0.5, 0.5, 0.5, 1.0, // pixel 0
            0.5, 0.5, 0.5, 1.0, // pixel 1
            0.5, 0.5, 0.5, 1.0, // pixel 2
            0.5, 0.5, 0.5, 1.0, // pixel 3
        ];
        let depth: [f32; 4] = [1.0, 1.0, 1.0, 1.0];

        let lens = LensParams {
            focal_length_mm: 50.0,
            fstop: 8.0,
            focus_distance: 5.0,
            sensor_size_mm: 36.0,
        };

        let deep_data = DeepSampleData {
            sample_counts: sample_counts.as_ptr(),
            rgba: rgba.as_ptr(),
            depth: depth.as_ptr(),
            pixel_count: 4,
            total_samples: 4,
        };

        let mut output1: [f32; 16] = [0.0; 16];
        let mut output2: [f32; 16] = [0.0; 16];

        // First call — initialises the singleton.
        opendefocus_deep_render(
            &deep_data as *const DeepSampleData,
            output1.as_mut_ptr(),
            width,
            height,
            &lens as *const LensParams,
            0, // CPU only
            0, // Quality::Low
        );

        // Second call — must reuse the singleton without panic or deadlock.
        opendefocus_deep_render(
            &deep_data as *const DeepSampleData,
            output2.as_mut_ptr(),
            width,
            height,
            &lens as *const LensParams,
            0, // CPU only
            0, // Quality::Low
        );

        // Both calls should produce valid (non-NaN) output.
        for (i, &v) in output1.iter().enumerate() {
            assert!(!v.is_nan(), "output1[{i}] is NaN");
        }
        for (i, &v) in output2.iter().enumerate() {
            assert!(!v.is_nan(), "output2[{i}] is NaN");
        }

        eprintln!("[test_singleton_reuse] call 1: {:?}", &output1[..4]);
        eprintln!("[test_singleton_reuse] call 2: {:?}", &output2[..4]);
    }
}
