use glam::{UVec2, Vec2, Vec4};
#[cfg(not(any(target_arch = "spirv")))]
use image::Pixel;
use opendefocus_shared::*;
#[cfg(not(any(target_arch = "spirv")))]
use opendefocus_shared::cpu_image::{CPUImage, Sampler};
use opendefocus_shared::math::{mix, mix_vec};
#[cfg(target_arch = "spirv")]
use spirv_std::{Sampler, image::Image2d};

#[cfg(target_arch = "spirv")]
pub fn load_texture(image: &Image2d, coordinates: Vec2, sampler: &Sampler, lod: f32) -> Vec4 {
    image.sample_by_lod::<f32>(*sampler, coordinates, lod)
}

#[cfg(target_arch = "spirv")]
pub fn load_single_texture(image: &Image2d, coordinates: Vec2, sampler: &Sampler) -> Vec4 {
    image.sample_by_lod::<f32>(*sampler, coordinates, 0.0)
}

#[cfg(not(any(target_arch = "spirv")))]
pub fn load_texture<P: Pixel<Subpixel = f32>>(
    data: &CPUImage<P>,
    coordinates: Vec2,
    sampler: &Sampler,
    lod: f32,
) -> Vec4 {
    data.load_texture(coordinates, sampler, lod)
}

#[cfg(not(any(target_arch = "spirv")))]
pub fn load_single_texture<P: Pixel<Subpixel = f32>>(
    data: &CPUImage<P>,
    coordinates: Vec2,
    sampler: &Sampler,
) -> Vec4 {
    data.load_single_mip(coordinates, sampler, 0)
}

pub fn bilinear_depth_based(
    #[cfg(not(any(target_arch = "spirv")))] image: &CPUImage<impl Pixel<Subpixel = f32>>,
    #[cfg(target_arch = "spirv")] image: &Image2d,
    #[cfg(not(any(target_arch = "spirv")))] depth: &CPUImage<impl Pixel<Subpixel = f32>>,
    #[cfg(target_arch = "spirv")] depth: &Image2d,
    settings: &ConvolveSettings,
    sampler: &Sampler,
    coordinates: Vec2,
    coc: f32,
    lod: f32,
) -> Vec4 {
    let resolution = settings.get_image_resolution().as_vec2();
    let normalized_coordinates = coordinates * resolution;
    let base_coords = normalized_coordinates.floor();
    let fract = normalized_coordinates - base_coords;
    let mut sample = load_texture(image, base_coords / resolution, sampler, lod);

    let x_coords = base_coords + Vec2::new(1.0, 0.0);
    let x1 = load_texture(image, x_coords / resolution, sampler, lod);
    let x1_coc = load_texture(depth, x_coords / resolution, sampler, lod).x;
    let x_diff = clamp!((x1_coc - coc).abs(), 0.0, 1.0);

    sample = mix_vec(sample, x1, fract.x * (1.0 - x_diff)).into();

    let y_coords_base = base_coords + Vec2::new(0.0, 1.0);
    let y0 = load_texture(image, y_coords_base / resolution, sampler, lod);
    let y0_coc = load_texture(depth, y_coords_base / resolution, sampler, lod).x;

    let y_coords_diag = base_coords + Vec2::ONE;
    let y1 = load_texture(image, y_coords_diag / resolution, sampler, lod);
    let y1_coc = load_texture(depth, y_coords_diag / resolution, sampler, lod).x;

    let y_diff = clamp!((y0_coc - y1_coc).abs(), 0.0, 1.0);

    let y_sample = mix_vec(y0, y1, fract.x * (1.0 - y_diff));

    let y_coc = mix(y0_coc, y1_coc, fract.x * (1.0 - y_diff));

    mix_vec(
        sample,
        y_sample,
        fract.y * clamp!(1.0 - (y_coc - coc).abs(), 0.0, 1.0),
    )
}

/// Writes data into the target at the specified coordinates.
///
/// The data is assumed to be in packed format, meaning that the buffer looks like this:
///
/// |       |       |       |       |
/// |-------|-------|-------|-------|
/// | red   | green | blue  | alpha |
/// | 0,0   | 0,0   | 0,0   | 0,0   |
/// | 0,1   | 0,1   | 0,1   | 0,1   |
/// | 1,0   | 1,0   | 1,0   | 1,0   |
/// | 1,1   | 1,1   | 1,1   | 1,1   |
///
/// This function calculates the position in the target array based on the given coordinates and width,
/// and then writes the data slice into that position.
pub fn write_texture(
    target: &mut [f32],
    data: &[f32; OUTPUT_CHANNELS],
    coordinates: UVec2,
    width: u32,
) {
    if target.len() == 5 {
        target[0] = data[0];
        target[1] = data[1];
        target[2] = data[2];
        target[3] = data[3];
        target[4] = data[4];
        return;
    }

    if coordinates.x >= width
        || coordinates.y as usize * width as usize * OUTPUT_CHANNELS
            + coordinates.x as usize * OUTPUT_CHANNELS
            + OUTPUT_CHANNELS
            > target.len()
    {
        return;
    }

    let data_position = coordinates.y as usize * (width as usize * OUTPUT_CHANNELS)
        + coordinates.x as usize * OUTPUT_CHANNELS;

    target[data_position] = data[0]; // spirv compiler doesnt support the copy from slice for dynamic arrays, so manual unrolled loop it is
    target[data_position + 1] = data[1];
    target[data_position + 2] = data[2];
    target[data_position + 3] = data[3];
    target[data_position + 4] = data[4];
}

#[cfg(test)]
mod tests {
    use super::*;
    use rstest::rstest;

    #[rstest]
    #[case([0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0, 12.0, 13.0, 14.0, 15.0, 16.0, 17.0, 18.0, 19.0],
        [0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0, 12.0, 13.0, 14.0, 20.0, 20.0, 20.0, 20.0, 20.0],
        [1, 1],
        [20.0, 20.0, 20.0, 20.0, 20.0],
        2)]
    fn test_write_texture(
        #[case] data: [f32; 20],
        #[case] expected: [f32; 20],
        #[case] coordinates: [u32; 2],
        #[case] test_input: [f32; 5],
        #[case] width: u32,
    ) {
        let mut target = data;
        write_texture(&mut target, &test_input, coordinates.into(), width);

        assert_eq!(target, expected);
    }
}
