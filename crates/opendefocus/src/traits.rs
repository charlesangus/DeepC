use core::ops::{Add, AddAssign, Div, DivAssign, Mul, MulAssign, Sub, SubAssign};

use image_ndarray::prelude::*;
use num_traits::{AsPrimitive, Zero};

pub trait TraitBounds:
    NormalizedFloat<Self>
    + AsPrimitive<f32>
    + AsPrimitive<f64>
    + Mul<Output = Self>
    + MulAssign
    + Add<Output = Self>
    + AddAssign
    + Div<Output = Self>
    + DivAssign
    + Sub<Output = Self>
    + SubAssign
    + Default
    + Zero
    + std::marker::Send
    + Sync
{
}
impl<T> TraitBounds for T where
    T: NormalizedFloat<T>
        + AsPrimitive<f32>
        + AsPrimitive<f64>
        + Mul<Output = T>
        + MulAssign
        + Add<Output = T>
        + AddAssign
        + Div<Output = T>
        + DivAssign
        + Sub<Output = T>
        + SubAssign
        + Default
        + Zero
        + std::marker::Send
        + Sync
{
}
