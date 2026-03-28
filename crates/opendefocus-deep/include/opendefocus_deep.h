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
 */
void opendefocus_deep_render(const DeepSampleData* data,
                             float*                output_rgba,
                             uint32_t              width,
                             uint32_t              height,
                             const LensParams*     lens,
                             int                   use_gpu);

#ifdef __cplusplus
}
#endif
