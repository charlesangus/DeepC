#![warn(unused_extern_crates)]
#![cfg_attr(target_arch = "spirv", no_std)]

mod datamodel;
mod stages;
mod util;
use glam::UVec2;
#[cfg(not(any(target_arch = "spirv")))]
use image::{LumaA, Rgba};
#[cfg(not(any(target_arch = "spirv")))]
use opendefocus_shared::cpu_image::{CPUImage, Sampler};
use opendefocus_shared::{ConvolveSettings, GlobalFlags, ThreadId, math::get_real_coordinates};

#[cfg(target_arch = "spirv")]
use glam::UVec3;
#[cfg(target_arch = "spirv")]
use spirv_std::{
    Sampler,
    image::{Image, Image2d},
    spirv,
};

use crate::{
    stages::{
        ring::{Rings, calculate_ring},
        sample::get_alpha_map,
    },
    util::image::{load_texture, write_texture},
};

fn skip_overlap(position: UVec2, settings: &ConvolveSettings) -> bool {
    let coordinates = get_real_coordinates(settings, position);
    !((coordinates.x as i32) >= settings.process_region.x
        && (coordinates.x as i32) < settings.process_region.z
        && (coordinates.y as i32) >= settings.process_region.y - 1
        && (coordinates.y as i32) <= settings.process_region.w)
}

#[inline(always)]
pub fn global_entrypoint(
    thread_id: ThreadId,
    output_image: &mut [f32],
    settings: &ConvolveSettings,
    cached_samples: &[f32],
    #[cfg(not(any(target_arch = "spirv")))] holdout: &[f32],
    #[cfg(not(any(target_arch = "spirv")))] input_image: &CPUImage<Rgba<f32>>,
    #[cfg(not(any(target_arch = "spirv")))] inpaint: &CPUImage<Rgba<f32>>,
    #[cfg(not(any(target_arch = "spirv")))] filter: &CPUImage<Rgba<f32>>,
    #[cfg(not(any(target_arch = "spirv")))] depth: &CPUImage<LumaA<f32>>,
    #[cfg(not(any(target_arch = "spirv")))] bilinear_sampler: &Sampler,
    #[cfg(not(any(target_arch = "spirv")))] nearest_sampler: &Sampler,
    #[cfg(target_arch = "spirv")] input_image: &Image2d,
    #[cfg(target_arch = "spirv")] inpaint: &Image2d,
    #[cfg(target_arch = "spirv")] filter: &Image2d,
    #[cfg(target_arch = "spirv")] depth: &Image2d,
    #[cfg(target_arch = "spirv")] bilinear_sampler: &Sampler,
    #[cfg(target_arch = "spirv")] nearest_sampler: &Sampler,
) {
    let coords = thread_id.get_coordinates();
    if coords.x >= settings.full_region.z as u32 || coords.y >= settings.full_region.w as u32 {
        return;
    }
    let resolution = settings.get_image_resolution();
    let coordinates_scale = coords.as_vec2() / settings.get_image_resolution().as_vec2();
    if skip_overlap(coords, settings) {
        return;
    }

    let center_size = if GlobalFlags::from_bits_retain(settings.flags).contains(GlobalFlags::IS_2D)
    {
        settings.max_size
    } else {
        load_texture(depth, coordinates_scale, nearest_sampler, 0.0).x
    };
    let mut main_rings = Rings::new(0, 1.0);

    for ring_id in (0..settings.samples).rev() {
        let current_rings = calculate_ring(
            settings,
            input_image,
            filter,
            inpaint,
            depth,
            cached_samples,
            #[cfg(not(any(target_arch = "spirv")))]
            holdout,
            #[cfg(not(any(target_arch = "spirv")))]
            resolution.x,
            ring_id,
            center_size,
            coords,
            bilinear_sampler,
            nearest_sampler,
        );
        main_rings.merge(cached_samples, current_rings);
    }
    main_rings.background.normalize();

    if GlobalFlags::from_bits_retain(settings.flags).contains(GlobalFlags::IS_2D) {
        let alpha = get_alpha_map(settings, center_size);
        write_texture(
            output_image,
            &[
                main_rings.background.color.x * alpha,
                main_rings.background.color.y * alpha,
                main_rings.background.color.z * alpha,
                main_rings.background.color.w * alpha,
                alpha,
            ],
            coords,
            resolution.x,
        );
        return;
    }
    main_rings.foreground.normalize();

    let mut output = main_rings.background.color;
    output = main_rings.foreground.color + output * (1.0 - main_rings.foreground.alpha);
    let alpha = main_rings.foreground.alpha_masked
        + main_rings.background.alpha_masked * (1.0 - main_rings.foreground.alpha);

    write_texture(
        output_image,
        &[
            output.x,
            output.y,
            output.z,
            output.w,
            alpha.clamp(0.0, 1.0),
        ],
        coords,
        resolution.x,
    );
}

/// GPU entry point for Vulkan/SPIR-V
#[cfg(target_arch = "spirv")]
#[spirv(compute(threads(16, 16)))]
pub fn convolve_kernel_f32(
    #[spirv(global_invocation_id)] gid: UVec3,
    #[spirv(uniform, descriptor_set = 0, binding = 0)] settings: &ConvolveSettings,
    #[spirv(descriptor_set = 0, binding = 1)] bilinear_sampler: &Sampler,
    #[spirv(descriptor_set = 0, binding = 2)] nearest_sampler: &Sampler,
    #[spirv(storage_buffer, descriptor_set = 0, binding = 3)] output_image: &mut [f32],
    #[spirv(descriptor_set = 0, binding = 4)] input_image: &Image!(2D, type=f32, sampled),
    #[spirv(descriptor_set = 0, binding = 5)] filter: &Image!(2D, type=f32, sampled),
    #[spirv(descriptor_set = 0, binding = 6)] inpaint_image: &Image!(2D, type=f32, sampled),
    #[spirv(descriptor_set = 0, binding = 7)] depth: &Image!(2D, type=f32, sampled),
    #[spirv(storage_buffer, descriptor_set = 0, binding = 8)] cached_sample_weights: &[f32],
) {
    let thread_id = ThreadId::new(gid.x, gid.y);
    global_entrypoint(
        thread_id,
        output_image,
        settings,
        cached_sample_weights,
        input_image,
        inpaint_image,
        filter,
        depth,
        bilinear_sampler,
        nearest_sampler,
    );
}
