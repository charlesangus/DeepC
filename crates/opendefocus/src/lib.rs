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

pub use renders::resize::MipmapBuffer;

use crate::{error::Error, runners::ConvolveRunner, traits::TraitBounds};
use datamodel::{
    IVector4, UVector2,
    defocus::DefocusMode,
    render::{FilterMode, RenderSpecs},
};
use error::Result;
use ndarray::{Array2, Array3, ArrayViewMut3};
use worker::engine::{MINIMUM_FILTER_SIZE, RenderEngine};

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

    /// Build filter mipmaps once so they can be reused across multiple layers.
    ///
    /// The result is identical to the mipmap chain produced inside
    /// `engine.render`, but computed once and returned as a `MipmapBuffer<f32>`
    /// that the caller may pass to [`render_stripe_prepped`].
    ///
    /// `channels` should match the channel count of the images that will be
    /// rendered (typically 4 for RGBA).
    ///
    /// [`render_stripe_prepped`]: #method.render_stripe_prepped
    pub async fn prepare_filter_mipmaps(
        &self,
        settings: &datamodel::Settings,
        channels: usize,
    ) -> Result<MipmapBuffer<f32>> {
        use renders::resize::create_mipmaps;

        let max_size = settings
            .defocus
            .get_padding()
            .max(MINIMUM_FILTER_SIZE);

        // Replicate the filter-prep path from engine.rs::render().
        let filter_image = {
            use renders::resize::resize_by_scale;
            use resize::Type;

            let channels_count = channels;
            let image: Array3<f32> = if settings.render.filter.mode() == FilterMode::Simple {
                Array3::zeros((
                    MINIMUM_FILTER_SIZE as usize,
                    MINIMUM_FILTER_SIZE as usize,
                    channels_count,
                ))
            } else {
                let resolution = settings
                    .render
                    .filter
                    .calculate_filter_box(settings.bokeh.aspect_ratio);
                let resolution = glam::UVec2::new(
                    resolution[2] - resolution[0],
                    resolution[3] - resolution[1],
                )
                .as_usizevec2();
                let mut img = Array3::<f32>::zeros((resolution.y, resolution.x, channels_count));
                bokeh_creator::Renderer::render_to_array(settings.bokeh, &mut img.view_mut());
                img
            };

            // Downscale to max_size.
            let (height, width, _) = image.dim();
            let scale_factor = height.max(width) as f32;
            resize_by_scale(image.view(), max_size as f32 / scale_factor, Type::Mitchell)?
        };

        // Normalise to f32 (already f32 after the above path) and build mipmaps.
        let filter_mipmaps = if settings.defocus.defocus_mode() == DefocusMode::Twod {
            vec![filter_image]
        } else {
            create_mipmaps(filter_image)?
        };

        let filter_mipmaps: Vec<Array3<f32>> = filter_mipmaps;
        MipmapBuffer::from_vec(filter_mipmaps)
    }

    /// Render a stripe using pre-built filter mipmaps.
    ///
    /// Identical to [`render_stripe`] but skips the filter-prep + mipmap
    /// construction step (and the Telea inpaint step) by accepting a
    /// `MipmapBuffer<f32>` built via [`prepare_filter_mipmaps`].  This is
    /// the hot path for deep-image rendering where many layers share the same
    /// filter shape.
    ///
    /// [`render_stripe`]: #method.render_stripe
    /// [`prepare_filter_mipmaps`]: #method.prepare_filter_mipmaps
    pub async fn render_stripe_prepped<'image, T: TraitBounds>(
        &self,
        render_specs: datamodel::render::RenderSpecs,
        settings: datamodel::Settings,
        image: ArrayViewMut3<'image, T>,
        depth: Array2<T>,
        filter_mipmaps: &MipmapBuffer<f32>,
        holdout: &[f32],
    ) -> Result<()> {
        // validate with None filter (filter is already baked into mipmaps).
        let filter_none: Option<Array3<T>> = None;
        let depth_opt = Some(depth);
        // We only validate defocus mode / result mode constraints here — skip
        // the filter-presence check by converting to the optional form first.
        if settings.render.filter.mode() == FilterMode::Image {
            // Image-filter mode requires a filter; but in prepped path the
            // filter is already in the MipmapBuffer — no need to error.
            // Fall through to render.
        } else {
            self.validate(&settings, &depth_opt, &filter_none)?;
        }
        let engine = RenderEngine::new(settings, render_specs);
        engine
            .render_with_prebuilt_mipmaps(
                &self.runner,
                image,
                depth_opt.unwrap(),
                filter_mipmaps,
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