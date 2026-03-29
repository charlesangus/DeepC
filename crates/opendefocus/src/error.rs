use thiserror::Error;

/// Error types that can occur during OpenDefocus operations.
///
/// This enum represents various error conditions that may be encountered
/// when using the OpenDefocus library. Each variant provides
/// specific information about the nature of the error.
/// Use this error struct when catching errors in your code.
#[derive(Debug, Error)]
pub enum Error {
    #[error("An error occured during the handling of the image: '{0}'.")]
    Image(#[from] image::ImageError),
    #[error("Resize failed: '{0}'")]
    Resize(#[from] resize::Error),
    #[error("Image-ndarray: '{0}'")]
    ImageNdarray(#[from] image_ndarray::Error),
    #[error("Inpaint: '{0}'")]
    Inpaint(#[from] inpaint::Error),
    #[error("No suitable GPU adapter found")]
    NoAdapter,
    #[error("No filter provided but 'image' selected as filter")]
    NoFilterProvided,
    #[error("Slice reference could not be retrieved")]
    SliceRetrievalFailed,
    #[error("No device is available to render. This means that no CPU or GPU could be found.")]
    IncompatibleSystem,
    #[error("Filter image is set to 'image' but no image is provided.")]
    FilterNotProvided,
    #[error("Focal plane setup is only available on depth based operations, not 2D.")]
    FocalPlaneOverlayWhile2D,
    #[error("No image data provided to process.")]
    NoDataProvided,
    #[error("The number of depth images and regular images is not the same.")]
    DepthNotFound,
    #[error("Index is invalid")]
    InvalidIndex,
    #[error("Buffer could not be constructed.")]
    BufferConstructionFailure,
    #[error("Mipmap buffer is empty and cannot be constructed")]
    MipmapEmpty,
    #[error("No render device available. Please make sure Vulkanpipe or a GPU is installed.")]
    NoDeviceAvailable,
    #[error("{0}")]
    Shape(#[from] ndarray::ShapeError),
    #[error("Output slice was unable to be created")]
    OutputSlice,
    #[error("Image is deep, but the provided channels do not contain an alpha channel. (Index 4 is missing).")]
    InvalidDeep,
    #[error("{0}")]
    IOError(#[from] std::io::Error),
    #[cfg(feature = "wgpu")]
    #[error("wgpu error: {0}")]
    Wgpu(#[from] wgpu::Error),
    #[cfg(feature = "wgpu")]
    #[error("WGPU device error: {0}")]
    WgpuRequestDevice(#[from] wgpu::RequestDeviceError),
    #[cfg(feature = "wgpu")]
    #[error("WGPU is using cpu fallback, which is not supported.")]
    WgpuUsingCPUFallback,
    #[cfg(feature = "wgpu")]
    #[error("Convolve pipeline is not available within instance.")]
    ConvolvePipelineNotAvailable,
    #[cfg(feature = "wgpu")]
    #[error("{0}")]
    Canceled(#[from] futures::channel::oneshot::Canceled),
    #[cfg(feature = "wgpu")]
    #[error("{0}")]
    BufferAsync(#[from] wgpu::BufferAsyncError),
    #[error("The input channel count is resizing to a lower value which is not supported")]
    InvalidChannelCount,
}

pub type Result<T> = core::result::Result<T, Error>;
