use glam::{UVec2, Vec4};
#[cfg(not(any(target_arch = "spirv")))]
use image::{LumaA, Rgba};
#[cfg(not(any(target_arch = "spirv")))]
use opendefocus_shared::cpu_image::{CPUImage, Sampler};
use opendefocus_shared::{ConvolveSettings, GlobalFlags, math};
#[cfg(target_arch = "spirv")]
use spirv_std::{Sampler, image::Image2d};

use crate::stages::sample::{Sample, calculate_sample, get_coc_sample};

const PLANE_SEPARATION: f32 = 0.0;
/// Data object to store ring calculations
#[derive(Default)]
pub struct Ring {
    /// Color of sampled pixels multiplied by the kernels
    pub color: Vec4,
    /// Kernels (filters) only value
    pub kernel: Vec4,
    /// Alpha retrieved from deep input. Used for merging at a later state.
    pub count: u32,
    /// Total count of found samples
    pub found: i32,
    /// Calculated weight
    pub weight: f32,
    /// Radius in pixels of the current ring samples
    pub radius: f32,
    /// Alpha value to multiply eventually with
    pub alpha: f32,
    /// Alpha value used to generate the mask
    pub alpha_masked: f32,
    /// Circle of Confusion
    pub coc: f32,
    pub deep: f32,
}
impl Ring {
    pub fn add_sample(&mut self, sample: Sample) {
        self.color += sample.color;
        self.kernel += sample.kernel;
        self.alpha_masked += sample.alpha_masked;
        self.alpha += sample.alpha;
        self.weight += sample.weight;
        self.found += 1;
        self.coc += sample.coc;
        self.deep += sample.deep;
    }

    pub fn normalize(&mut self) {
        if self.kernel.z != 0.0 {
            self.color /= self.kernel;
            self.alpha /= self.kernel.z;
            self.alpha_masked /= self.kernel.z;
            self.deep /= self.kernel.z;
        }
    }
}

/// Get the radius for a provided ring id
///
/// * id: id of ring to get radius for
///
/// Returns: radius float value
fn get_radius_for_ring_id(id: u32, settings: &ConvolveSettings) -> f32 {
    (id as f32 / settings.samples as f32 * settings.samples as f32 * settings.ring_distance)
        .max(0.00001)
}

pub struct Rings {
    pub foreground: Ring,
    pub background: Ring,
}

impl Rings {
    pub fn new(total_points: u32, radius: f32) -> Self {
        let mut foreground = Ring::default();
        let mut background = Ring::default();

        foreground.count = total_points;
        foreground.radius = radius;
        background.count = total_points;
        background.radius = radius;
        Self {
            foreground,
            background,
        }
    }
    pub fn merge(&mut self, cached_samples: &[f32], other: Rings) {
        self.merge_foreground_rings(other.foreground);
        self.merge_background_rings(cached_samples, other.background)
    }

    fn merge_foreground_rings(&mut self, other: Ring) {
        self.foreground.color += other.color;
        self.foreground.kernel += other.kernel;
        self.foreground.alpha += other.alpha;
        self.foreground.alpha_masked += other.alpha_masked;
        self.foreground.radius = other.radius;
        self.foreground.found = other.found;
        self.foreground.count = other.count;
        self.foreground.deep += other.deep;
    }
    fn merge_background_rings(&mut self, cached_samples: &[f32], other: Ring) {
        let previous_weight = math::get_sample_weight(cached_samples, self.background.coc)
            * self.background.found as f32;
        let current_weight =
            math::get_sample_weight(cached_samples, other.coc) * other.found as f32;
        let background_ring_radius = self.background.radius * previous_weight;
        let foreground_ring_radius = other.radius * current_weight;

        let mut occlusion = if foreground_ring_radius == 0.0 {
            0.0
        } else {
            (-((background_ring_radius).abs() - (foreground_ring_radius).abs())
                / (foreground_ring_radius).abs())
            .clamp(0.0, 1.0)
        };

        occlusion = 1.0 - math::smoothstep(0.0, 1.0, occlusion);
        self.background.color = other.color + self.background.color * occlusion;
        self.background.kernel = other.kernel + self.background.kernel * occlusion;

        self.background.alpha = other.alpha + self.background.alpha * occlusion;
        self.background.deep = other.deep + self.background.deep * occlusion;
        self.background.alpha_masked =
            other.alpha_masked + self.background.alpha_masked * occlusion;

        self.background.radius = other.radius;
        self.background.found = other.found;
        self.background.count = other.count;
        self.background.coc = other.coc;
    }
}

/// Apply depth-correct holdout transmittance attenuation to a sample.
///
/// The holdout slice encodes `width * height * 4` f32 values where each group of
/// 4 floats is `(depth0, T0, depth1, T1)` — two depth/T breakpoints per output pixel.
/// T is cumulative transmittance through the holdout at that depth.
/// If the sample's CoC depth exceeds `depth0`, apply `T0`; if it also exceeds `depth1`,
/// apply `T1`. Default T = 1.0 (no attenuation) when holdout is empty or pixel is
/// out of bounds.
#[cfg(not(any(target_arch = "spirv")))]
fn apply_holdout_attenuation(sample: &mut Sample, holdout: &[f32], output_width: u32, position: UVec2) {
    if holdout.is_empty() {
        return;
    }
    let pixel_idx = (position.y * output_width + position.x) as usize;
    let base = pixel_idx * 4;
    if holdout.len() < base + 4 {
        return;
    }
    let d0 = holdout[base];
    let t0 = holdout[base + 1];
    let d1 = holdout[base + 2];
    let t1 = holdout[base + 3];
    let sample_depth = sample.coc;
    let t: f32 = if sample_depth > d1 {
        t1
    } else if sample_depth > d0 {
        t0
    } else {
        1.0
    };
    sample.color *= t;
    sample.alpha *= t;
    sample.alpha_masked *= t;
}

pub fn calculate_ring(
    settings: &ConvolveSettings,
    #[cfg(not(any(target_arch = "spirv")))] image: &CPUImage<Rgba<f32>>,
    #[cfg(target_arch = "spirv")] image: &Image2d,
    #[cfg(not(any(target_arch = "spirv")))] filter: &CPUImage<Rgba<f32>>,
    #[cfg(target_arch = "spirv")] filter: &Image2d,
    #[cfg(not(any(target_arch = "spirv")))] inpaint: &CPUImage<Rgba<f32>>,
    #[cfg(target_arch = "spirv")] inpaint: &Image2d,
    #[cfg(not(any(target_arch = "spirv")))] depth: &CPUImage<LumaA<f32>>,
    #[cfg(target_arch = "spirv")] depth: &Image2d,
    cached_samples: &[f32],
    #[cfg(not(any(target_arch = "spirv")))] holdout: &[f32],
    #[cfg(not(any(target_arch = "spirv")))] output_width: u32,
    ring_id: u32,
    center_size: f32,
    position: UVec2,
    #[cfg(not(any(target_arch = "spirv")))] bilinear_sampler: &Sampler,
    #[cfg(not(any(target_arch = "spirv")))] nearest_sampler: &Sampler,
    #[cfg(target_arch = "spirv")] bilinear_sampler: &Sampler,
    #[cfg(target_arch = "spirv")] nearest_sampler: &Sampler,
) -> Rings {
    let radius = get_radius_for_ring_id(ring_id, settings);
    let total_points = math::get_points_for_ring(ring_id, settings.samples, true);
    let base_points = math::get_points_for_ring(ring_id, settings.samples, false);
    let degree_per_point = 360.0 / total_points as f32;
    let mut rings = Rings::new(total_points, radius);
    let coverage_weight =
        math::calculate_normalized_coverage_weight(radius, base_points, total_points);

    let resolution = settings.get_image_resolution().as_vec2();
    for i in 0..total_points {
        let angle = degree_per_point * i as f32;
        let mut coordinates = math::get_coordinates_on_circle(angle, radius);
        if settings.pixel_aspect > 1.0 {
            coordinates.x *= 1.0 * settings.pixel_aspect_normalizer;
        } else {
            coordinates.y *= settings.pixel_aspect;
        }

        if settings.filter_aspect_ratio < 1.0 {
            coordinates.x *= settings.filter_aspect_ratio;
        } else {
            coordinates.y *= settings.filter_aspect_ratio_normalizer;
        }

        let distance = coordinates;

        coordinates += position.as_vec2();
        coordinates /= resolution;

        let sample_result =
            get_coc_sample(depth, settings, coordinates, center_size, nearest_sampler);

        if !GlobalFlags::from_bits_retain(settings.flags).contains(GlobalFlags::IS_2D) {
            let mut fg_sample = calculate_sample(
                image,
                inpaint,
                filter,
                depth,
                cached_samples,
                settings,
                sample_result.foreground_sample,
                sample_result.foreground_sample.abs(),
                coordinates,
                distance,
                position,
                coverage_weight,
                false,
                true,
                bilinear_sampler,
                nearest_sampler,
            );
            #[cfg(not(any(target_arch = "spirv")))]
            apply_holdout_attenuation(&mut fg_sample, holdout, output_width, position);
            rings.foreground.add_sample(fg_sample);
        }

        let straight_distance = distance.length_squared();
        let calculated_sample_size = sample_result.background_sample.abs();

        if sample_result.background_sample < PLANE_SEPARATION
            && straight_distance <= math::powf(math::ceilf(calculated_sample_size), 2.0)
        {
            let mut sample = calculate_sample(
                image,
                inpaint,
                filter,
                depth,
                cached_samples,
                settings,
                sample_result.background_sample,
                calculated_sample_size,
                coordinates,
                distance,
                position,
                coverage_weight,
                sample_result.uses_inpaint,
                false,
                bilinear_sampler,
                nearest_sampler,
            );
            #[cfg(not(any(target_arch = "spirv")))]
            apply_holdout_attenuation(&mut sample, holdout, output_width, position);
            rings.background.add_sample(sample);
        }
    }
    if rings.background.found != 0 {
        rings.background.coc /= rings.background.found as f32;
    }
    rings
}

#[cfg(test)]
mod tests {
    use super::*;
    use glam::Vec4;

    fn make_sample(coc: f32, color: Vec4) -> Sample {
        Sample {
            color,
            kernel: Vec4::ONE,
            weight: 1.0,
            alpha: 1.0,
            deep: 0.5,
            alpha_masked: 0.8,
            coc,
        }
    }

    #[test]
    fn test_rings_new() {
        let total_points = 10;
        let radius = 5.0;
        let rings: Rings = Rings::new(total_points, radius);

        assert_eq!(rings.foreground.count, total_points);
        assert_eq!(rings.foreground.radius, radius);
        assert_eq!(rings.background.count, total_points);
        assert_eq!(rings.background.radius, radius);
    }

    /// Holdout slice format: [d0, T0, d1, T1] per pixel.
    /// When sample.coc <= d0: T = 1.0 (no attenuation).
    /// When sample.coc in (d0, d1]: T = T0.
    /// When sample.coc > d1: T = T1.
    #[test]
    fn test_holdout_no_attenuation_below_d0() {
        // d0=2.0, T0=0.5, d1=4.0, T1=0.2; sample coc=1.0 < d0 → T=1.0
        let holdout: Vec<f32> = vec![2.0, 0.5, 4.0, 0.2];
        let mut sample = make_sample(1.0, Vec4::new(1.0, 1.0, 1.0, 1.0));
        let position = UVec2::new(0, 0);
        apply_holdout_attenuation(&mut sample, &holdout, 1, position);
        assert!((sample.color.x - 1.0).abs() < 1e-5, "expected T=1.0, got color.x={}", sample.color.x);
        assert!((sample.alpha - 1.0).abs() < 1e-5);
        assert!((sample.alpha_masked - 0.8).abs() < 1e-5);
    }

    #[test]
    fn test_holdout_attenuation_between_d0_and_d1() {
        // d0=2.0, T0=0.5, d1=4.0, T1=0.2; sample coc=3.0 > d0 but <= d1 → T=0.5
        let holdout: Vec<f32> = vec![2.0, 0.5, 4.0, 0.2];
        let mut sample = make_sample(3.0, Vec4::new(1.0, 1.0, 1.0, 1.0));
        let position = UVec2::new(0, 0);
        apply_holdout_attenuation(&mut sample, &holdout, 1, position);
        assert!((sample.color.x - 0.5).abs() < 1e-5, "expected T=0.5, got color.x={}", sample.color.x);
        assert!((sample.alpha - 0.5).abs() < 1e-5);
        assert!((sample.alpha_masked - 0.4).abs() < 1e-5);
    }

    #[test]
    fn test_holdout_attenuation_beyond_d1() {
        // d0=2.0, T0=0.5, d1=4.0, T1=0.2; sample coc=5.0 > d1 → T=0.2
        let holdout: Vec<f32> = vec![2.0, 0.5, 4.0, 0.2];
        let mut sample = make_sample(5.0, Vec4::new(1.0, 1.0, 1.0, 1.0));
        let position = UVec2::new(0, 0);
        apply_holdout_attenuation(&mut sample, &holdout, 1, position);
        assert!((sample.color.x - 0.2).abs() < 1e-5, "expected T=0.2, got color.x={}", sample.color.x);
        assert!((sample.alpha - 0.2).abs() < 1e-5);
        assert!((sample.alpha_masked - 0.16).abs() < 1e-5);
    }

    #[test]
    fn test_holdout_empty_slice_no_attenuation() {
        // Empty holdout → T=1.0 always
        let holdout: Vec<f32> = vec![];
        let mut sample = make_sample(10.0, Vec4::new(1.0, 1.0, 1.0, 1.0));
        let position = UVec2::new(0, 0);
        apply_holdout_attenuation(&mut sample, &holdout, 1, position);
        assert!((sample.color.x - 1.0).abs() < 1e-5);
        assert!((sample.alpha - 1.0).abs() < 1e-5);
    }

    #[test]
    fn test_holdout_out_of_bounds_pixel_no_attenuation() {
        // Holdout only covers pixel 0, position is pixel 2 — out of bounds → T=1.0
        let holdout: Vec<f32> = vec![2.0, 0.5, 4.0, 0.2]; // only pixel 0
        let mut sample = make_sample(5.0, Vec4::new(1.0, 1.0, 1.0, 1.0));
        let position = UVec2::new(2, 0); // pixel_idx=2, base=8, holdout.len()=4 < 12
        apply_holdout_attenuation(&mut sample, &holdout, 4, position);
        assert!((sample.color.x - 1.0).abs() < 1e-5, "out-of-bounds pixel should have T=1.0");
    }

    #[test]
    fn test_holdout_multi_pixel_correct_lookup() {
        // width=2, pixel (1,0) is at index 1; holdout = [p0_d0, p0_T0, p0_d1, p0_T1, p1_d0, p1_T0, p1_d1, p1_T1]
        let holdout: Vec<f32> = vec![
            100.0, 0.1, 200.0, 0.05, // pixel 0 — high depth breakpoints
            1.0,   0.7,   3.0,  0.3, // pixel 1 — lower depth breakpoints
        ];
        let mut sample = make_sample(2.0, Vec4::new(1.0, 1.0, 1.0, 1.0));
        let position = UVec2::new(1, 0); // pixel_idx=1
        apply_holdout_attenuation(&mut sample, &holdout, 2, position);
        // coc=2.0 > d0=1.0, <= d1=3.0 → T=0.7
        assert!((sample.color.x - 0.7).abs() < 1e-5, "pixel1 T should be 0.7, got {}", sample.color.x);
    }

    #[test]
    fn test_holdout_kernel_and_weight_unchanged() {
        // Verify kernel and weight fields are NOT modified by attenuation
        let holdout: Vec<f32> = vec![1.0, 0.3, 3.0, 0.1];
        let mut sample = make_sample(5.0, Vec4::new(1.0, 1.0, 1.0, 1.0));
        sample.kernel = Vec4::new(2.0, 3.0, 4.0, 5.0);
        sample.weight = 0.75;
        let position = UVec2::new(0, 0);
        apply_holdout_attenuation(&mut sample, &holdout, 1, position);
        // kernel and weight must be untouched
        assert!((sample.kernel.x - 2.0).abs() < 1e-5, "kernel.x must not be modified");
        assert!((sample.weight - 0.75).abs() < 1e-5, "weight must not be modified");
        // but color must be attenuated
        assert!((sample.color.x - 0.1).abs() < 1e-5, "color.x should be attenuated by T=0.1");
    }
}
