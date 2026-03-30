#pragma once

#include <stdint.h>

/**
 * LensParams — mirrors the Rust #[repr(C)] struct in opendefocus-deep.
 *
 * Carries lens/camera parameters needed to compute circle-of-confusion (CoC)
 * for each deep sample.  The layout MUST stay in sync with `src/lib.rs`.
 */
typedef struct LensParams {
    /** Lens focal length in millimetres (e.g. 50.0). */
    float focal_length_mm;
    /** Aperture f-stop (e.g. 2.8).  Lower = wider aperture = more blur. */
    float fstop;
    /** Distance to the plane of focus in scene units. */
    float focus_distance;
    /** Sensor width in millimetres (full-frame = 36.0). */
    float sensor_size_mm;
} LensParams;

/**
 * DeepSampleData — mirrors the Rust #[repr(C)] struct in opendefocus-deep.
 *
 * All pointer fields point to memory owned by the caller; the Rust lib never
 * frees them.  The layout MUST stay in sync with `src/lib.rs`.
 */
typedef struct DeepSampleData {
    /** Per-pixel sample counts (length: pixel_count). */
    const uint32_t* sample_counts;
    /** Interleaved RGBA samples (length: total_samples * 4). */
    const float*    rgba;
    /** Per-sample depth values (length: total_samples). */
    const float*    depth;
    /** Number of pixels in the image. */
    uint32_t        pixel_count;
    /** Total number of deep samples across all pixels. */
    uint32_t        total_samples;
} DeepSampleData;

/**
 * HoldoutData — holdout transmittance map.
 *
 * Each pixel i encodes two (depth, T) breakpoints:
 *   data[i*4 + 0] = d0   (near depth breakpoint)
 *   data[i*4 + 1] = T0   (transmittance at d0)
 *   data[i*4 + 2] = d1   (far depth breakpoint)
 *   data[i*4 + 3] = T1   (transmittance beyond d1)
 *
 * Pixels with no holdout should encode [0.0, 1.0, 0.0, 1.0] (T=1 everywhere).
 * A null data pointer with len=0 means no holdout (equivalent to T=1 for all).
 *
 * The layout MUST stay in sync with `src/lib.rs`.
 */
typedef struct HoldoutData {
    /** Flat float array: width*height*4 values encoding (d0,T0,d1,T1) per pixel.
     *  May be null when len is 0. */
    const float* data;
    /** Number of floats in data; must be width*height*4 or 0. */
    uint32_t     len;
} HoldoutData;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Render deep-defocus into output_rgba.
 *
 * S01: stub — returns immediately without modifying output_rgba.
 * S02: replaced with GPU kernel dispatch using per-sample CoC from lens.
 *
 * @param data         Valid pointer to DeepSampleData; pointer fields must be
 *                     non-null and valid for pixel_count / total_samples entries.
 * @param output_rgba  Output buffer; at least width * height * 4 floats.
 * @param width        Image width in pixels.
 * @param height       Image height in pixels.
 * @param lens         Lens parameters for CoC computation; must be non-null.
 * @param use_gpu      Non-zero to attempt GPU backend; 0 forces CPU-only path.
 * @param quality      Render quality level: 0 = Low, 1 = Medium, 2 = High.
 */
void opendefocus_deep_render(const DeepSampleData* data,
                             float*                output_rgba,
                             uint32_t              width,
                             uint32_t              height,
                             const LensParams*     lens,
                             int                   use_gpu,
                             int                   quality);

/**
 * Render deep-defocus with holdout transmittance attenuation.
 *
 * Identical to opendefocus_deep_render but applies per-pixel holdout
 * transmittance from the holdout map to attenuate gathered samples, enabling
 * depth-correct compositing with holdout mattes.
 *
 * @param data         Valid pointer to DeepSampleData (same as opendefocus_deep_render).
 * @param output_rgba  Output buffer; at least width * height * 4 floats.
 * @param width        Image width in pixels.
 * @param height       Image height in pixels.
 * @param lens         Lens parameters for CoC computation; must be non-null.
 * @param use_gpu      Non-zero to attempt GPU backend; 0 forces CPU-only path.
 * @param holdout      Holdout transmittance map; may be null (equivalent to no holdout).
 *                     When non-null, holdout->len must be width*height*4 or 0.
 * @param quality      Render quality level: 0 = Low, 1 = Medium, 2 = High.
 */
void opendefocus_deep_render_holdout(const DeepSampleData* data,
                                     float*                output_rgba,
                                     uint32_t              width,
                                     uint32_t              height,
                                     const LensParams*     lens,
                                     int                   use_gpu,
                                     const HoldoutData*    holdout,
                                     int                   quality);

#ifdef __cplusplus
}
#endif
