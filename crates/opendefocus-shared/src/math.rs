use crate::internal_settings::ConvolveSettings;
#[cfg(target_arch = "spirv")]
use core::arch::asm;
use core::f32::consts::PI;
use glam::{UVec2, Vec2, Vec4};

pub const BASE_SAMPLES: u32 = 20;

#[must_use]
#[inline]
/// Calculate the base number of points to distribute along a ring
pub fn get_points_for_ring(ring_id: u32, samples: u32, use_base_points: bool) -> u32 {
    if ring_id == 0 {
        return 1;
    }
    let mut base_points = 0;
    if use_base_points {
        base_points += BASE_SAMPLES;
    }

    base_points += ring_id * 2;
    ((samples as f32) * 0.1 * (base_points as f32)) as u32
}

#[must_use]
#[inline]
#[cfg(not(feature = "libm"))]
pub fn powf(value: f32, pow: f32) -> f32 {
    value.powf(pow)
}

#[must_use]
#[inline]
#[cfg(feature = "libm")]
pub fn powf(value: f32, pow: f32) -> f32 {
    libm::powf(value, pow as f32)
}

#[must_use]
#[inline]
#[cfg(not(feature = "libm"))]
pub fn sqrt(value: f32) -> f32 {
    value.sqrt()
}

#[inline]
#[cfg(not(feature = "libm"))]
/// Just a simple port from <https://mazzo.li/posts/vectorized-atan2.html>
/// This is a fast port mostly without branching
pub fn atan2f(a: f32, b: f32) -> f32 {
    let swap = a.abs() > b.abs();
    let atan_input = if swap { b / a } else { a / b };

    const A1: f32 = 0.99997726;
    const A3: f32 = -0.33262347;
    const A5: f32 = 0.19354346;
    const A7: f32 = -0.11643287;
    const A9: f32 = 0.05265332;
    const A11: f32 = -0.01172120;

    let z_sq = atan_input * atan_input;
    let res =
        atan_input * (A1 + z_sq * (A3 + z_sq * (A5 + z_sq * (A7 + z_sq * (A9 + z_sq * A11)))));

    let mut res = if_else!(swap, core::f32::consts::FRAC_PI_2 - res, res);
    res = if_else!(b < 0.0, core::f32::consts::PI - res, res);

    let temp_res_bits = res.to_bits();
    let temp_y_bits = a.to_bits();
    let result_bits = (temp_res_bits & 0x7FFFFFFF) | (temp_y_bits & 0x80000000);

    f32::from_bits(result_bits)
}

#[inline]
#[cfg(feature = "libm")]
pub fn atan2f(a: f32, b: f32) -> f32 {
    libm::atan2f(a, b)
}

#[must_use]
#[inline]
#[cfg(feature = "libm")]
pub fn sqrt(value: f32) -> f32 {
    libm::sqrtf(value)
}

#[must_use]
#[inline]
#[cfg(not(feature = "libm"))]
pub fn cosf(value: f32) -> f32 {
    value.cos()
}

#[must_use]
#[inline]
#[cfg(feature = "libm")]
pub fn cosf(value: f32) -> f32 {
    libm::cosf(value)
}

#[must_use]
#[inline]
#[cfg(not(feature = "libm"))]
pub fn sinf(value: f32) -> f32 {
    value.sin()
}

#[must_use]
#[inline]
#[cfg(feature = "libm")]
pub fn sinf(value: f32) -> f32 {
    libm::sinf(value)
}

#[must_use]
#[inline]
#[cfg(not(feature = "libm"))]
pub fn floorf(value: f32) -> f32 {
    value.floor()
}

#[must_use]
#[inline]
#[cfg(feature = "libm")]
pub fn floorf(value: f32) -> f32 {
    libm::floorf(value)
}

#[must_use]
#[inline]
#[cfg(not(feature = "libm"))]
pub fn ceilf(value: f32) -> f32 {
    value.ceil()
}

#[must_use]
#[inline]
#[cfg(feature = "libm")]
pub fn ceilf(value: f32) -> f32 {
    libm::ceilf(value)
}

#[must_use]
#[inline]
#[cfg(not(feature = "libm"))]
pub fn log2f(value: f32) -> f32 {
    value.log2()
}

#[must_use]
#[inline]
#[cfg(feature = "libm")]
pub fn log2f(value: f32) -> f32 {
    libm::log2f(value)
}

#[must_use]
#[inline]
/// Function that allows pow to be in the negative.
pub fn neg_pow(number: f32, exponent: f32) -> f32 {
    f32::copysign(powf(number.abs(), exponent), number)
}

#[must_use]
#[inline]
/// Linearly interpolates between two arrays of the same size.
pub fn mix_vec(a: Vec4, b: Vec4, t: f32) -> Vec4 {
    a * (1.0 - t) + b * t
}

#[must_use]
#[inline]
#[cfg(not(target_arch = "spirv"))]
/// Linearly interpolates between two values.
pub fn mix(x: f32, y: f32, a: f32) -> f32 {
    x * (1.0 - a) + y * a
}

#[must_use]
#[inline]
#[cfg(target_arch = "spirv")]
/// Linearly interpolates between two values.
pub fn mix(x: f32, y: f32, a: f32) -> f32 {
    let result;

    unsafe {
        asm!(
            "%glsl = OpExtInstImport \"GLSL.std.450\"",
            "%float = OpTypeFloat 32",
            "%x = OpLoad _ {x}",
            "%y = OpLoad _ {y}",
            "%a = OpLoad _ {a}",

            // 55 = PackUnorm4x8
            "{result} = OpExtInst %float %glsl 46 %x %y %a",
            x = in(reg) &x,
            y = in(reg) &y,
            a = in(reg) &a,
            result = out(reg) result,
        );
    }

    result
}

#[must_use]
#[inline]
pub fn saturate(x: f32) -> f32 {
    x.clamp(0.0, 1.0)
}

#[must_use]
#[inline]
#[cfg(target_arch = "spirv")]
pub fn smoothstep(edge0: f32, edge1: f32, x: f32) -> f32 {
    let result;

    unsafe {
        asm!(
            "%glsl = OpExtInstImport \"GLSL.std.450\"",
            "%float = OpTypeFloat 32",
            "%edge0 = OpLoad _ {edge0}",
            "%edge1 = OpLoad _ {edge1}",
            "%x = OpLoad _ {x}",

            // 55 = PackUnorm4x8
            "{result} = OpExtInst %float %glsl 49 %edge0 %edge1 %x",
            edge0 = in(reg) &edge0,
            edge1 = in(reg) &edge1,
            x = in(reg) &x,
            result = out(reg) result,
        );
    }

    result
}

#[must_use]
#[inline]
#[cfg(not(target_arch = "spirv"))]
pub fn smoothstep(edge0: f32, edge1: f32, x: f32) -> f32 {
    if edge1 == edge0 {
        return 0.0;
    }
    let x = saturate((x - edge0) / (edge1 - edge0));
    x * x * (3.0 - 2.0 * x)
}

#[must_use]
#[inline]
/// Calculate the coordinates in X and Y on a circle by the radius.
pub fn get_coordinates_on_circle(angle: f32, radius: f32) -> Vec2 {
    let theta = angle.to_radians();

    Vec2::new(radius * cosf(theta), radius * sinf(theta))
}

#[must_use]
#[inline]
/// Get the interpolated sample weight based on Circle of Confusion (CoC)
pub fn get_sample_weight(cached_samples: &[f32], coc: f32) -> f32 {
    let bottom = floorf(coc.abs()) as usize;
    let top = ceilf(coc.abs()) as usize;
    let bottom_value = cached_samples[bottom];
    let top_value = cached_samples[top];

    let interpolation_weight = coc.abs() - floorf(coc.abs());
    mix(bottom_value, top_value, interpolation_weight)
}

#[must_use]
#[inline]
/// Calculate the coverage weight for a pixel position in a ring
///
/// This function calculates how many times a pixel gets covered by the sampling pattern
/// based on the radius and angular distance between samples.
pub fn calculate_coverage_weight(radius: f32, total_points: u32) -> f32 {
    if total_points == 0 {
        return 1.0;
    }

    let angle_per_sample = 360.0 / total_points as f32;
    let arc_length = radius * angle_per_sample * (PI / 180.0);
    1.0 / arc_length.max(0.0001)
}

#[must_use]
#[inline]
/// Calculate the normalized coverage weight for a pixel position in a ring
pub fn calculate_normalized_coverage_weight(
    radius: f32,
    base_points: u32,
    total_points: u32,
) -> f32 {
    if base_points == 0 || total_points == 0 {
        return 1.0;
    }

    let base_weight = calculate_coverage_weight(radius, base_points);
    let total_weight = calculate_coverage_weight(radius, total_points);

    total_weight / base_weight
}

#[must_use]
#[inline]
/// Get the coordinates in screen-space based on the full region
pub fn get_real_coordinates(settings: &ConvolveSettings, coordinates: UVec2) -> UVec2 {
    UVec2::new(
        settings.full_region.x as u32 + coordinates.x,
        settings.full_region.y as u32 + coordinates.y,
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use rstest::rstest;

    #[rstest]
    #[case(1.0, 0.0, 1.5707964)]
    #[case(0.0, 1.0, 0.0)]
    #[case(1.0, 1.0, core::f32::consts::FRAC_PI_4)]
    fn test_atan2(#[case] a: f32, #[case] b: f32, #[case] expected: f32) {
        let result = atan2f(a, b);
        println!("result: '{result}', expected: '{expected}'");
        assert!((result - expected).abs() < 1e-3, "Difference is too large");
    }

    #[rstest]
    #[case(10.0, 1.0, 10.0)] // Positive number with positive exponent
    #[case(-10.0, 1.0, -10.0)] // Negative number with positive exponent
    #[case(10.0, -1.0, 0.1)] // Positive number with negative exponent
    #[case(-10.0, -1.0, -0.1)] // Negative number with negative exponent
    #[case(0.0, 2.0, 0.0)] // Zero with positive exponent
    #[case(2.0, 0.5, core::f32::consts::SQRT_2)] // Positive number with fractional exponent
    #[case(1e-6, 2.0, 1e-12)] // Very small number with positive exponent
    #[case(1e6, -2.0, 1e-12)] // Very large number with negative exponent

    fn test_neg_pow(#[case] input: f32, #[case] exponent: f32, #[case] expected: f32) {
        assert_eq!(neg_pow(input, exponent), expected)
    }
    #[rstest]
    #[case(0.0, 1.0, 0.0, 0.0)] // x below edge0
    #[case(0.0, 1.0, 0.5, 0.5)] // x between edge0 and edge1
    #[case(2.0, 1.0, 0.5, 1.0)] // x above edge1
    #[case(0.0, 0.0, 0.0, 0.0)] // edge0 == edge1
    fn test_smoothstep(
        #[case] edge0: f32,
        #[case] edge1: f32,
        #[case] x: f32,
        #[case] expected: f32,
    ) {
        assert_eq!(smoothstep(edge0, edge1, x), expected);
    }
}
