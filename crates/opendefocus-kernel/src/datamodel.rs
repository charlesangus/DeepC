use glam::Vec2;

#[derive(Debug)]
/// Data object to store calculated non uniform values to multiply a kernel by.
pub struct NonUniformValue {
    /// Multiplication value of opacity for kernel
    pub opacity: f32,
    /// Offset in x and y for kernel by pixel distance
    pub offset: Vec2,
    /// Size multiplication value for CoC
    pub scale: f32,
}

impl Default for NonUniformValue {
    fn default() -> Self {
        NonUniformValue {
            opacity: 1.0,
            offset: Vec2::ZERO,
            scale: 1.0,
        }
    }
}
