use glam::{UVec2, Vec2, Vec4};

use image::{GenericImageView, ImageBuffer, Pixel, imageops::interpolate_bilinear};

use crate::math::mix_vec;

#[derive(PartialEq)]
pub enum Interpolation {
    Nearest,
    Linear,
}
pub struct Sampler {
    interpolation: Interpolation,
}

impl Sampler {
    pub fn new(interpolation: Interpolation) -> Self {
        Self { interpolation }
    }
}

/// Converts a pixel to a Vec4, handling pixels with fewer than 4 channels.
///
/// This function converts any pixel type with f32 subpixels to a Vec4.
/// For pixels with fewer than 4 channels, it repeats the last available channel
/// to fill the remaining components. This is useful for converting RGB pixels
/// to RGBA by duplicating the last channel as alpha.
///
/// # Performance
/// - Accesses pixel channels only once
/// - Pre-calculates the channel clamping value
/// - Uses array indexing for better compiler optimization
///
/// # Safety
/// Handles edge cases where CHANNEL_COUNT might be 0 or very small
fn pixel_to_vec4<P: Pixel<Subpixel = f32>>(pixel: &P) -> Vec4 {
    let pixel_channels = pixel.channels();

    let max_channel = (P::CHANNEL_COUNT as usize).saturating_sub(1);

    Vec4::new(
        pixel_channels[0.min(max_channel)],
        pixel_channels[1.min(max_channel)],
        pixel_channels[2.min(max_channel)],
        pixel_channels[3.min(max_channel)],
    )
}

pub struct CPUImage<P>
where
    P: Pixel<Subpixel = f32>,
{
    images: Vec<ImageBuffer<P, Vec<f32>>>,
}

impl<P> CPUImage<P>
where
    P: Pixel<Subpixel = f32>,
{
    pub fn new(data: &[ImageBuffer<P, Vec<f32>>]) -> Self {
        Self {
            images: data.to_vec(),
        }
    }
    fn bilinear(&self, coordinates: Vec2, mip: usize) -> Vec4 {
        let image = &self.images[mip.clamp(0, self.images.len() - 1)];
        let (width, height) = image.dimensions();
        let resolution = UVec2::new(width, height);
        if coordinates.x.ceil() as u32 >= resolution.x
            || coordinates.y.ceil() as u32 >= resolution.y
        {
            return Vec4::default();
        }

        match interpolate_bilinear(image, coordinates.x, coordinates.y) {
            Some(pixel) => pixel_to_vec4(&pixel),
            None => Vec4::default(),
        }
    }

    pub fn load_single_mip(&self, coordinates: Vec2, sampler: &Sampler, mip: usize) -> Vec4 {
        let image = &self.images[mip.clamp(0, self.images.len() - 1)];
        let (width, height) = image.dimensions();
        let resolution = Vec2::new(width as f32, height as f32);
        let converted_coordinates = (coordinates * resolution).clamp(Vec2::ZERO, resolution - 1.0);
        if sampler.interpolation == Interpolation::Nearest {
            let pixel = unsafe {
                image.unsafe_get_pixel(
                    converted_coordinates.x as u32,
                    converted_coordinates.y as u32,
                )
            };
            pixel_to_vec4(&pixel)
        } else {
            self.bilinear(converted_coordinates, mip)
        }
    }

    pub fn load_texture(&self, coordinates: Vec2, sampler: &Sampler, mip: f32) -> Vec4 {
        let multiple_images = mip.round() as usize == mip as usize;
        if !multiple_images {
            return self.load_single_mip(coordinates, sampler, mip as usize);
        }

        let bottom = mip.floor();
        let top = mip.ceil();
        let fract = mip - bottom;

        let bottom_pixel = self.load_single_mip(coordinates, sampler, bottom as usize);
        let top_pixel = self.load_single_mip(coordinates, sampler, top as usize);

        mix_vec(bottom_pixel, top_pixel, fract)
    }
}
