use crate::abort::get_aborted;
use crate::error::{Error, Result};
use crate::runners::runner::Backend;
use crate::{renders::resize::MipmapBuffer, runners::runner::ConvolveRunner};
use ndarray::{Array3};
use opendefocus_shared::{ConvolveSettings, GlobalFlags};
use wgpu::{DeviceType, util::DeviceExt};
use wgpu::{ExperimentalFeatures, Limits, include_wgsl};

#[derive(Debug, Clone)]
pub struct WgpuRunner {
    pub device: wgpu::Device,
    queue: wgpu::Queue,
    convolve_pipeline: Option<wgpu::ComputePipeline>,
    convolve_bind_group_layout: Option<wgpu::BindGroupLayout>,
    backend_name: String,
    adapter_name: String,
}

impl WgpuRunner {
    pub async fn new() -> Result<Self> {
        #[cfg(all(target_os = "macos"))]
        let backends = wgpu::Backends::METAL;

        #[cfg(not(target_os = "macos"))]
        let backends = wgpu::Backends::all();

        let instance = wgpu::Instance::new(&wgpu::InstanceDescriptor {
            backends,
            ..Default::default()
        });

        let adapter = instance
            .request_adapter(&wgpu::RequestAdapterOptions {
                power_preference: wgpu::PowerPreference::HighPerformance,
                force_fallback_adapter: false,
                compatible_surface: None,
            })
            .await
            .map_err(|_| Error::NoAdapter)?;

        if adapter.get_info().device_type == DeviceType::Cpu {
            return Err(Error::WgpuUsingCPUFallback);
        }

        let info = adapter.get_info();
        let backend_name = match info.backend {
            wgpu::Backend::Vulkan => "Vulkan",
            wgpu::Backend::Metal => "Metal",
            wgpu::Backend::Dx12 => "DirectX 12",
            wgpu::Backend::Gl => "OpenGL",
            wgpu::Backend::BrowserWebGpu => "WebGPU",
            _ => "Unknown",
        }
        .to_string();
        let adapter_name = info.name.clone();

        let (device, queue) = adapter
            .request_device(&wgpu::DeviceDescriptor {
                label: Some("GPU Device"),
                experimental_features: ExperimentalFeatures::disabled(),
                required_limits: Limits {
                    ..Default::default()
                },
                required_features: wgpu::Features::FLOAT32_FILTERABLE,
                memory_hints: Default::default(),
                trace: wgpu::Trace::default(),
            })
            .await?;

        let (convolve_pipeline, convolve_bind_group_layout) =
            Self::create_convolve_pipeline(&device);

        Ok(Self {
            device,
            queue,
            convolve_pipeline,
            convolve_bind_group_layout,
            backend_name,
            adapter_name,
        })
    }

    fn create_convolve_pipeline(
        device: &wgpu::Device,
    ) -> (Option<wgpu::ComputePipeline>, Option<wgpu::BindGroupLayout>) {
        let wgsl = include_wgsl!("../shaders/opendefocus-kernel.wgsl");
        let shader_module = unsafe {
            device.create_shader_module_trusted(wgsl, wgpu::ShaderRuntimeChecks::unchecked())
        };

        let bind_group_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("Convolve Bind Group Layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform {},
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                // Output image buffer
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: false },
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                // Input image buffer
                wgpu::BindGroupLayoutEntry {
                    binding: 4,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                // Filter buffer
                wgpu::BindGroupLayoutEntry {
                    binding: 5,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                // Inpaint buffer
                wgpu::BindGroupLayoutEntry {
                    binding: 6,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                // Depth buffer
                wgpu::BindGroupLayoutEntry {
                    binding: 7,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 8,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: true },
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
            ],
        });

        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("Convolve Pipeline Layout"),
            bind_group_layouts: &[&bind_group_layout],
            ..Default::default()
        });

        let entry_point = "convolve_kernel_f32_";

        let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("Convolve Pipeline"),
            layout: Some(&pipeline_layout),
            module: &shader_module,
            entry_point: Some(entry_point),
            compilation_options: Default::default(),
            cache: None,
        });

        (Some(pipeline), Some(bind_group_layout))
    }

    async fn execute_compute_shader(
        &self,
        output_image: &mut [f32],
        input_image: Array3<f32>,
        inpaint: Array3<f32>,
        filters: &MipmapBuffer<f32>,
        depth: Array3<f32>,
        convolve_settings: ConvolveSettings,
        cached_samples: &[f32],
        pipeline: &wgpu::ComputePipeline,
        bind_group_layout: &wgpu::BindGroupLayout,
    ) -> Result<()> {
        let size = std::mem::size_of_val(output_image) as u64;
        let resolution = convolve_settings.get_image_resolution();
        let image_size = wgpu::Extent3d {
            width: resolution.x,
            height: resolution.y,
            depth_or_array_layers: 1,
        };

        let filter_size = wgpu::Extent3d {
            width: convolve_settings.filter_resolution.x,
            height: convolve_settings.filter_resolution.y,
            depth_or_array_layers: 1,
        };
        let num_workgroups = Self::compute_workgroup_count(resolution);
        let convolve_settings_bytes = bytemuck::bytes_of(&convolve_settings);

        let bilinear_sampler = self.device.create_sampler(&wgpu::SamplerDescriptor {
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::MipmapFilterMode::Linear,
            ..Default::default()
        });

        let nearest_sampler = self.device.create_sampler(&wgpu::SamplerDescriptor {
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Nearest,
            min_filter: wgpu::FilterMode::Nearest,
            mipmap_filter: wgpu::MipmapFilterMode::Nearest,
            ..Default::default()
        });

        let convolve_settings_buffer =
            self.device
                .create_buffer_init(&wgpu::util::BufferInitDescriptor {
                    label: Some("Convolve settings"),
                    contents: &convolve_settings_bytes,
                    usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
                });

        let output_image_buffer =
            self.device
                .create_buffer_init(&wgpu::util::BufferInitDescriptor {
                    label: Some("Output Image Buffer"),
                    contents: bytemuck::cast_slice(output_image),
                    usage: wgpu::BufferUsages::STORAGE
                        | wgpu::BufferUsages::COPY_SRC
                        | wgpu::BufferUsages::COPY_DST,
                });

        let input_image_buffer = self.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("Input Image Buffer"),
            size: image_size,
            mip_level_count: 1,
            sample_count: 1,
            view_formats: &[],
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba32Float,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        });

        let inpaint_image_buffer = self.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("Inpaint Image Buffer"),
            size: image_size,
            mip_level_count: 1,
            sample_count: 1,
            view_formats: &[],
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba32Float,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        });

        let filter_buffer = self.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("Input filter"),
            size: filter_size,
            mip_level_count: filters.get_images_view().len() as u32,
            sample_count: 1,
            view_formats: &[],
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba32Float,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        });

        let depth_buffer = self.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("Depth Buffer"),
            size: image_size,
            mip_level_count: 1,
            sample_count: 1,
            view_formats: &[],
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rg32Float,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        });

        let cached_samples_buffer =
            self.device
                .create_buffer_init(&wgpu::util::BufferInitDescriptor {
                    label: Some("Depth Buffer"),
                    contents: bytemuck::cast_slice(cached_samples),
                    usage: wgpu::BufferUsages::STORAGE,
                });

        let staging_buffer = self.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("Output Image Staging Buffer"),
            size,
            usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });

        let current_resolution = convolve_settings.get_image_resolution();
        let texture = input_image_buffer.as_image_copy();
        let texture_size = wgpu::Extent3d {
            width: current_resolution.x,
            height: current_resolution.y,
            depth_or_array_layers: 1,
        };
        self.queue.write_texture(
            texture,
            bytemuck::cast_slice(input_image.as_slice().unwrap()),
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(
                    current_resolution.x
                        * std::mem::size_of::<f32>() as u32
                        * input_image.dim().2 as u32,
                ),
                rows_per_image: None,
            },
            texture_size,
        );

        let mut current_resolution = convolve_settings.filter_resolution;
        if !GlobalFlags::from_bits_retain(convolve_settings.flags)
            .intersects(GlobalFlags::SIMPLE_DISC)
        {
            for (i, mipmap) in filters.get_images_view().iter().enumerate() {
                let mut texture = filter_buffer.as_image_copy();
                texture.mip_level = i as u32;
                let texture_size = wgpu::Extent3d {
                    width: current_resolution.x,
                    height: current_resolution.y,
                    depth_or_array_layers: 1,
                };
                self.queue.write_texture(
                    texture,
                    bytemuck::cast_slice(mipmap.as_slice().unwrap()),
                    wgpu::TexelCopyBufferLayout {
                        offset: 0,
                        bytes_per_row: Some(
                            current_resolution.x
                                * std::mem::size_of::<f32>() as u32
                                * mipmap.dim().2 as u32,
                        ),
                        rows_per_image: None,
                    },
                    texture_size,
                );
                current_resolution /= 2;
            }
        }

        if !GlobalFlags::from_bits_retain(convolve_settings.flags).intersects(GlobalFlags::IS_2D) {
            let current_resolution = convolve_settings.get_image_resolution();
            let texture = inpaint_image_buffer.as_image_copy();
            let texture_size = wgpu::Extent3d {
                width: current_resolution.x,
                height: current_resolution.y,
                depth_or_array_layers: 1,
            };
            self.queue.write_texture(
                texture,
                bytemuck::cast_slice(inpaint.as_slice().unwrap()),
                wgpu::TexelCopyBufferLayout {
                    offset: 0,
                    bytes_per_row: Some(
                        current_resolution.x
                            * std::mem::size_of::<f32>() as u32
                            * inpaint.dim().2 as u32,
                    ),
                    rows_per_image: None,
                },
                texture_size,
            );

            self.queue.write_texture(
                depth_buffer.as_image_copy(),
                bytemuck::cast_slice(depth.as_slice().unwrap()),
                wgpu::TexelCopyBufferLayout {
                    offset: 0,
                    bytes_per_row: Some(
                        image_size.width as u32
                            * std::mem::size_of::<f32>() as u32
                            * depth.dim().2 as u32,
                    ),
                    rows_per_image: None,
                },
                image_size,
            );
        }

        let bind_group = self.device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("Convolve Bind Group"),
            layout: bind_group_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: convolve_settings_buffer.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(&bilinear_sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::Sampler(&nearest_sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: output_image_buffer.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 4,
                    resource: wgpu::BindingResource::TextureView(
                        &input_image_buffer.create_view(&wgpu::TextureViewDescriptor::default()),
                    ),
                },
                wgpu::BindGroupEntry {
                    binding: 5,
                    resource: wgpu::BindingResource::TextureView(
                        &filter_buffer.create_view(&wgpu::TextureViewDescriptor::default()),
                    ),
                },
                wgpu::BindGroupEntry {
                    binding: 6,
                    resource: wgpu::BindingResource::TextureView(
                        &inpaint_image_buffer.create_view(&wgpu::TextureViewDescriptor::default()),
                    ),
                },
                wgpu::BindGroupEntry {
                    binding: 7,
                    resource: wgpu::BindingResource::TextureView(
                        &depth_buffer.create_view(&wgpu::TextureViewDescriptor::default()),
                    ),
                },
                wgpu::BindGroupEntry {
                    binding: 8,
                    resource: cached_samples_buffer.as_entire_binding(),
                },
            ],
        });

        let mut encoder = self
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                label: Some(&format!("Convolve Encoder")),
            });

        {
            let mut compute_pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: Some(&format!("Convolve Pass Stage",)),
                timestamp_writes: None,
            });

            compute_pass.set_pipeline(pipeline);
            compute_pass.set_bind_group(0, &bind_group, &[]);

            compute_pass.dispatch_workgroups(num_workgroups.x, num_workgroups.y, 1);
        }

        if get_aborted() {
            return Ok(());
        }
        let mut copy_encoder =
            self.device
                .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                    label: Some("Convolve Copy Encoder"),
                });

        copy_encoder.copy_buffer_to_buffer(&output_image_buffer, 0, &staging_buffer, 0, size);
        let (sender, receiver) = futures::channel::oneshot::channel();
        copy_encoder.map_buffer_on_submit(&staging_buffer, wgpu::MapMode::Read, .., |result| {
            let _ = sender.send(result);
        });

        self.queue.submit([encoder.finish(), copy_encoder.finish()]);

        let buffer_slice = staging_buffer.slice(..);
        let _ = self.device.poll(wgpu::PollType::wait_indefinitely());
        receiver.await??;

        {
            let view = buffer_slice.get_mapped_range();
            output_image.copy_from_slice(bytemuck::cast_slice(&view));
        }

        staging_buffer.unmap();

        Ok(())
    }
}

impl ConvolveRunner for WgpuRunner {
    fn backend(&self) -> super::runner::Backend {
        Backend {
            backend: self.backend_name.to_string(),
            device_name: self.adapter_name.to_string(),
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
        _holdout: &[f32],
    ) -> Result<()> {
        if self.convolve_pipeline.is_none() || self.convolve_bind_group_layout.is_none() {
            return Err(Error::ConvolvePipelineNotAvailable);
        }

        let pipeline = self.convolve_pipeline.as_ref().unwrap();
        let bind_group_layout = self.convolve_bind_group_layout.as_ref().unwrap();

        self.execute_compute_shader(
            output_image,
            input_image,
            inpaint,
            filters,
            depth,
            convolve_settings,
            cached_samples,
            pipeline,
            bind_group_layout,
        )
        .await
    }
}
