use crate::datamodel::settings_to_convolve_settings;
use crate::renders::resize::MipmapBuffer;
use crate::traits::TraitBounds;
use glam::{UVec2};
use ndarray::{Array3, ArrayView3};
use opendefocus_datastructure::{
    Settings,
    render::{FilterMode, RenderSpecs},
};
use opendefocus_shared::{ConvolveSettings, WORKGROUP_SIZE};

use crate::error::Result;

pub const OUTPUT_CHANNELS: usize = 5;

pub struct Backend {
    pub device_name: String,
    pub backend: String,
}

pub trait ConvolveRunner {
    fn backend(&self) -> Backend;

    async fn execute_kernel_pass(
        &self,
        output_image: &mut [f32],
        input_image: Array3<f32>,
        inpaint: Array3<f32>,
        filters: &MipmapBuffer<f32>,
        depth: Array3<f32>,
        convolve_settings: ConvolveSettings,
        cached_samples: &[f32],
        holdout: &[f32],
    ) -> Result<()>;

    fn prepare_data<T: TraitBounds>(&self, data: &[T]) -> (Vec<f32>, usize) {
        let gpu_data: Vec<f32> = data
            .iter()
            .map(|&x| x.to_f32_normalized().unwrap_or_default())
            .collect();
        (gpu_data, data.len())
    }

    fn finalize_data<T: TraitBounds>(&self, gpu_data: &[f32], output: &mut [T]) {
        for (i, &val) in gpu_data.iter().take(output.len()).enumerate() {
            output[i] = T::from_f32_normalized(val).unwrap_or_default();
        }
    }

    async fn convolve<'image, T: TraitBounds>(
        &self,
        output_image: &mut [T],
        input_image: ArrayView3<'image, T>,
        inpaint: Array3<T>,
        filters: &MipmapBuffer<f32>,
        depth: Array3<f32>,
        render_specs: &RenderSpecs,
        settings: &Settings,
        holdout: &[f32],
    ) -> Result<()> {
        if output_image.len() <= 1 {
            return Ok(());
        }

        let (mut gpu_output_image_data, original_output_image_size) =
            self.prepare_data(output_image);

        let image_elements = output_image.len() / OUTPUT_CHANNELS;

        let convolve_settings = settings_to_convolve_settings(
            settings,
            render_specs,
            if settings.render.filter.mode() == FilterMode::Simple {
                let resolution = settings.render.filter.calculate_filter_box(settings.bokeh.aspect_ratio);
                UVec2::new(resolution[2] - resolution[0], resolution[3] - resolution[1])
            } else {
                filters.get_resolution().as_uvec2()
            },
            image_elements as u32,
        );
        let cached_samples = convolve_settings.get_sample_weights();

        let input_image_normalized =
            input_image.mapv(|f| f.to_f32_normalized().unwrap_or_default());
        let inpaint_normalized = inpaint.mapv(|f| f.to_f32_normalized().unwrap_or_default());

        self.execute_kernel_pass(
            &mut gpu_output_image_data,
            input_image_normalized,
            inpaint_normalized,
            filters,
            depth,
            convolve_settings,
            &cached_samples,
            holdout,
        )
        .await?;

        gpu_output_image_data.truncate(original_output_image_size);

        self.finalize_data::<T>(&gpu_output_image_data, output_image);

        Ok(())
    }
    fn compute_workgroup_count(resolution: UVec2) -> UVec2 {
        UVec2::new(
            resolution.x.div_ceil(WORKGROUP_SIZE),
            resolution.y.div_ceil(WORKGROUP_SIZE),
        )
    }
}
