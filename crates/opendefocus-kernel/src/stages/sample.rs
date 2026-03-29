use glam::{UVec2, Vec2, Vec4};
#[cfg(not(any(target_arch = "spirv")))]
use image::{LumaA, Rgba};
use opendefocus_shared::*;
#[cfg(not(any(target_arch = "spirv")))]
use opendefocus_shared::cpu_image::{CPUImage, Sampler};
use opendefocus_shared::{
    math::{get_real_coordinates, get_sample_weight, smoothstep},
};
#[cfg(target_arch = "spirv")]
use spirv_std::{Sampler, image::Image2d};

use crate::{
    datamodel::NonUniformValue,
    stages::{
        axial_aberration::NonUniformMapRenderer,
        kernel::{get_kernel_by_distance, get_kernel_by_distance_multi_channel},
        non_uniform::get_non_uniform,
    },
    util::image::{bilinear_depth_based, load_single_texture, load_texture},
};

#[derive(Default)]
/// Data object to store retrieved sample value
pub struct Sample {
    /// Color of sampled pixel multiplied by the kernel
    pub color: Vec4,
    /// Kernel (filter) only value
    pub kernel: Vec4,
    pub weight: f32,
    pub alpha: f32,
    pub deep: f32,
    pub alpha_masked: f32,
    pub coc: f32,
}
impl Sample {
    pub fn new(
        color: Vec4,
        kernel: Vec4,
        weight: f32,
        alpha: f32,
        alpha_masked: f32,
        deep: f32,
        coc: f32,
    ) -> Self {
        Self {
            color: color * weight,
            kernel: kernel * weight,
            weight,
            alpha: alpha * weight,
            alpha_masked: alpha_masked * weight,
            deep: deep * weight,
            coc,
        }
    }
}

pub struct SampleResult {
    pub uses_inpaint: bool,
    pub background_sample: f32,
    pub foreground_sample: f32,
}

impl SampleResult {
    pub fn new(uses_inpaint: bool, background_sample: f32, foreground_sample: f32) -> Self {
        Self {
            uses_inpaint,
            background_sample,
            foreground_sample,
        }
    }
}

/// Get the sample from the correct mip map
///
/// * coordinates: position to sample from
/// * foreground: if sample is retrieved for foreground
/// * center_size: coc of current pixel
///
/// Returns: coc size
pub fn get_coc_sample(
    #[cfg(target_arch = "spirv")] input_depth: &Image2d,
    #[cfg(not(any(target_arch = "spirv")))] input_depth: &CPUImage<LumaA<f32>>,
    settings: &ConvolveSettings,
    scale: Vec2,
    center_size: f32,
    #[cfg(target_arch = "spirv")] nearest_sampler: &Sampler,
    #[cfg(not(any(target_arch = "spirv")))] nearest_sampler: &Sampler,
) -> SampleResult {
    if GlobalFlags::from_bits_retain(settings.flags).contains(GlobalFlags::IS_2D) {
        return SampleResult::new(false, -settings.max_size, 0.0);
    }

    let current_sample = load_texture(input_depth, scale, nearest_sampler, 0.0);
    let sample = bilinear_depth_based(
        input_depth,
        input_depth,
        settings,
        nearest_sampler,
        scale,
        current_sample.x,
        0.0,
    );
    let foreground_sample: f32 = if_else!(
        sample.x < 0.0,
        if_else!(center_size > 0.0, -center_size, -settings.max_size),
        sample.x
    );

    if center_size >= 0.0 {
        return SampleResult::new(true, sample.y, foreground_sample);
    }

    SampleResult::new(false, sample.x, foreground_sample)
}

fn get_color_sample(
    #[cfg(not(any(target_arch = "spirv")))] input_image: &CPUImage<Rgba<f32>>,
    #[cfg(target_arch = "spirv")] input_image: &Image2d,
    #[cfg(not(any(target_arch = "spirv")))] inpaint_image: &CPUImage<Rgba<f32>>,
    #[cfg(target_arch = "spirv")] inpaint_image: &Image2d,
    #[cfg(not(any(target_arch = "spirv")))] input_depth: &CPUImage<LumaA<f32>>,
    #[cfg(target_arch = "spirv")] input_depth: &Image2d,
    settings: &ConvolveSettings,
    coordinates: Vec2,
    uses_inpaint: bool,
    coc: f32,
    #[cfg(not(any(target_arch = "spirv")))] bilinear_sampler: &Sampler,
    #[cfg(target_arch = "spirv")] bilinear_sampler: &Sampler,
    #[cfg(not(any(target_arch = "spirv")))] nearest_sampler: &Sampler,
    #[cfg(target_arch = "spirv")] nearest_sampler: &Sampler,
) -> Vec4 {
    if GlobalFlags::from_bits_retain(settings.flags).contains(GlobalFlags::IS_2D) {
        return load_single_texture(input_image, coordinates, bilinear_sampler);
    }
    if uses_inpaint {
        return bilinear_depth_based(
            inpaint_image,
            input_depth,
            settings,
            nearest_sampler,
            coordinates,
            coc,
            0.0,
        );
    }
    bilinear_depth_based(
        input_image,
        input_depth,
        settings,
        nearest_sampler,
        coordinates,
        coc,
        0.0,
    )
}

pub fn get_alpha_map(settings: &ConvolveSettings, center_size: f32) -> f32 {
    smoothstep(
        0.0,
        1.0,
        ((center_size * (settings.render_scale as f32)) * 0.75).abs(),
    )
}

fn get_kernel(
    #[cfg(not(any(target_arch = "spirv")))] filter: &CPUImage<Rgba<f32>>,
    #[cfg(target_arch = "spirv")] filter: &Image2d,
    settings: &ConvolveSettings,
    sample_size: f32,
    distance: Vec2,
    bilinear_sampler: &Sampler,
    non_uniform: NonUniformValue,
) -> Vec4 {
    (if !GlobalFlags::from_bits_retain(settings.flags)
        .contains(GlobalFlags::AXIAL_ABERRATION_ENABLE)
    {
        get_kernel_by_distance(
            filter,
            sample_size * non_uniform.scale,
            non_uniform.offset + distance,
            settings,
            bilinear_sampler,
        )
    } else {
        let sample_size_multichannel =
            NonUniformMapRenderer::new(settings.get_axial_aberration_settings())
                .calculate_coc(sample_size * non_uniform.scale);
        get_kernel_by_distance_multi_channel(
            filter,
            sample_size_multichannel,
            non_uniform.offset + distance,
            settings,
            bilinear_sampler,
        )
    }) * non_uniform.opacity
}

/// Calculate a single convolve sample
///
/// Calculates the actual value of the bokeh for a specific pixel
/// based on the CoC and distance to the center
pub fn calculate_sample(
    #[cfg(not(any(target_arch = "spirv")))] image: &CPUImage<Rgba<f32>>,
    #[cfg(target_arch = "spirv")] image: &Image2d,
    #[cfg(not(any(target_arch = "spirv")))] inpaint: &CPUImage<Rgba<f32>>,
    #[cfg(target_arch = "spirv")] inpaint: &Image2d,
    #[cfg(not(any(target_arch = "spirv")))] filter: &CPUImage<Rgba<f32>>,
    #[cfg(target_arch = "spirv")] filter: &Image2d,
    #[cfg(not(any(target_arch = "spirv")))] depth: &CPUImage<LumaA<f32>>,
    #[cfg(target_arch = "spirv")] depth: &Image2d,
    cached_samples: &[f32],
    settings: &ConvolveSettings,
    sample_size: f32,
    calculated_sample_size: f32,
    coordinates: Vec2,
    distance: Vec2,
    position: UVec2,
    coverage_weight: f32,
    uses_inpaint: bool,
    foreground: bool,
    #[cfg(not(any(target_arch = "spirv")))] bilinear_sampler: &Sampler,
    #[cfg(target_arch = "spirv")] bilinear_sampler: &Sampler,
    #[cfg(not(any(target_arch = "spirv")))] nearest_sampler: &Sampler,
    #[cfg(target_arch = "spirv")] nearest_sampler: &Sampler,
) -> Sample {
    let mut local_distance = distance;
    let color = if_else!(
        foreground && sample_size <= 0.0,
        Vec4::ZERO,
        get_color_sample(
            image,
            inpaint,
            depth,
            settings,
            coordinates,
            uses_inpaint,
            sample_size,
            bilinear_sampler,
            nearest_sampler,
        )
    );
    local_distance.x *= if_else!(settings.pixel_aspect > 1.0, settings.pixel_aspect, 1.0);
    local_distance.y *= if_else!(
        settings.pixel_aspect < 1.0,
        settings.pixel_aspect_normalizer,
        1.0
    );

    let non_uniform =
        if GlobalFlags::from_bits_retain(settings.flags).contains(GlobalFlags::USE_NON_UNIFORM) && sample_size != 0.0{
            let real_position = get_real_coordinates(settings, position);
            get_non_uniform(
                settings,
                local_distance,
                real_position,
                sample_size,
            )
        } else {
            NonUniformValue::default()
        };

    let alpha_map = if_else!(
        uses_inpaint,
        1.0,
        get_alpha_map(settings, calculated_sample_size)
    );

    let filter_kernel = get_kernel(
        filter,
        settings,
        sample_size,
        distance,
        bilinear_sampler,
        non_uniform,
    );

    let weight = get_sample_weight(cached_samples, calculated_sample_size) / coverage_weight;

    Sample::new(
        color * filter_kernel * alpha_map,
        filter_kernel,
        weight,
        if sample_size >= 0.0 || !foreground {
            filter_kernel.z
        } else {
            0.0
        },
        if sample_size >= 0.0 || !foreground {
            alpha_map * filter_kernel.z
        } else {
            0.0
        },
        color.w * filter_kernel.w * alpha_map,
        calculated_sample_size,
    )
}
