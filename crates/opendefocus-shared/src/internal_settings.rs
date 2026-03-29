use crate::math::{ceilf, get_points_for_ring};

use bitflags::bitflags;
use bytemuck::{Pod, Zeroable};
use core::f32::consts::PI;
use glam::{IVec4, UVec2, Vec2};
const LARGEST_WEIGHT: f32 = 1.0 / (PI * 0.5);

bitflags! {
    /// These are the most commonly used flags, to reduce memory reads they are together
    #[derive(Debug, Clone, Copy, Zeroable, Pod, PartialEq)]
    #[repr(C)]
    pub struct GlobalFlags: u32 {
        /// Processing in 2d or using depth map
        const IS_2D                                         = 0b00000001;
        /// Use a simple disc, this prevents reads to the filter kernel
        const SIMPLE_DISC                                   = 0b00000010;
        /// Merge low defocus size over the original image
        const HIGH_RESOLUTION_INTERPOLATION                 = 0b00000100;
        /// Use non uniform bokeh calculations
        const USE_NON_UNIFORM                               = 0b00001000;
        /// Inverse the bokeh shape in the foreground
        const INVERSE_FOREGROUND_BOKEH_SHAPE                = 0b00010000;
        /// Enable offset size of the filter kernel per channel
        const AXIAL_ABERRATION_ENABLE                       = 0b00100000;
        /// Interpret the y from bottom to top
        const INVERSE_Y                                     = 0b01000000;

    }

    // Flags for non uniform settings
    #[derive(Debug, Clone, Copy, Zeroable, Pod, PartialEq)]
    #[repr(C)]
    pub struct NonUniformFlags: u32 {
        /// Use Catseye non uniform calculations
        const CATSEYE_ENABLED                               = 0b00000001;
        /// Flag indicating whether to invert catseye for foreground
        const CATSEYE_INVERSE                               = 0b00000010;
        /// Flag indicating whether to invert catseye for foreground
        const CATSEYE_INVERSE_FOREGROUND                    = 0b00000100;
        /// Flag indicating whether catseye is screen relative
        const CATSEYE_SCREEN_RELATIVE                       = 0b00001000;
        /// Enable the astigmatism artifact
        const ASTIGMATISM_ENABLED                           = 0b00010000;
        /// Enable the barndoors cutting of bokehs
        const BARNDOORS_ENABLED                             = 0b00100000;
        /// Inverse the entirity of barndoors
        const BARNDOORS_INVERSE                             = 0b01000000;
        /// Inverse barndoors only in foreground
        const BARNDOORS_INVERSE_FOREGROUND                  = 0b10000000;
    }

}

/// Artifact that is caused by the light rays entering the lens at a different
/// rate at different focal distances.
pub struct AxialAberration {
    /// Flag indicating whether axial aberration is enabled
    pub enable: bool,
    /// Amount of the axial aberration effect
    pub amount: f32,
    /// Color type for the aberration
    pub color_type: AxialAberrationType,
}

#[repr(u32)]
pub enum AxialAberrationType {
    /// Red and blue color combination
    RedBlue = 1,
    /// Blue and yellow color combination
    BlueYellow = 2,
    /// Green and purple color combination
    GreenPurple = 3,
}

/// Settings structure for convolution operations.
#[derive(Copy, Clone, Debug, Pod, Zeroable)]
#[repr(C, align(16))]
pub struct ConvolveSettings {
    /// Region to process (x, y, width, height)
    pub process_region: IVec4,
    /// Full region (x, y, width, height)
    pub full_region: IVec4,
    /// Resolution of the render (width, height)
    pub resolution: UVec2,
    /// Resolution of the filter (width, height)
    pub filter_resolution: UVec2,
    /// Center point of the convolution
    pub center: Vec2,
    /// Global operation flags
    pub flags: u32,
    /// Flags for non uniform settings
    pub non_uniform_flags: u32,
    /// Number of samples to use for convolution
    pub samples: u32,
    /// Pixel aspect ratio
    pub pixel_aspect: f32,
    /// Base size for convolution
    pub size: f32,
    /// Maximum size for convolution
    pub max_size: f32,
    /// Amount of catseye effect
    pub catseye_amount: f32,
    /// Gamma correction for catseye
    pub catseye_gamma: f32,
    /// Softness of catseye edges
    pub catseye_softness: f32,
    /// Amount of astigmatism
    pub astigmatism_amount: f32,
    /// Gamma correction for astigmatism
    pub astigmatism_gamma: f32,
    /// Amount of barndoors effect
    pub barndoors_amount: f32,
    /// Gamma correction for barndoors
    pub barndoors_gamma: f32,
    /// Top position of barndoors
    pub barndoors_top: f32,
    /// Bottom position of barndoors
    pub barndoors_bottom: f32,
    /// Left position of barndoors
    pub barndoors_left: f32,
    /// Right position of barndoors
    pub barndoors_right: f32,
    /// Normalization factor for filter aspect ratio
    pub filter_aspect_ratio_normalizer: f32,
    /// Total number of
    pub image_elements: u32,
    /// Distance between rings in the filter
    pub ring_distance: f32,
    /// Aspect ratio of the filter
    pub filter_aspect_ratio: f32,
    /// Normalization factor for pixel aspect
    pub pixel_aspect_normalizer: f32,
    /// Size of render
    pub render_scale: u32,
    /// Flag indicating whether to invert foreground bokeh shape
    pub axial_aberration_amount: f32,
    pub axial_aberration_type: i32,

    pub _padding: u32,
}
impl ConvolveSettings {
    pub fn compute_sample_weight(&self, coc: f32) -> f32 {
        let mut total_points = 1;
        let mut ring_id = 0;
        let mut remaining_coc = coc;

        while remaining_coc > 0.0 {
            total_points += get_points_for_ring(ring_id, self.samples, true);
            remaining_coc -= self.ring_distance;
            ring_id += 1;
        }
        (1.0 / (total_points as f32)).min(LARGEST_WEIGHT)
    }

    pub fn get_sample_weights(&self) -> [f32; 2048] {
        let mut weights = [0.0; 2048];
        for coc in 0..ceilf(self.max_size) as i32 + 1 {
            weights[coc as usize] = self.compute_sample_weight(coc as f32)
        }
        weights
    }

    pub fn get_image_resolution(&self) -> UVec2 {
        UVec2::new(
            (self.full_region.z - self.full_region.x) as u32,
            (self.full_region.w - self.full_region.y) as u32,
        )
    }

    /// Get the coordinates in screen-space based on the full region
    pub fn get_real_coordinates(&self, coordinates: UVec2) -> UVec2 {
        UVec2::new(
            (self.full_region.x as u32) + coordinates.x,
            (self.full_region.y as u32) + coordinates.y,
        )
    }

    pub fn get_axial_aberration_settings(&self) -> AxialAberration {
        AxialAberration {
            enable: NonUniformFlags::from_bits_retain(self.non_uniform_flags)
                .contains(NonUniformFlags::BARNDOORS_ENABLED),
            amount: self.axial_aberration_amount,
            color_type: match self.axial_aberration_type {
                1 => AxialAberrationType::RedBlue,
                2 => AxialAberrationType::BlueYellow,
                3 => AxialAberrationType::GreenPurple,
                _ => AxialAberrationType::RedBlue,
            },
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Struct size needs to be properly aligned for spirv
    #[test]
    fn test_struct_size() {
        assert_eq!(core::mem::size_of::<ConvolveSettings>(), 160)
    }
}
