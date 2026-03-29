use glam::{Vec2, Vec4};
#[cfg(not(any(target_arch = "spirv")))]
use image::Rgba;
#[cfg(not(any(target_arch = "spirv")))]
use opendefocus_shared::cpu_image::{CPUImage, Sampler};
use opendefocus_shared::math;
use opendefocus_shared::*;
#[cfg(target_arch = "spirv")]
use spirv_std::{Sampler, image::Image2d};

struct SamplePosition {
    pub scale: Vec2,
    pub mip: f32,
}

use crate::util::image::load_texture;

/// Get the kernel value by provided distance for all channels.
///
/// This samples the kernel value for based on the distance to the
/// x and y axis to the center of the kernel.
///
/// * depth_sample: calculated CoC size
/// * x_distance: distance to the center of the kernel horizontally
/// * y_distance: distance to the center of the kernel vertically
///
/// Returns: calculated kernel value
pub fn get_kernel_by_distance(
    #[cfg(not(any(target_arch = "spirv")))] filter: &CPUImage<Rgba<f32>>,
    #[cfg(target_arch = "spirv")] filter: &Image2d,
    depth_sample: f32,
    distance: Vec2,
    settings: &ConvolveSettings,
    #[cfg(not(any(target_arch = "spirv")))] bilinear_sampler: &Sampler,
    #[cfg(target_arch = "spirv")] bilinear_sampler: &Sampler,
) -> Vec4 {
    let sample_position = get_mip_and_scale(depth_sample, distance, settings);
    if math::powf((sample_position.scale.x - 0.5) * 2.0, 2.0)
        + math::powf((sample_position.scale.y - 0.5) * 2.0, 2.0)
        >= 1.0
    {
        return Vec4::ZERO;
    }

    if GlobalFlags::from_bits_retain(settings.flags).contains(GlobalFlags::SIMPLE_DISC) {
        return Vec4::ONE;
    }
    load_texture(
        filter,
        sample_position.scale,
        bilinear_sampler,
        sample_position.mip,
    )
}

pub fn get_kernel_by_distance_multi_channel(
    #[cfg(not(any(target_arch = "spirv")))] filter: &CPUImage<Rgba<f32>>,
    #[cfg(target_arch = "spirv")] filter: &Image2d,
    depth_sample: Vec4,
    distance: Vec2,
    settings: &ConvolveSettings,
    #[cfg(not(any(target_arch = "spirv")))] bilinear_sampler: &Sampler,
    #[cfg(target_arch = "spirv")] bilinear_sampler: &Sampler,
) -> Vec4 {
    Vec4::new(
        get_kernel_by_distance(filter, depth_sample.x, distance, settings, bilinear_sampler).x,
        get_kernel_by_distance(filter, depth_sample.y, distance, settings, bilinear_sampler).y,
        get_kernel_by_distance(filter, depth_sample.z, distance, settings, bilinear_sampler).z,
        get_kernel_by_distance(filter, depth_sample.w, distance, settings, bilinear_sampler).w,
    )
}

fn get_mip_and_scale(
    depth_sample: f32,
    distance: Vec2,
    settings: &ConvolveSettings,
) -> SamplePosition {
    let depth_diameter = (depth_sample.abs() * 2.0).max(1.0);
    // As CoC is radius, we need the diameter
    let pixel_increment = settings.filter_resolution.as_vec2() / depth_diameter;

    let mut coordinates =
        (-distance * pixel_increment) + settings.filter_resolution.as_vec2() * 0.5;
    coordinates = if_else!(
        depth_sample > 0.0
            && GlobalFlags::from_bits_retain(settings.flags)
                .contains(GlobalFlags::INVERSE_FOREGROUND_BOKEH_SHAPE),
        coordinates - 1.0,
        coordinates
    );

    let scaling_factor = depth_diameter / settings.filter_resolution.as_vec2();
    let max_scale = scaling_factor.max_element();

    let mip = 0.0_f32.max(math::log2f(1.0 / max_scale));
    let scale = coordinates / settings.filter_resolution.as_vec2();
    SamplePosition { scale, mip }
}
