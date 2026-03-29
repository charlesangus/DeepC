use crate::abort::get_aborted;
use crate::error::{Error, Result};
use crate::renders::focal_plane_setup::render_focal_plane_overlay;
use crate::renders::preprocessor::get_inpaint_image;
use crate::renders::resize::{MipmapBuffer, create_mipmaps, resize_by_scale};
use crate::runners::ConvolveRunner;
use crate::runners::OUTPUT_CHANNELS;
use crate::runners::shared_runner::SharedRunner;
use crate::traits::TraitBounds;
use crate::worker::chunks::ChunkHandler;
use circle_of_confusion::{Math, Resolution};
use core::f32;
use glam::UVec2;
use ndarray::{Array2, Array3, ArrayView2, ArrayView3, ArrayViewMut3, Zip, s};
use opendefocus_datastructure::defocus::DefocusMode;
use opendefocus_datastructure::render::{FilterMode, RenderSpecs};
use opendefocus_datastructure::{Settings, render::ResultMode};
use resize::Type;
const MINIMUM_FILTER_SIZE: u32 = 8;
const ALPHA_CHANNEL: usize = 4;
const PROCESSING_CHANNEL_COUNT: usize = 4;

pub struct RenderEngine {
    settings: Settings,
    render_specs: RenderSpecs,
}

impl RenderEngine {
    pub fn new(settings: Settings, render_specs: RenderSpecs) -> Self {
        Self {
            settings,
            render_specs,
        }
    }

    /// Main render wrangler
    ///
    /// It decides what to render and when according to the settings.
    ///
    /// It will always write the output to the first image of the images vec
    pub async fn render<'image, T: TraitBounds>(
        &self,
        runner: &SharedRunner,
        mut image: ArrayViewMut3<'image, T>,
        depth: Array2<T>,
        filter: Option<Array3<T>>,
        holdout: &[f32],
    ) -> Result<()> {
        if self.settings.render.filter.preview {
            render_preview_bokeh(image, &self.settings)?;
            return Ok(());
        }

        let channels = image.dim().2;
        let filter_image = prepare_filter_image(
            filter,
            &self.settings,
            self.settings.defocus.get_padding().max(MINIMUM_FILTER_SIZE),
            channels,
        )?;
        let depth_image = prepare_depth_map(depth, &self.settings)?;
        if self.settings.render.result_mode() == ResultMode::FocalPlaneSetup
            && self.settings.defocus.defocus_mode() != DefocusMode::Twod
        {
            render_focal_plane_preview(
                image,
                depth_image.view(),
                !self.settings.defocus.show_image,
            )?;
            return Ok(());
        }

        if get_aborted() {
            return Ok(());
        }

        let filter_mipmaps = if self.settings.defocus.defocus_mode() == DefocusMode::Twod {
            vec![filter_image]
        } else {
            create_mipmaps(filter_image)?
        };

        let filter_mipmaps = {
            let mut maps = Vec::new();
            for map in filter_mipmaps.iter() {
                let mut target_map = Array3::zeros(map.dim());
                for ((y, x, z), value) in map.indexed_iter() {
                    target_map[[y, x, z]] = value.to_f32_normalized().unwrap_or_default();
                }
                maps.push(target_map);
            }
            maps
        };
        let filter_mipmaps = MipmapBuffer::from_vec(filter_mipmaps)?;
        if get_aborted() {
            return Ok(());
        }
        let chunks = ChunkHandler::new(
            &self.render_specs,
            self.settings.defocus.get_padding() as i32,
        );
        for chunk in chunks.get_render_specs() {
            let image = image.slice_mut(s![
                (chunk.full_region.y - self.render_specs.full_region.y) // looks complicated but we need the actual slice index, not the render spec slice as that is a coordinate in true screenspace.
                    ..(chunk.full_region.w - self.render_specs.full_region.y),
                (chunk.full_region.x - self.render_specs.full_region.x)
                    ..(chunk.full_region.z - self.render_specs.full_region.x),
                ..
            ]);
            let depth_view = if self.settings.defocus.defocus_mode() == DefocusMode::Twod {
                depth_image.view()
            } else {
                depth_image.slice(s![
                    (chunk.full_region.y - self.render_specs.full_region.y)
                        ..(chunk.full_region.w - self.render_specs.full_region.y),
                    (chunk.full_region.x - self.render_specs.full_region.x)
                        ..(chunk.full_region.z - self.render_specs.full_region.x),
                ])
            };

            self.render_convolve(image, runner, &chunk, &filter_mipmaps, depth_view, holdout)
                .await?;
        }

        Ok(())
    }

    async fn render_convolve<'image, 'depth, T: TraitBounds>(
        &self,
        mut image: ArrayViewMut3<'image, T>,
        runner: &SharedRunner,
        render_specs: &RenderSpecs,
        filter_mipmaps: &MipmapBuffer<f32>,
        depth: ArrayView2<'depth, f32>,
        holdout: &[f32],
    ) -> Result<()> {
        let original_image = resize_array_dimensions(image.view(), 4)?;

        let mut output_image_data = Array3::zeros((
            original_image.dim().0,
            original_image.dim().1,
            OUTPUT_CHANNELS,
        ));

        let (inpaint_image, depth_array) =
            if self.settings.defocus.defocus_mode() == DefocusMode::Twod {
                let depth_array = Array3::from_shape_vec(
                    (depth.dim().0, depth.dim().1, 2),
                    depth
                        .iter()
                        .flat_map(|&x| vec![x, x]) // duplicate each element
                        .collect(),
                )?;
                (Array3::zeros(original_image.dim()), depth_array)
            } else {
                let (inpaint_image, inpaint_depth) = get_inpaint_image(original_image.view(), depth.view())?;
                let depth_array = Array3::from_shape_vec(
                    (depth.dim().0, depth.dim().1, 2),
                    depth
                        .iter()
                        .zip(inpaint_depth.as_slice().unwrap_or_default().iter())
                        .flat_map(|(&d, &i)| vec![d, i])
                        .collect(),
                )?;
                (inpaint_image, depth_array)
            };

        runner
            .convolve(
                output_image_data.as_slice_mut().ok_or(Error::OutputSlice)?,
                original_image.view(),
                inpaint_image,
                filter_mipmaps,
                depth_array,
                render_specs,
                &self.settings,
                holdout,
            )
            .await?;

        Zip::indexed(&mut image).par_for_each(|(y, x, channel), value| {
            let alpha = output_image_data[[y, x, ALPHA_CHANNEL]];
            *value = output_image_data[[y, x, channel]]
                + original_image[[y, x, channel]]
                    * (T::from_f32_normalized(1.0).unwrap_or_default() - alpha);
        });

        Ok(())
    }
}

/// Prepare the filter image for further processing
///
/// This checks if a filter image is provided. If so, just scale down based on the max size.
/// Else it will render a new kernel image using the KernelRenderer
fn prepare_filter_image<T: TraitBounds>(
    filter: Option<Array3<T>>,
    settings: &Settings,
    max_size: u32,
    channels: usize,
) -> Result<Array3<T>> {
    let image = if let Some(image) = filter {
        let (height, width, _) = image.dim();
        let scale_factor = height.max(width) as f32;
        let filter_image =
            resize_by_scale(image.view(), max_size as f32 / scale_factor, Type::Mitchell)?;

        resize_array_dimensions(filter_image.view(), PROCESSING_CHANNEL_COUNT)?

        
    } else if settings.render.filter.mode() == FilterMode::Simple {
        Array3::zeros((
            MINIMUM_FILTER_SIZE as usize,
            MINIMUM_FILTER_SIZE as usize,
            channels,
        ))
    } else {
        let resolution = settings
            .render
            .filter
            .calculate_filter_box(settings.bokeh.aspect_ratio);
        let resolution =
            UVec2::new(resolution[2] - resolution[0], resolution[3] - resolution[1]).as_usizevec2();
        let mut image = Array3::zeros((resolution.y, resolution.x, channels));
        bokeh_creator::Renderer::render_to_array(settings.bokeh, &mut image.view_mut());
        resize_array_dimensions(image.view(), PROCESSING_CHANNEL_COUNT)?
    };

    Ok(image)
}

/// Render the kernel directly to the target image
fn render_preview_bokeh<'a, T: TraitBounds>(
    mut image: ArrayViewMut3<'a, T>,
    settings: &Settings,
) -> Result<()> {
    bokeh_creator::Renderer::render_to_array(settings.bokeh.clone(), &mut image);
    Ok(())
}

/// Create a depth image according to the settings for all provided images.
fn prepare_depth_map<T: TraitBounds>(
    depth_image: Array2<T>,
    settings: &Settings,
) -> Result<Array2<f32>> {
    if settings.defocus.defocus_mode() == DefocusMode::Twod {
        return Ok(Array2::from_elem((1, 1), settings.defocus.get_size()));
    }

    if settings.defocus.use_direct_math {
        return Ok(depth_image.mapv(|value| value.to_f32_normalized().unwrap_or_default()));
    }
    let mut coc_settings = settings.defocus.circle_of_confusion;
    coc_settings.max_size = settings.defocus.get_max_size();
    coc_settings.size = settings.defocus.get_size();

    if settings.defocus.use_camera_focal {
        if let Some(camera_data) = coc_settings.camera_data.as_mut() {
            camera_data.resolution = Resolution {
                width: settings.render.resolution.x,
                height: settings.render.resolution.y,
            };
        }
    }

    coc_settings.math = match coc_settings.math() {
        Math::OneDividedByZ => circle_of_confusion::Math::OneDividedByZ.into(),
        Math::Real => circle_of_confusion::Math::Real.into(),
    };

    let calculator = circle_of_confusion::Calculator::new(coc_settings);

    return Ok(depth_image.mapv(|value| {
        -circle_of_confusion::calculate(&calculator, value.to_f32_normalized().unwrap_or_default()) // TODO remove negative and adjust algorithm in kernel to work with the new logic
    }));
}

/// Render the overlay based on the rendered depth images.
///
/// This creates a red/green/blue overlay.
/// * Red is in the far field
/// * Green is in focus
/// * Blue is out of focus
fn render_focal_plane_preview<'a, T: TraitBounds>(
    image: ArrayViewMut3<'a, T>,
    depth: ArrayView2<f32>,
    output_real_values: bool,
) -> Result<()> {
    render_focal_plane_overlay::<T>(image, &depth, output_real_values);

    Ok(())
}

fn resize_array_dimensions<T: TraitBounds>(
    array: ArrayView3<T>,
    dimensions: usize,
) -> Result<Array3<T>> {
    let difference = dimensions as i32 - array.dim().2 as i32;
    if difference < 0 {
        return Err(Error::InvalidChannelCount);
    } else if difference == 0 {
        return Ok(array.to_owned());
    }
    let mut target = Array3::zeros((array.dim().0, array.dim().1, dimensions));
    target.slice_mut(s![.., .., ..array.dim().2]).assign(&array);

    Ok(target)
}
