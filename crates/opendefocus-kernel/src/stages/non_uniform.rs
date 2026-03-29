use glam::{UVec2, Vec2};
use opendefocus_shared::*;

use crate::datamodel::NonUniformValue;

/// Calculate the non-uniform artifacts
///
/// This function computes various lens artifacts that create non-uniform bokeh effects.
/// It returns a NonUniformValue struct containing values to modify kernel sampling,
/// allowing for effects like cut-off bokehs, stretched bokehs, and other lens distortions.
///
/// The function applies three types of artifacts if enabled in settings:
/// 1. Barndoors (matte box effect) - creates cut-off edges
/// 2. Astigmatism - stretches bokeh based on screen position
/// 3. Catseye - creates circular artifacts with varying intensity
///
/// * distance: distance from the sample to the kernel center
/// * real_position: actual screen-space coordinates of the sample
/// * size: calculated Circle of Confusion (CoC) size for the sample
///
/// Returns: NonUniformValue struct containing opacity, offset, and other parameters
/// to modify kernel sampling for non-uniform effects
pub fn get_non_uniform(
    settings: &ConvolveSettings,
    distance: Vec2,
    real_position: UVec2,
    size: f32,
) -> NonUniformValue {
    let mut non_uniform_values = NonUniformValue::default();

    let current_position = real_position.as_vec2() + distance;

    let inverse_y = 1.0
        - 2.0
            * (f32_from_bool!(
                GlobalFlags::from_bits_retain(settings.flags).contains(GlobalFlags::INVERSE_Y)
            ));
    let distance_to_kernel_center = distance / size * 2.0;
    let distance_to_screen_center =
        (current_position - settings.center) / settings.center * inverse_y;
    non_uniform_values.opacity *= if_else!(
        NonUniformFlags::from_bits_retain(settings.non_uniform_flags)
            .contains(NonUniformFlags::BARNDOORS_ENABLED),
        calculate_barndoors(
            settings,
            distance_to_kernel_center,
            distance_to_screen_center,
            size,
        ),
        1.0
    );

    non_uniform_values.offset += if_else!(
        NonUniformFlags::from_bits_retain(settings.non_uniform_flags)
            .contains(NonUniformFlags::ASTIGMATISM_ENABLED),
        calculate_astigmatism(
            settings,
            distance_to_kernel_center,
            distance_to_screen_center,
            real_position,
            size,
        ),
        Vec2::ZERO
    );
    non_uniform_values.opacity *= calculate_catseye(
        settings,
        distance_to_kernel_center,
        distance_to_screen_center,
        size,
    );

    non_uniform_values
}

const BARNDOORS_MULTIPLICATION_FACTOR: f32 = 0.02;

// #[inline]
/// Calculate the barndoor artifact
///
/// Also known as the matte box effect, this artifact simulates light being blocked
/// by physical objects in the lens, creating cut-off edges in the bokeh.
/// This results in straight cuts or partial obstructions in the out-of-focus areas.
fn calculate_barndoors(
    settings: &ConvolveSettings,
    distance_to_kernel_center: Vec2,
    distance_to_screen_center: Vec2,
    size: f32,
) -> f32 {
    let mut calculated_barndoor_multiplier =
        Vec2::new(settings.barndoors_amount, settings.barndoors_amount);
    calculated_barndoor_multiplier *= (size.abs() * 0.2).clamp(0.0, 1.0);

    calculated_barndoor_multiplier.x *= if_else!(
        distance_to_screen_center.x < 0.0,
        settings.barndoors_right * BARNDOORS_MULTIPLICATION_FACTOR,
        settings.barndoors_left * BARNDOORS_MULTIPLICATION_FACTOR
    );
    calculated_barndoor_multiplier.y *= if_else!(
        distance_to_screen_center.y < 0.0,
        settings.barndoors_top * BARNDOORS_MULTIPLICATION_FACTOR,
        settings.barndoors_bottom * BARNDOORS_MULTIPLICATION_FACTOR
    );

    let distance_to_screen_center_gamma = Vec2::new(
        math::neg_pow(distance_to_screen_center.x, 1.0 / settings.barndoors_gamma),
        math::neg_pow(distance_to_screen_center.y, 1.0 / settings.barndoors_gamma),
    );

    let mut inverse = if_else!(
        NonUniformFlags::from_bits_retain(settings.non_uniform_flags)
            .contains(NonUniformFlags::BARNDOORS_INVERSE),
        1.0,
        -1.0
    );
    inverse *= if_else!(
        NonUniformFlags::from_bits_retain(settings.non_uniform_flags)
            .contains(NonUniformFlags::BARNDOORS_INVERSE_FOREGROUND)
            && size > 0.0,
        -1.0,
        1.0
    );

    let positive = Vec2::new(
        f32::copysign(1.0, distance_to_screen_center.x),
        f32::copysign(1.0, distance_to_screen_center.y),
    );

    let distance_to_kernel =
        inverse * distance_to_kernel_center + (calculated_barndoor_multiplier - 0.5) * positive;
    let result = 1.0 - (distance_to_kernel * distance_to_screen_center_gamma);
    f32_from_bool!(result.x > 0.0).min(f32_from_bool!(result.y > 0.0))
}

/// Calculate the astigmatism artifact
///
/// Astigmatism creates a stretching effect in the bokeh based on the sample's position
/// relative to the screen center. This simulates lens imperfections that cause
/// different amounts of refraction in different orientations.
fn calculate_astigmatism(
    settings: &ConvolveSettings,
    distance_to_kernel_center: Vec2,
    distance_to_screen_center: Vec2,
    position: UVec2,
    size: f32,
) -> Vec2 {
    let angle_to_center = angle_to_center(settings.center, position);

    let new_kernel_center = Vec2::new(
        distance_to_kernel_center.x * math::cosf(angle_to_center.x)
            + distance_to_kernel_center.y * math::sinf(angle_to_center.x),
        -distance_to_kernel_center.x * math::sinf(angle_to_center.y)
            + distance_to_kernel_center.y * math::cosf(angle_to_center.y),
    );

    let mut offset = new_kernel_center * size * settings.astigmatism_amount * 0.4;
    offset.x *= math::powf(
        distance_to_screen_center.y.abs(),
        1.0 / settings.astigmatism_gamma,
    );
    offset.y *= math::powf(
        distance_to_screen_center.x.abs(),
        1.0 / settings.astigmatism_gamma,
    );

    f32_from_bool!((position.as_vec2() - distance_to_screen_center) != Vec2::ZERO) * offset
}

fn angle_to_center(center: Vec2, position: UVec2) -> Vec2 {
    let distance: Vec2 = center - position.as_vec2();
    let angle_to_center = Vec2::new(
        if_else!(
            (position.y as f32) < center.y,
            math::atan2f(-(distance.y.abs()), -(distance.x)) + 90.0_f32.to_radians(),
            math::atan2f(distance.y, distance.x) + 90.0_f32.to_radians()
        ),
        if_else!(
            (position.x as f32) < center.x,
            math::atan2f(distance.y, distance.x),
            math::atan2f(-(distance.y), -(distance.x))
        ),
    );

    angle_to_center
}

/// Calculate the catseye artifact
///
/// The catseye artifact creates circular patterns with varying intensity based on
/// the sample's distance from the screen center. This simulates lens imperfections
/// that cause circular artifacts to appear in the bokeh.
fn calculate_catseye(
    settings: &ConvolveSettings,
    distance_to_kernel_center: Vec2,
    distance_to_screen_center: Vec2,
    size: f32,
) -> f32 {
    let mut catseye_kernel_center = distance_to_kernel_center;
    let mut distance_to_center = distance_to_screen_center;

    let mut inverse = if_else!(
        size > 0.0
            && NonUniformFlags::from_bits_retain(settings.non_uniform_flags)
                .contains(NonUniformFlags::CATSEYE_INVERSE),
        -1.0,
        1.0
    );
    inverse *= if_else!(
        size > 0.0
            && NonUniformFlags::from_bits_retain(settings.non_uniform_flags)
                .contains(NonUniformFlags::CATSEYE_INVERSE_FOREGROUND),
        -1.0,
        1.0
    );
    distance_to_center *= ((size * 0.2).abs()).clamp(0.0, 1.0);

    let normalized_resolution_y = (settings.resolution.y as f32) * settings.pixel_aspect_normalizer;
    let screen_relative = NonUniformFlags::from_bits_retain(settings.non_uniform_flags)
        .contains(NonUniformFlags::CATSEYE_SCREEN_RELATIVE);

    let division = Vec2::new(
        if_else!(screen_relative && (settings.resolution.x as f32) > normalized_resolution_y, normalized_resolution_y / (settings.resolution.x as f32), 1.0),
        if_else!(screen_relative && (settings.resolution.x as f32) < normalized_resolution_y, (settings.resolution.x as f32) / normalized_resolution_y, 1.0),

    );
    distance_to_center.x = math::neg_pow(distance_to_center.x, division.x / settings.catseye_gamma);
    distance_to_center.y = math::neg_pow(distance_to_center.y, division.y / settings.catseye_gamma);

    catseye_kernel_center += inverse * (distance_to_center * settings.catseye_amount);

    let diagonal_to_center_squared = catseye_kernel_center.length_squared();
    let mut result = -((diagonal_to_center_squared * 0.25) - 1.0);
    result /= (1.0 + (settings.catseye_softness * 0.5)) - 1.0;
    result.clamp(0.0, 1.0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use rstest::rstest;

    #[rstest]
    #[case(
        Vec2::new(5.0, 5.0),
        UVec2::new(3, 3),
        Vec2::new(-45.0_f32.to_radians(), 45.0_f32.to_radians()),
    )]
    #[case(
        Vec2::new(5.0, 5.0),
        UVec2::new(6, 6),
        Vec2::new(-45.0_f32.to_radians(), 45.0_f32.to_radians()),
    )]
    #[case(
        Vec2::new(5.0, 5.0),
        UVec2::new(5, 10),
        Vec2::new(0.0, 90.0_f32.to_radians()),
    )]
    #[case(
        Vec2::new(5.0, 5.0),
        UVec2::new(10, 5),
        Vec2::new(270.0_f32.to_radians(), 0.0),
    )]
    #[case(
        Vec2::new(5.0, 5.0),
        UVec2::new(10, 5),
        Vec2::new(270.0_f32.to_radians(), 0.0),
    )]
    #[case(
        Vec2::new(5.0, 5.0),
        UVec2::new(10, 5),
        Vec2::new(270.0_f32.to_radians(), 0.0),
    )]
    #[case(
        Vec2::new(5.0, 5.0),
        UVec2::new(10, 5),
        Vec2::new(270.0_f32.to_radians(), 0.0),
    )]
    #[case(
        Vec2::new(5.0, 5.0),
        UVec2::new(10, 10),
        Vec2::new(-45.0_f32.to_radians(), 45.0_f32.to_radians()),
    )]
    #[case(
        Vec2::new(5.0, 5.0),
        UVec2::new(5, 10),
        Vec2::new(0.0_f32.to_radians(), 90.0_f32.to_radians()),
    )]
    #[case(
        Vec2::new(5.0, 5.0),
        UVec2::new(5, 10),
        Vec2::new(0.0_f32.to_radians(), 90.0_f32.to_radians()),
    )]
    fn test_angle_to_center(#[case] center: Vec2, #[case] position: UVec2, #[case] expected: Vec2) {
        let result = angle_to_center(center, position);
        println!("{}", result);
        println!("{}", expected);
        assert!((result.x - expected.x).abs() < 1e-5, "Test case x failed");
        assert!((result.y - expected.y).abs() < 1e-5, "Test case y failed");
    }
}
