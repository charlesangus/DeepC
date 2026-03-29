use crate::error::Result;
use crate::traits::TraitBounds;
use inpaint::telea_inpaint;
use ndarray::{Array2, Array3, ArrayView2, ArrayView3, s};

/// Get the alpha of the foreground
///
/// If the provided depth is below 0, it will return 0. If its above 0, it will return 1.
/// This is for all channels (rgba, so 4 channels)
fn get_inpaint_mask(depth: ArrayView2<f32>) -> Array2<u8> {
    depth.mapv(|f| if f < 0.0 { 0 } else { 255 })
}

/// Get the inpaint image based on the for- and background.
///
/// This gets the alpha from background, premults it and inpaints the missing region.
/// It also copies the depth into the alpha channel, for an inpainted background depth.
pub fn get_inpaint_image<'a, T: TraitBounds>(
    image: ArrayView3<'a, T>,
    depth: ArrayView2<'a, f32>,
) -> Result<(Array3<T>, Array2<f32>)> {
    let alpha = get_inpaint_mask(depth);
    let channels = image.dim().2;
    let converted_image = image.mapv(|f| f.to_f32_normalized().unwrap_or_default());
    let mut inpaint_image = Array3::default((image.dim().0, image.dim().1, image.dim().2 + 1));
    inpaint_image
        .slice_mut(s![.., .., ..channels])
        .assign(&converted_image);
    inpaint_image.slice_mut(s![.., .., channels]).assign(&depth);
    telea_inpaint::<f32, u8>(&mut inpaint_image.view_mut(), &alpha.view(), 4)?;

    let converted_inpaint_image = inpaint_image
        .slice(s![.., .., ..channels])
        .mapv(|f| T::from_f32_normalized(f).unwrap_or_default());

    Ok((
        converted_inpaint_image,
        inpaint_image.slice(s![.., .., channels]).to_owned(),
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{DynamicImage, ImageBuffer, Luma, Rgba32FImage};
    use image_ndarray::prelude::ImageArray;
    use rstest::rstest;
    use std::path::PathBuf;

    fn load_test_image(path: PathBuf) -> Rgba32FImage {
        let image = image::open(path);
        image.unwrap().to_rgba32f()
    }

    #[rstest]
    #[case([-1.0, 0.0, 1.0, 2.0], [0, 255, 255, 255])]
    #[case([1.0, 148912.0, 1.0, 0.0], [255, 255, 255, 255])]
    #[case([-123.0, -140.0, -231.0, -2.0], [0, 0, 0, 0])]

    /// Test to get the alpha channel based on the depth values of the input
    fn test_get_foreground_alpha(#[case] input: [f32; 4], #[case] expected: [u8; 4]) {
        let image = Array2::from_shape_vec((2, 2), input.to_vec()).unwrap();
        let alpha = get_inpaint_mask(image.view());

        assert_eq!(alpha.as_slice().unwrap().to_vec(), expected);
    }

    #[rstest]
    #[case(
        PathBuf::from("../../test/images/preprocessor/image.jpg"),
        PathBuf::from("../../test/images/preprocessor/depth.exr"),
        PathBuf::from("../../test/images/preprocessor/expected_telea.png"),
        PathBuf::from("../../test/images/preprocessor/expected_depth_telea.png")
    )]

    /// Test inpaint of provided image with mask
    fn test_inpaint(
        #[case] image: PathBuf,
        #[case] depth: PathBuf,
        #[case] expected_input: PathBuf,
        #[case] expected_depth_input: PathBuf,
    ) {
        let depth_image = load_test_image(depth).to_ndarray();
        let input_image = load_test_image(image).to_ndarray();

        let (inpainted_image, mut inpainted_depth) =
            get_inpaint_image(input_image.view(), depth_image.slice(s!(.., .., 0))).unwrap();

        inpainted_depth = inpainted_depth.abs();

        let inpainted_depth =
            ImageBuffer::<Luma<f32>, Vec<f32>>::from_ndarray(inpainted_depth).unwrap();

        let inpainted_image = Rgba32FImage::from_ndarray(inpainted_image).unwrap();

        let result_image = DynamicImage::from(inpainted_image.clone()).to_rgba8();
        let result_depth = DynamicImage::from(inpainted_depth.clone()).to_rgba8();
        // result_depth.save(expected_depth_input.clone());
        // result_image.save(expected_input.clone());

        let expected_input_image =
            DynamicImage::from(load_test_image(expected_input.clone())).to_rgba8();
        let expected_depth_image =
            DynamicImage::from(load_test_image(expected_depth_input.clone())).to_rgba8();
        let comparison_score =
            image_compare::rgba_hybrid_compare(&result_image, &expected_input_image)
                .unwrap()
                .score;

        println!("Test image got score: {comparison_score}");
        assert!(comparison_score >= 0.99);

        let comparison_score =
            image_compare::rgba_hybrid_compare(&result_depth, &expected_depth_image)
                .unwrap()
                .score;

        println!("Test depth got score: {comparison_score}");
        assert!(comparison_score >= 0.99);
    }
}
