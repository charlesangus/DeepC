#![warn(unused_extern_crates)]
#![cfg_attr(target_arch = "spirv", no_std)]
#![cfg_attr(
    target_arch = "spirv",
    allow(internal_features),
    feature(asm_experimental_arch, core_intrinsics, lang_items, repr_simd)
)]

// Convencience macros for branchless structure and compile-time optimization
//
// Basically sort of same as inline, but with the benefit of forcing the compiler to do so
// With the cost of larger binary size, sorry! Performance is more important here imho.

#[macro_export]
macro_rules! f32_from_bool {
    ($a:expr) => {{
        #[cfg(not(target_arch = "spirv"))]
        { f32::from($a) }
        #[cfg(target_arch = "spirv")]
        { $a as u32 as f32 }
    }};
}

/// Branchless if else expression
#[macro_export]
macro_rules! if_else {
    ($condition:expr, $then:expr, $otherwise:expr) => {{ (f32_from_bool!($condition) * ($then)) + ((!$condition as u32 as f32) * ($otherwise)) }};
}

#[macro_export]
macro_rules! max {
    ($a:expr, $b:expr) => {{ if_else!($a > $b, $a, $b) }};
}

#[macro_export]
macro_rules! min {
    ($a:expr, $b:expr) => {{ if_else!($a < $b, $a, $b) }};
}

#[macro_export]
macro_rules! clamp {
    ($value:expr, $low:expr, $high:expr) => {{ min!(max!($value, $low), $high) }};
}


use glam::UVec2;
mod internal_settings;
pub use crate::internal_settings::{
    AxialAberration, AxialAberrationType, ConvolveSettings, GlobalFlags, NonUniformFlags,
};
pub mod math;

#[cfg(not(any(target_arch = "spirv")))]
pub mod cpu_image;

pub const WORKGROUP_SIZE: u32 = 16;
pub const OUTPUT_CHANNELS: usize = 5;

#[derive(Copy, Clone, Debug)]
pub struct ThreadId {
    x: u32,
    y: u32,
}

impl ThreadId {
    pub fn new(x: u32, y: u32) -> Self {
        Self { x, y }
    }

    /// Calculates 2D coordinates from a thread ID based on the resolution.
    pub fn get_coordinates(&self) -> UVec2 {
        UVec2::new(
            self.x,
            self.y,
        )
    }
}

