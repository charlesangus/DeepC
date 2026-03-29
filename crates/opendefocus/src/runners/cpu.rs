use crate::error::Result;
use crate::renders::resize::MipmapBuffer;
use crate::runners::runner::{Backend, ConvolveRunner, OUTPUT_CHANNELS};
use image::{ImageBuffer, LumaA, Rgba32FImage};
use image_ndarray::prelude::*;
use ndarray::Array3;
use opendefocus_kernel::global_entrypoint;
use rayon::iter::{IndexedParallelIterator, ParallelIterator};
use rayon::prelude::*;

use opendefocus_shared::cpu_image::{CPUImage, Sampler};
use opendefocus_shared::{ConvolveSettings, ThreadId};

#[derive(Debug, Clone)]
pub struct CpuRunner;

impl ConvolveRunner for CpuRunner {
    fn backend(&self) -> Backend {
        Backend {
            device_name: "CPU".to_string(),
            backend: "Native".to_string(),
        }
    }

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
    ) -> Result<()> {
        let resolution = input_image.dim().1;
        let input_cpu_image = CPUImage::new(&[Rgba32FImage::from_ndarray(input_image)?]);
        let inpaint_cpu_image = CPUImage::new(&[Rgba32FImage::from_ndarray(inpaint)?]);
        let filter_images: Vec<Rgba32FImage> = filters
            .get_images_view()
            .iter()
            .filter_map(|filter_mip| Rgba32FImage::from_ndarray(filter_mip.to_owned()).ok())
            .collect();

        let filters_cpu_images = CPUImage::new(&filter_images);
        let depth_cpu_image = CPUImage::new(&[ImageBuffer::<LumaA<f32>, Vec<f32>>::from_ndarray(
            depth.to_owned(),
        )?]);

        output_image
            .par_chunks_exact_mut(OUTPUT_CHANNELS)
            .enumerate()
            .for_each(|(index, slice)| {
                let x = index % resolution as usize; // TODO switch to workgroups
                let y = index / resolution as usize;

                let thread_id = ThreadId::new(x as u32, y as u32);
                global_entrypoint(
                    thread_id,
                    slice,
                    &convolve_settings,
                    cached_samples,
                    holdout,
                    &input_cpu_image,
                    &inpaint_cpu_image,
                    &filters_cpu_images,
                    &depth_cpu_image,
                    &Sampler::new(opendefocus_shared::cpu_image::Interpolation::Linear),
                    &Sampler::new(opendefocus_shared::cpu_image::Interpolation::Nearest),
                );
            });

        Ok(())
    }
}
