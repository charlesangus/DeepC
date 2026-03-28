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

use std::sync::Mutex;

use futures::executor::block_on;
use ndarray::{Array2, Array3};
use once_cell::sync::Lazy;
use opendefocus::{
    OpenDefocusRenderer,
    datamodel::{
        IVector4, Settings, UVector2,
        circle_of_confusion::{CameraData, Filmback, Resolution, WorldUnit},
        defocus::DefocusMode,
        render::{FilterMode, Quality, RenderSpecs, ResultMode},
    },
};

// ---------------------------------------------------------------------------
// Singleton renderer — initialised once, reused across all renderStripe calls.
// Initialisation is async; we block the first call with block_on().
// ---------------------------------------------------------------------------

static RENDERER: Lazy<Mutex<Option<OpenDefocusRenderer>>> = Lazy::new(|| Mutex::new(None));

fn get_renderer() -> OpenDefocusRenderer {
    // We create a fresh renderer per call rather than storing inside a Lazy
    // to avoid poisoning issues and because GPU init is cheap after the first
    // wgpu instance is created in-process.  A per-frame singleton would
    // require Arc<Mutex<…>> and is deferred to S03 optimisation.
    let mut settings = Settings::default();
    match block_on(OpenDefocusRenderer::new(true, &mut settings)) {
        Ok(r) => {
            if r.is_gpu() {
                eprintln!("[opendefocus-deep] GPU backend active");
            } else {
                eprintln!("[opendefocus-deep] CPU fallback: rayon");
            }
            r
        }
        Err(e) => {
            eprintln!("[opendefocus-deep] renderer init error: {e} — retrying CPU-only");
            block_on(OpenDefocusRenderer::new(false, &mut settings))
                .expect("OpenDefocusRenderer CPU init must not fail")
        }
    }
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

// SAFETY: raw pointers.  Callers guarantee the pointed-to data outlives the
// function call and is not mutated concurrently (single-threaded renderStripe).
unsafe impl Send for DeepSampleData {}
unsafe impl Sync for DeepSampleData {}

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
    // 5. Initialise renderer (GPU with CPU fallback).
    // -----------------------------------------------------------------------
    let renderer = get_renderer();

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
                    layer_rgba[base] = r;
                    layer_rgba[base + 1] = g;
                    layer_rgba[base + 2] = b;
                    layer_rgba[base + 3] = a;
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
        settings.render.quality = Quality::Low.into();
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

        // ── 6e. Render layer. render_stripe takes ownership of depth ──
        //        and a mutable view into the image array.
        let mut rendered = image_array;
        let render_result = block_on(renderer.render_stripe(
            render_specs,
            settings,
            rendered.view_mut(),
            Some(depth_array),
            None,
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
