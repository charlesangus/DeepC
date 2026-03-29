#![warn(unused_extern_crates)]
#![doc=include_str!("../README.md")]

pub mod abort;
mod error;
mod renders;
mod runners;
mod traits;
mod worker;

/// Exported datastructure containing all settings for configuring the convolution
pub mod datamodel {
    pub use bokeh_creator;
    pub use circle_of_confusion;
    pub use opendefocus_datastructure::*;
}

use crate::{error::Error, runners::ConvolveRunner, traits::TraitBounds};
use datamodel::{
    IVector4, UVector2,
    defocus::DefocusMode,
    render::{FilterMode, RenderSpecs},
};
use error::Result;
use ndarray::{Array2, Array3, ArrayViewMut3};
use worker::engine::RenderEngine;

use crate::runners::shared_runner::SharedRunner;

#[derive(Debug, Clone)]
/// OpenDefocus rendering instance that stores the device configuration
pub struct OpenDefocusRenderer {
    /// Runner that is able to interpret the kernel
    runner: SharedRunner,
    gpu: bool,
}

impl OpenDefocusRenderer {
    /// Create a new `OpenDefocus` instance.
    pub async fn new(prefer_gpu: bool, settings: &mut datamodel::Settings) -> Result<Self> {
        let runner = SharedRunner::init(prefer_gpu).await;
        let mut gpu = false;
        #[cfg(feature = "wgpu")]
        if let SharedRunner::Cpu(_) = runner {
            log::warn!(
                "Using CPU software rendering for OpenDefocus. This is significantly slower compared to GPU rendering."
            )
        } else {
            gpu = true;
        };

        let backend_info = runner.backend();
        let (device_name, backend) = (backend_info.device_name, backend_info.backend);
        let device_name = format!("{device_name} - {backend}").to_string();

        settings.render.device_name = Some(device_name);
        Ok(Self { runner, gpu })
    }

    /// Return status if the renderer is using a GPU
    pub fn is_gpu(&self) -> bool {
        self.gpu
    }

    /// Render the provided image using the stripes functionality.
    ///
    /// Stripes are a subset of an entire image. Nuke uses this to keep the viewer interactive.
    /// Basically it calls this function a few times for the portion of the image.
    ///
    /// Some more information: [Nuke NDK stripes](https://learn.foundry.com/nuke/developers/16.0/ndkdevguide/2d/planariops.html)
    ///
    /// Using the provided renderspecs OpenDefocus knows which portion is being rendered
    /// and can calculate the actual screenspace position that way.
    ///
    /// Unless actually necessary, the [render](#method.render) function is a better choice.
    ///
    /// # Examples
    /// ## Skip padding region
    /// ```rust
    /// use ndarray::Array3;
    /// use opendefocus::{
    ///    OpenDefocusRenderer,
    ///     datamodel::render::RenderSpecs,
    ///     datamodel::{IVector4, Settings, UVector2},
    /// };
    ///
    /// let mut image: Array3<f32> = Array3::zeros((256, 256, 4));
    /// let mut settings = Settings::default();
    /// settings.render.resolution = UVector2 { x: 256, y: 256 }; // resolution of full image
    /// let full_region = IVector4 {
    ///     x: 0,
    ///     y: 0,
    ///     z: 256,
    ///     w: 256,
    /// }; // full stripe size
    /// let render_region = IVector4 {
    ///     x: 10,
    ///     y: 10,
    ///     z: 246,
    ///     w: 246,
    /// }; // region we want to render (padding of 6)
    /// let render_specs = RenderSpecs {
    ///     full_region,
    ///     render_region,
    /// };
    /// # tokio_test::block_on(async {
    /// let renderer = OpenDefocusRenderer::new(true, &mut settings).await.unwrap();
    /// renderer
    ///     .render_stripe(
    ///         render_specs,
    ///         settings,
    ///         image.view_mut(),
    ///         None,
    ///         None,
    ///         &[],
    ///     )
    ///     .await
    ///     .unwrap();
    /// # })
    /// ```
    ///
    /// ## Render sub portion of array
    /// ```rust
    /// use ndarray::{Array3, s};
    /// use opendefocus::{
    ///    OpenDefocusRenderer,
    ///     datamodel::render::RenderSpecs,
    ///     datamodel::{IVector4, Settings, UVector2},
    /// };
    ///
    /// let mut image: Array3<f32> = Array3::zeros((256, 256, 4));
    /// let mut settings = Settings::default();
    /// settings.render.resolution = UVector2 { x: 256, y: 256 }; // resolution of full image
    /// let full_region = IVector4 {
    ///     x: 50,
    ///     y: 50,
    ///     z: 206,
    ///     w: 206,
    /// }; // full stripe size
    /// let render_region = IVector4 {
    ///     x: 56,
    ///     y: 56,
    ///     z: 200,
    ///     w: 200,
    /// }; // region we want to render (padding of 6)
    /// let render_specs = RenderSpecs {
    ///     full_region,
    ///     render_region,
    /// };
    /// # tokio_test::block_on(async {
    /// let renderer = OpenDefocusRenderer::new(true, &mut settings).await.unwrap();
    /// renderer
    ///     .render_stripe(
    ///         render_specs,
    ///         settings,
    ///         image.slice_mut(s!(50..250, 50..250, ..)).view_mut(),
    ///         None,
    ///         None,
    ///         &[],
    ///     )
    ///     .await
    ///     .unwrap();
    /// # })
    /// ```
    ///
    pub async fn render_stripe<'image, T: TraitBounds>(
        &self,
        render_specs: datamodel::render::RenderSpecs,
        settings: datamodel::Settings,
        image: ArrayViewMut3<'image, T>,
        depth: Option<Array2<T>>,
        filter: Option<Array3<T>>,
        holdout: &[f32],
    ) -> Result<()> {
        self.validate(&settings, &depth, &filter)?;
        let engine = RenderEngine::new(settings, render_specs);
        engine
            .render(
                &self.runner,
                image,
                depth.unwrap_or(Array2::zeros((1, 1))),
                filter,
                holdout,
            )
            .await?;
        Ok(())
    }

    /// Render the provided image.
    ///
    /// # Examples
    /// ## Render provided image
    /// ```rust
    /// use ndarray::{Array3};
    /// use opendefocus::{
    ///    OpenDefocusRenderer,
    ///     datamodel::render::RenderSpecs,
    ///     datamodel::{IVector4, Settings, UVector2},
    /// };
    ///
    /// let mut image: Array3<f32> = Array3::zeros((256, 256, 4)); // obviously load the actual image, this is just empty data
    /// let mut settings = Settings::default();
    ///
    /// # tokio_test::block_on(async {
    /// let renderer = OpenDefocusRenderer::new(true, &mut settings).await.unwrap();
    /// renderer
    ///     .render(
    ///         settings,
    ///         image.view_mut(),
    ///         None,
    ///         None,
    ///     )
    ///     .await
    ///     .unwrap();
    /// # })
    /// ```
    ///
    pub async fn render<'image, T: TraitBounds>(
        &self,
        mut settings: datamodel::Settings,
        image: ArrayViewMut3<'image, T>,
        depth: Option<Array2<T>>,
        filter: Option<Array3<T>>,
    ) -> Result<()> {
        self.validate(&settings, &depth, &filter)?;
        let region = IVector4 {
            x: 0,
            y: 0,
            z: image.dim().1 as i32,
            w: image.dim().0 as i32,
        };
        let render_specs = RenderSpecs {
            full_region: region,
            render_region: region,
        };
        settings.render.resolution = UVector2 {
            x: image.dim().1 as u32,
            y: image.dim().0 as u32,
        };
        let engine = RenderEngine::new(settings, render_specs);
        engine
            .render(
                &self.runner,
                image,
                depth.unwrap_or(Array2::zeros((1, 1))),
                filter,
                &[],
            )
            .await?;
        Ok(())
    }

    /// Internal validator which performs some checks ahead of actual rendering.
    fn validate<T: TraitBounds>(
        &self,
        settings: &datamodel::Settings,
        depth: &Option<Array2<T>>,
        filter: &Option<Array3<T>>,
    ) -> Result<()> {
        if settings.render.filter.mode() == FilterMode::Image && filter.is_none() {
            return Err(Error::NoFilterProvided);
        }

        if settings.defocus.defocus_mode() != DefocusMode::Twod && depth.is_none() {
            return Err(Error::DepthNotFound);
        }

        if settings.render.result_mode() == datamodel::render::ResultMode::FocalPlaneSetup
            && settings.defocus.defocus_mode() == datamodel::defocus::DefocusMode::Twod
        {
            return Err(Error::FocalPlaneOverlayWhile2D);
        }

        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use image_ndarray::prelude::ImageArray;
    use opendefocus_datastructure::Settings;

    use super::*;
    #[tokio::test]
    async fn test_multi_channel_renderings()  {
        let test_image = image::load_from_memory(include_bytes!("../../../test/images/any/toad.png")).unwrap().to_rgb32f();
        let mut settings = Settings::default();
        let renderer = OpenDefocusRenderer::new(true, &mut settings).await.unwrap();
        renderer.render(settings, test_image.to_ndarray().view_mut(), None, None).await.unwrap();
    }
}