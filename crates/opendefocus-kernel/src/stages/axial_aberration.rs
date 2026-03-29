/// Non uniform map processor related code.
///
/// This offsets the different channels to give a different size per channel.
///
use glam::Vec4;
use opendefocus_shared::{AxialAberration, AxialAberrationType};

/// Object that offsets each R G and B channel by a slight value to
/// create the axial aberration artifact.
pub struct NonUniformMapRenderer {
    settings: AxialAberration,
    corrected_axial_aberration: f32,
}

impl NonUniformMapRenderer {
    pub fn new(settings: AxialAberration) -> Self {
        let corrected_axial_aberration = settings.amount * 0.25;
        Self {
            settings,
            corrected_axial_aberration,
        }
    }
    fn calculate_red_blue_axial_aberration(&self, component: usize, aberration_amount: f32) -> f32 {
        if component == 0 && aberration_amount > 0.0 {
            return self.corrected_axial_aberration.abs();
        }
        if component == 1 {
            return self.corrected_axial_aberration.abs() * 0.5;
        }
        if component == 2 && aberration_amount < 0.0 {
            return self.corrected_axial_aberration.abs();
        }
        0.0
    }
    fn calculate_blue_yellow_axial_aberration(
        &self,
        component: usize,
        aberration_amount: f32,
    ) -> f32 {
        if component == 0 && aberration_amount < 0.0 {
            return self.corrected_axial_aberration.abs();
        }
        if component == 1 && aberration_amount < 0.0 {
            return self.corrected_axial_aberration.abs();
        }
        if component == 2 && aberration_amount > 0.0 {
            return self.corrected_axial_aberration.abs();
        }
        0.0
    }
    fn calculate_green_purple_axial_aberration(
        &self,
        component: usize,
        aberration_amount: f32,
    ) -> f32 {
        if component == 0 && aberration_amount > 0.0 {
            return self.corrected_axial_aberration.abs();
        }
        if component == 1 && aberration_amount < 0.0 {
            return self.corrected_axial_aberration.abs() * 0.5;
        }
        if component == 2 && aberration_amount > 0.0 {
            return self.corrected_axial_aberration.abs();
        }
        0.0
    }

    fn calculate_axial_aberration(&self, source_value: f32, component: usize) -> f32 {
        let aberration_amount = if source_value < 0.0 {
            self.settings.amount
        } else {
            -self.settings.amount
        };
        let calculated_percentage_offset = match self.settings.color_type {
            AxialAberrationType::BlueYellow => {
                self.calculate_blue_yellow_axial_aberration(component, aberration_amount)
            }
            AxialAberrationType::RedBlue => {
                self.calculate_red_blue_axial_aberration(component, aberration_amount)
            }
            AxialAberrationType::GreenPurple => {
                self.calculate_green_purple_axial_aberration(component, aberration_amount)
            }
        };
        source_value * (1.0 - calculated_percentage_offset)
    }

    pub fn calculate_coc(&self, coc: f32) -> Vec4 {
        Vec4::new(
            self.calculate_axial_aberration(coc, 0),
            self.calculate_axial_aberration(coc, 1),
            self.calculate_axial_aberration(coc, 2),
            coc,
        )
    }
}
