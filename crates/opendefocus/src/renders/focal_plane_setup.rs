use crate::traits::{TraitBounds};
use ndarray::{ArrayView2, ArrayViewMut3};

fn render_pixel(depth: f32, channel: usize, output_real_values: bool) -> f32 {
    if output_real_values {
        match channel {
            0 => {
                if depth > 0.0 {
                    depth.abs()
                } else {
                    0.0
                }
            }
            1 => {
                if depth == 0.0 {
                    1.0
                } else {
                    0.0
                }
            }
            2 => {
                if depth < 0.0 {
                    depth.abs()
                } else {
                    0.0
                }
            }
            _ => 1.0,
        }
    } else {
        match channel {
            0 => {
                if depth > 0.0 {
                    1.0
                } else {
                    0.0
                }
            }
            1 => {
                if depth == 0.0 {
                    1.0
                } else {
                    0.0
                }
            }
            2 => {
                if depth < 0.0 {
                    1.0
                } else {
                    0.0
                }
            }
            _ => 0.5,
        }
    }
}

pub fn render_focal_plane_overlay<T: TraitBounds>(
    mut image: ArrayViewMut3<T>,
    depth: &ArrayView2<f32>,
    output_real_values: bool,
) {
    image
        .indexed_iter_mut()
        .for_each(|((y, x, channel), value)| {
            let alpha = T::from_f32_normalized(render_pixel(depth[[y, x]], 3, output_real_values))
                .unwrap_or_default();
            *value =
                ((T::from_f32_normalized(render_pixel(depth[[y, x]], channel, output_real_values))
                    .unwrap_or_default())
                    * alpha)
                    + (*value * (T::from_f32_normalized(1.0).unwrap_or_default() - alpha))
        });
}

#[cfg(test)]
mod tests {
    use super::*;
    use assert_approx_eq::assert_approx_eq;
    use rstest::rstest;

    #[rstest]
    #[case(true, 10.0, 10.0, 0.0, 0.0, 1.0)] // Test positive depth with output_real_values
    #[case(true, -10.0, 0.0, 0.0, 10.0, 1.0)] // Test negative depth with output_real_values
    #[case(true, 0.0, 0.0, 1.0, 0.0, 1.0)] // Test zero depth with output_real_values
    #[case(false, 10.0, 1.0, 0.0, 0.0, 0.5)] // Test positive depth with output_real_values=false
    #[case(false, -10.0, 0.0, 0.0, 1.0, 0.5)] // Test negative depth with output_real_values=false
    #[case(false, 0.0, 0.0, 1.0, 0.0, 0.5)] // Test zero depth with output_real_values=false

    /// Test render_pixel method with different depth values and output_real_values settings
    fn test_render_pixel(
        #[case] output_real_values: bool,
        #[case] depth: f32,
        #[case] expected_r: f32,
        #[case] expected_g: f32,
        #[case] expected_b: f32,
        #[case] expected_a: f32,
    ) {
        assert_approx_eq!(render_pixel(depth, 0, output_real_values), expected_r, 1e-6);
        assert_approx_eq!(render_pixel(depth, 1, output_real_values), expected_g, 1e-6);
        assert_approx_eq!(render_pixel(depth, 2, output_real_values), expected_b, 1e-6);
        assert_approx_eq!(render_pixel(depth, 3, output_real_values), expected_a, 1e-6);
    }
}
