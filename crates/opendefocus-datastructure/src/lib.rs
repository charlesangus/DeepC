#![warn(unused_extern_crates)]

use circle_of_confusion::Math;
use glam::{IVec4, UVec2, Vec2};
use opendefocus_shared::{ConvolveSettings, GlobalFlags, NonUniformFlags};

use opendefocus_shared::math::{ceilf, floorf};

use crate::defocus::DefocusMode;
use crate::render::{FilterMode, Quality};

include!(concat!(env!("OUT_DIR"), "/opendefocus.rs"));

impl IVector4 {
    pub fn to_ivec4(&self) -> IVec4 {
        IVec4::new(self.x, self.y, self.z, self.w)
    }
}

impl UVector2 {
    pub fn to_uvec2(&self) -> UVec2 {
        UVec2::new(self.x, self.y)
    }
}

impl Vector2 {
    pub fn to_vec2(&self) -> Vec2 {
        Vec2::new(self.x, self.y)
    }
}

impl render::Filter {
    pub fn calculate_filter_box(&self, aspect_ratio: f32) -> [u32; 4] {
        let resolution = UVector2 {
            x: self.resolution,
            y: self.resolution,
        };
        let offset_multiplier = Vector2 {
            x: 3.0 - aspect_ratio.min(1.0) * 2.0,
            y: aspect_ratio.max(1.0) * 2.0 - 1.0,
        };
        let offset = UVector2 {
            x: floorf((resolution.x as f32 / offset_multiplier.x) * (1.0 - aspect_ratio).max(0.0))
                as u32,
            y: floorf((resolution.y as f32 / offset_multiplier.y) * (aspect_ratio - 1.0).max(0.0))
                as u32,
        };
        [
            offset.x,
            offset.y,
            resolution.x - offset.x,
            resolution.y - offset.y,
        ]
    }
}

impl render::RenderSpecs {
    /// Creates render specs from rectangles.
    pub fn from_rects(full_region: IVec4, render_region: IVec4) -> Self {
        Self {
            full_region: IVector4 {
                x: full_region.x,
                y: full_region.y,
                z: full_region.z,
                w: full_region.w,
            },
            render_region: IVector4 {
                x: render_region.x,
                y: render_region.y,
                z: render_region.z,
                w: render_region.w,
            },
        }
    }

    /// Scale the render specs by a specified factor.
    pub fn scale(&self, scale: f32) -> Self {
        let mut scaled_full_region = self.full_region;
        scaled_full_region.x = floorf(scaled_full_region.x as f32 * scale) as i32;
        scaled_full_region.y = floorf(scaled_full_region.y as f32 * scale) as i32;
        scaled_full_region.z = ceilf(scaled_full_region.z as f32 * scale) as i32;
        scaled_full_region.w = ceilf(scaled_full_region.w as f32 * scale) as i32;

        let mut scaled_render_region = self.render_region;
        scaled_render_region.x = floorf(scaled_render_region.x as f32 * scale) as i32;
        scaled_render_region.y = floorf(scaled_render_region.y as f32 * scale) as i32;
        scaled_render_region.z = ceilf(scaled_render_region.z as f32 * scale) as i32;
        scaled_render_region.w = ceilf(scaled_render_region.w as f32 * scale) as i32;
        Self {
            full_region: scaled_full_region,
            render_region: scaled_render_region,
        }
    }

    /// Get the resolution of the full region.
    pub fn get_resolution(&self) -> UVec2 {
        UVec2::new(
            (self.full_region.z - self.full_region.x) as u32,
            (self.full_region.w - self.full_region.y) as u32,
        )
    }

    /// Get the resolution of the render region.
    pub fn get_render_resolution(&self) -> UVec2 {
        UVec2::new(
            (self.render_region.z - self.render_region.x) as u32,
            (self.render_region.w - self.render_region.y) as u32,
        )
    }
}

impl defocus::Settings {
    /// Calculate the effective defocus size.
    pub fn get_size(&self) -> f32 {
        self.circle_of_confusion.size
            * self.circle_of_confusion.pixel_aspect
            * self.size_multiplier.max(0.0)
            * self.proxy_scale.unwrap_or(1.0)
    }

    pub fn get_raw_max_size(&self) -> f32 {
        match self.circle_of_confusion.camera_data {
            Some(_) => self.camera_max_size,
            _ => self.circle_of_confusion.max_size,
        }
    }

    fn get_size_multiplier(&self) -> f32 {
        self.size_multiplier.max(0.0)
    }

    fn get_proxy_scale(&self) -> f32 {
        self.proxy_scale.unwrap_or(1.0)
    }

    /// Calculate the maximum defocus size based on the current mode.
    pub fn get_max_size(&self) -> f32 {
        let max_size = self.get_raw_max_size();
        max_size
            * self.circle_of_confusion.pixel_aspect
            * self.get_size_multiplier()
            * self.get_proxy_scale()
    }

    /// Determines the math interpretation of depth.
    pub fn get_math(&self) -> Math {
        todo!()
    }

    /// Gets the current maximum size for defocus.
    pub fn get_current_max_size(&self) -> f32 {
        if self.defocus_mode() == DefocusMode::Twod {
            return self.get_size();
        }
        self.get_max_size()
    }

    /// Calculate the padding required for rendering.
    pub fn get_padding(&self) -> u32 {
        (ceilf(self.get_current_max_size())) as u32
    }
}

impl render::Settings {
    pub fn get_quality(&self) -> Quality {
        if self.gui {
            self.quality()
        } else {
            self.farm_quality()
        }
    }
}

fn settings_to_globalflags(settings: &Settings, non_uniform_flags: NonUniformFlags) -> GlobalFlags {
    let mut flags = GlobalFlags::empty();
    if settings.render.filter.mode() == FilterMode::Simple {
        flags |= GlobalFlags::SIMPLE_DISC;
    }
    if non_uniform_flags.intersects(
        NonUniformFlags::CATSEYE_ENABLED
            | NonUniformFlags::BARNDOORS_ENABLED
            | NonUniformFlags::ASTIGMATISM_ENABLED,
    ) {
        flags |= GlobalFlags::USE_NON_UNIFORM;
    }

    if settings.defocus.defocus_mode() == DefocusMode::Twod {
        flags |= GlobalFlags::IS_2D;
    }
    if settings.non_uniform.inverse_foreground {
        flags |= GlobalFlags::INVERSE_FOREGROUND_BOKEH_SHAPE;
    }
    if settings.render.inverse_y {
        flags |= GlobalFlags::INVERSE_Y;
    }
    if settings.non_uniform.axial_aberration.enable {
        flags |= GlobalFlags::AXIAL_ABERRATION_ENABLE;
    }
    flags
}

fn non_uniform_to_flags(settings: &non_uniform::Settings) -> NonUniformFlags {
    let mut non_uniform_flags = NonUniformFlags::empty();
    if settings.catseye.enable {
        non_uniform_flags |= NonUniformFlags::CATSEYE_ENABLED;
        if settings.catseye.inverse {
            non_uniform_flags |= NonUniformFlags::CATSEYE_INVERSE;
        }
        if settings.catseye.inverse_foreground {
            non_uniform_flags |= NonUniformFlags::CATSEYE_INVERSE_FOREGROUND;
        }
        if settings.catseye.relative_to_screen {
            non_uniform_flags |= NonUniformFlags::CATSEYE_SCREEN_RELATIVE;
        }
    }

    if settings.astigmatism.enable {
        non_uniform_flags |= NonUniformFlags::ASTIGMATISM_ENABLED;
    }

    if settings.barndoors.enable {
        non_uniform_flags |= NonUniformFlags::BARNDOORS_ENABLED;
        if settings.barndoors.inverse {
            non_uniform_flags |= NonUniformFlags::BARNDOORS_INVERSE
        }
        if settings.barndoors.inverse_foreground {
            non_uniform_flags |= NonUniformFlags::BARNDOORS_INVERSE_FOREGROUND
        }
    }
    non_uniform_flags
}

/// Calculates the number of samples to use for convolution based on render settings and size.
///
/// # Arguments
///
/// * `settings` - Reference to the render settings
/// * `size` - The size parameter used to calculate samples
///
/// # Returns
///
/// The calculated number of samples as an i32 value
pub fn get_samples(settings: &render::Settings, size: i32) -> u32 {
    let quality = settings.get_quality();
    match quality {
        render::Quality::Low => (size as f32 * 0.1).clamp(8.0, 20.0) as u32,
        render::Quality::Medium => (size as f32 * 0.2).clamp(12.0, 50.0) as u32,
        render::Quality::High => (size as f32 * 0.5).clamp(30.0, 100.0) as u32,
        render::Quality::Custom => settings.samples as u32,
    }
}

pub fn settings_to_convolve_settings(
    settings: &Settings,
    render_specs: &render::RenderSpecs,
    filter_resolution: UVec2,
    image_elements: u32,
) -> ConvolveSettings {
    let max_size = if settings.defocus.defocus_mode() == DefocusMode::Twod {
        settings.defocus.get_size()
    } else {
        settings.defocus.get_raw_max_size()
    };
    let samples = get_samples(&settings.render, ceilf(max_size) as i32);
    let filter_aspect_ratio = filter_resolution.x as f32 / filter_resolution.y as f32;
    let filter_aspect_ratio_normalizer = 1.0 / filter_aspect_ratio;
    let ring_distance = max_size / samples as f32;
    let pixel_aspect_normalizer = 1.0 / settings.defocus.circle_of_confusion.pixel_aspect;

    let non_uniform_flags = non_uniform_to_flags(&settings.non_uniform);
    let flags = settings_to_globalflags(settings, non_uniform_flags);

    ConvolveSettings {
        samples,
        center: settings.render.center.to_vec2(),
        process_region: render_specs.render_region.to_ivec4(),
        full_region: render_specs.full_region.to_ivec4(),
        resolution: settings.render.resolution.to_uvec2(),
        pixel_aspect: settings.defocus.circle_of_confusion.pixel_aspect,
        size: settings.defocus.get_size(),
        max_size,
        flags: flags.bits(),
        non_uniform_flags: non_uniform_flags.bits(),
        catseye_amount: settings.non_uniform.catseye.amount,
        catseye_gamma: settings.non_uniform.catseye.gamma,
        catseye_softness: settings.non_uniform.catseye.softness,
        astigmatism_amount: settings.non_uniform.astigmatism.amount,
        astigmatism_gamma: settings.non_uniform.astigmatism.gamma,
        barndoors_amount: settings.non_uniform.barndoors.amount,
        barndoors_gamma: settings.non_uniform.barndoors.gamma,
        barndoors_top: settings.non_uniform.barndoors.top,
        barndoors_bottom: settings.non_uniform.barndoors.bottom,
        barndoors_left: settings.non_uniform.barndoors.left,
        barndoors_right: settings.non_uniform.barndoors.right,
        axial_aberration_amount: settings.non_uniform.axial_aberration.amount,
        axial_aberration_type: settings.non_uniform.axial_aberration.color_type,
        filter_aspect_ratio_normalizer,
        filter_aspect_ratio,
        ring_distance,
        filter_resolution,
        pixel_aspect_normalizer,
        render_scale: 1 as u32,
        image_elements,
        _padding: 0,
    }
}
