use crate::error::Result;
use crate::runners::{CpuRunner, runner::ConvolveRunner};
use crate::{renders::resize::MipmapBuffer, runners::runner::Backend};
use ndarray::{Array3};
use opendefocus_shared::ConvolveSettings;

#[cfg(feature = "wgpu")]
use crate::runners::WgpuRunner;

#[derive(Debug, Clone)]
pub enum SharedRunner {
    Cpu(CpuRunner),

    #[cfg(feature = "wgpu")]
    WGPU(WgpuRunner),
}
impl SharedRunner {
    pub async fn init(_prefer_gpu: bool) -> Self {
        #[cfg(feature = "wgpu")]
        {
            let runner = if _prefer_gpu {
                match WgpuRunner::new().await {
                    Ok(runner) => Some(runner),
                    Err(error) => {
                        log::error!("{error}");
                        None
                    }
                }
            } else {
                None
            };

            if let Some(runner) = runner {
                return Self::WGPU(runner);
            }
        }

        Self::Cpu(CpuRunner)
    }
}

impl ConvolveRunner for SharedRunner {
    fn backend(&self) -> Backend {
        match self {
            Self::Cpu(runner) => runner.backend(),
            #[cfg(feature = "wgpu")]
            Self::WGPU(runner) => runner.backend(),
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
        match self {
            Self::Cpu(runner) => {
                runner
                    .execute_kernel_pass(
                        output_image,
                        input_image,
                        inpaint,
                        filters,
                        depth,
                        convolve_settings,
                        cached_samples,
                        holdout,
                    )
                    .await
            }
            #[cfg(feature = "wgpu")]
            Self::WGPU(runner) => {
                runner
                    .execute_kernel_pass(
                        output_image,
                        input_image,
                        inpaint,
                        filters,
                        depth,
                        convolve_settings,
                        cached_samples,
                        holdout,
                    )
                    .await
            }
        }
    }
}
