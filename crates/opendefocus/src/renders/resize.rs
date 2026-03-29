use crate::error::Error;
use crate::error::Result;
use crate::traits::TraitBounds;
use glam::{USizeVec2, Vec2};
use ndarray::s;
use ndarray::{Array3, ArrayView3};
use resize::Pixel::RGBF32;
use resize::Pixel::{GrayF32, RGBAF32};
use resize::Type;
use rgb::*;
pub fn resize_by_scale<T: TraitBounds>(
    src: ArrayView3<T>,
    scale: f32,
    filter: Type,
) -> Result<Array3<T>> {
    let target_resolution = get_resolution(src.view(), scale);
    let resized_image = resize_to_resolution(src.view(), target_resolution, filter)?;
    Ok(resized_image)
}

fn get_resolution<T>(src: ArrayView3<T>, scale: f32) -> USizeVec2 {
    (USizeVec2::new(src.dim().1, src.dim().0).as_vec2() * scale)
        .max(Vec2::new(1.0, 1.0))
        .round()
        .as_usizevec2()
}

pub fn resize_to_resolution<T: TraitBounds>(
    src: ArrayView3<T>,
    resolution: USizeVec2,
    filter: Type,
) -> Result<Array3<T>> {
    let channels = src.dim().2;
    let mut target = Array3::zeros((resolution.y, resolution.x, channels));
    let internal_source: Array3<f32> = src.mapv(|f| f.to_f32_normalized().unwrap_or_default());

    match channels {
        4 => {
            let mut resizer = resize::new(
                src.dim().1,
                src.dim().0,
                resolution.x,
                resolution.y,
                RGBAF32,
                filter,
            )?;
            resizer.resize(
                internal_source
                    .as_slice()
                    .ok_or(Error::SliceRetrievalFailed)?
                    .as_rgba(),
                target
                    .as_slice_mut()
                    .ok_or(Error::SliceRetrievalFailed)?
                    .as_rgba_mut(),
            )?;
        }
        3 => {
            let mut resizer = resize::new(
                src.dim().1,
                src.dim().0,
                resolution.x,
                resolution.y,
                RGBF32,
                filter,
            )?;
            resizer.resize(
                internal_source
                    .as_slice()
                    .ok_or(Error::SliceRetrievalFailed)?
                    .as_rgb(),
                target
                    .as_slice_mut()
                    .ok_or(Error::SliceRetrievalFailed)?
                    .as_rgb_mut(),
            )?;
        }
        _ => {
            // a little workaround to support any channel count ¯\_(ツ)_/¯
            let mut resizer = resize::new(
                src.dim().1,
                src.dim().0,
                resolution.x,
                resolution.y,
                GrayF32,
                filter,
            )?;
            for channel in 0..channels {
                resizer.resize(
                    internal_source
                        .slice(s![.., .., channel])
                        .as_slice()
                        .ok_or(Error::SliceRetrievalFailed)?
                        .as_gray(),
                    target
                        .slice_mut(s![.., .., channel])
                        .as_slice_mut()
                        .ok_or(Error::SliceRetrievalFailed)?
                        .as_gray_mut(),
                )?;
            }
        }
    }

    let target = target.map(|f| T::from_f32_normalized(*f).unwrap_or_default());

    Ok(target)
}

pub fn create_mipmaps<T: TraitBounds>(src: Array3<T>) -> Result<Vec<Array3<T>>> {
    let mut mipmaps = vec![src.to_owned()];
    let mut current_resolution = USizeVec2::new(src.dim().1, src.dim().0);
    let mut i = 0;
    while current_resolution.x > 1 && current_resolution.y > 1 {
        current_resolution /= 2;
        let mipmap = resize_to_resolution(mipmaps[i].view(), current_resolution, Type::Catrom)?;
        i += 1;
        mipmaps.push(mipmap);
    }
    Ok(mipmaps)
}

pub struct MipmapBuffer<T> {
    resolution: USizeVec2,
    mipmaps: Vec<Array3<T>>,
}

impl<T> MipmapBuffer<T> {
    /// Creates a new MipmapBuffer from a vector of mipmap images.
    pub fn from_vec(mipmaps: Vec<Array3<T>>) -> Result<Self> {
        if mipmaps.is_empty() {
            return Err(Error::MipmapEmpty);
        }

        Ok(Self {
            resolution: USizeVec2::new(mipmaps[0].dim().1, mipmaps[0].dim().0),
            mipmaps,
        })
    }

    pub fn get_images_view(&'_ self) -> Vec<ArrayView3<'_, T>> {
        self.mipmaps.iter().map(|f| f.view()).collect()
    }

    pub fn get_resolution(&self) -> USizeVec2 {
        self.resolution
    }
}

#[cfg(test)]
mod tests {
    use std::path::PathBuf;

    use super::*;
    use image::{DynamicImage, Rgba32FImage};
    use image_ndarray::prelude::ImageArray;
    use rstest::rstest;

    #[rstest]
    #[case(USizeVec2::new(100, 80), 0.5, USizeVec2::new(50, 40))]
    #[case(USizeVec2::new(200, 160), 2.0, USizeVec2::new(400, 320))]
    #[case(USizeVec2::new(300, 400), 0.75, USizeVec2::new(225, 300))]
    fn test_get_resolution(
        #[case] src_resolution: USizeVec2,
        #[case] scale: f32,
        #[case] expected_target: USizeVec2,
    ) {
        let src_image =
            Rgba32FImage::new(src_resolution.x as u32, src_resolution.y as u32).to_ndarray();
        assert_eq!(get_resolution(src_image.view(), scale), expected_target);
    }

    #[rstest]
    #[case(USizeVec2::new(256, 256),
    vec![
     USizeVec2::new(256, 256),
     USizeVec2::new(128, 128),
     USizeVec2::new(64, 64),
     USizeVec2::new(32, 32),
     USizeVec2::new(16, 16),
     USizeVec2::new(8, 8),
     USizeVec2::new(4, 4),
     USizeVec2::new(2, 2),
     USizeVec2::new(1, 1)
     ])]
    fn test_expected_resolutions(
        #[case] input_resolution: USizeVec2,
        #[case] expected_resolutions: Vec<USizeVec2>,
    ) {
        let original_buffer = Array3::zeros((input_resolution.y, input_resolution.x, 4));
        let result: Vec<Array3<f32>> = create_mipmaps(original_buffer).unwrap();
        for i in 0..expected_resolutions.len() {
            let (width, height, _) = result[i].dim();
            let result_resolution = USizeVec2::new(width, height);
            assert_eq!(result_resolution, expected_resolutions[i]);
        }
    }

    fn load_test_image(path: PathBuf) -> Rgba32FImage {
        let image = image::open(path);
        image.unwrap().to_rgba32f()
    }

    #[rstest]
    #[case(PathBuf::from("../../test/images/resize/"))]
    fn test_all_sizes(#[case] test_path: PathBuf) {
        let image = load_test_image(test_path.join("0.png")).to_ndarray();
        let result: Vec<Array3<f32>> = create_mipmaps(image).unwrap();
        for (i, image) in result.into_iter().enumerate() {
            let expected_image = image::open(test_path.join(format!("{i}.png")))
            .unwrap()
            .to_rgb8();
            // DynamicImage::from(Rgba32FImage::from_ndarray(image.clone()).unwrap()).to_rgb8().save(test_path.join(format!("{i}.png"))).unwrap();
            let comparison_score = image_compare::rgb_hybrid_compare(
                &DynamicImage::from(Rgba32FImage::from_ndarray(image).unwrap()).to_rgb8(),
                &expected_image,
            )
            .unwrap()
            .score;
            println!("Image: '{i}' got score: {comparison_score}");

            assert!(comparison_score >= 0.9);
        }
    }
}
