//! Source code for the chunk handler.
//! It splits the image into chunks that is able to be received by the GPU according to its limits.

use glam::IVec4;
use opendefocus_datastructure::render::RenderSpecs;

/// Object that handles splitting and merging of chunks
pub struct ChunkHandler {
    render_specs: RenderSpecs,
    limit: u32,
    padding: i32,
}

impl ChunkHandler {
    /// Create a new ChunkHandler with the given parameters.
    pub fn new(render_specs: &RenderSpecs, padding: i32) -> Self {
        Self {
            render_specs: *render_specs,
            limit: 4096,
            padding,
        }
    }

    /// Get the render specifications for the chunks.
    ///
    /// # Returns
    ///
    /// * `Vec<RenderSpecs>` - The render specifications.
    pub fn get_render_specs(&self) -> Vec<RenderSpecs> {
        let resolution = self.render_specs.get_resolution();
        if resolution.x <= self.limit && resolution.y <= self.limit {
            return vec![self.render_specs];
        }

        let mut specs = Vec::new();

        let mut y_boundary = self.render_specs.full_region.y;
        let mut unclamped_y_boundary = self.render_specs.full_region.y - self.padding;
        while y_boundary < self.render_specs.full_region.w {
            let mut x_boundary = self.render_specs.full_region.x;
            let mut unclamped_x_boundary = self.render_specs.full_region.x - self.padding;

            let y_end = y_boundary + self.limit as i32;

            while x_boundary < self.render_specs.full_region.z {
                let x_end = x_boundary + self.limit as i32;

                let full = IVec4::new(
                    x_boundary,
                    y_boundary,
                    x_end.min(self.render_specs.full_region.z),
                    y_end.min(self.render_specs.full_region.w),
                );
                let render = IVec4::new(
                    (unclamped_x_boundary + self.padding).max(self.render_specs.render_region.x),
                    (unclamped_y_boundary + self.padding).max(self.render_specs.render_region.y),
                    (x_end - self.padding).min(self.render_specs.render_region.z),
                    (y_end - self.padding).min(self.render_specs.render_region.w),
                );
                x_boundary = x_end - self.padding * 2;
                unclamped_x_boundary = x_end - self.padding * 2;
                let chunk = RenderSpecs::from_rects(full, render);
                specs.push(chunk);
            }
            y_boundary = y_end - self.padding * 2;
            unclamped_y_boundary = y_end - self.padding * 2;
        }
        specs
    }
}

#[cfg(test)]
mod tests {
    use opendefocus_datastructure::{IVector4, render::RenderSpecs};

    use super::ChunkHandler;

    #[test]
    fn test_get_render_specs_return_same() {
        let render_spec = RenderSpecs {
            full_region: IVector4 {
                x: 0,
                y: 0,
                z: 1920,
                w: 1080,
            },
            render_region: IVector4 {
                x: 0,
                y: 0,
                z: 1920,
                w: 1080,
            },
        };

        let test_chunk_handler = ChunkHandler::new(&render_spec, 100);
        assert_eq!(test_chunk_handler.get_render_specs(), vec![render_spec]);
    }

    #[test]
    fn test_get_multiple_render_specs_without_padding() {
        let render_spec = RenderSpecs {
            full_region: IVector4 {
                x: 0,
                y: 0,
                z: 8096,
                w: 1080,
            },
            render_region: IVector4 {
                x: 0,
                y: 0,
                z: 8096,
                w: 1080,
            },
        };
        let test_chunk_handler = ChunkHandler::new(&render_spec, 0);
        assert_eq!(
            test_chunk_handler.get_render_specs(),
            vec![
                RenderSpecs {
                    full_region: IVector4 {
                        x: 0,
                        y: 0,
                        z: 4096,
                        w: 1080
                    },
                    render_region: IVector4 {
                        x: 0,
                        y: 0,
                        z: 4096,
                        w: 1080
                    },
                },
                RenderSpecs {
                    full_region: IVector4 {
                        x: 4096,
                        y: 0,
                        z: 8096,
                        w: 1080
                    },
                    render_region: IVector4 {
                        x: 4096,
                        y: 0,
                        z: 8096,
                        w: 1080
                    },
                },
            ]
        )
    }

    #[test]
    fn test_get_multiple_render_specs_with_padding_horizontal() {
        let render_spec = RenderSpecs {
            full_region: IVector4 {
                x: 0,
                y: 0,
                z: 8096,
                w: 1080,
            },
            render_region: IVector4 {
                x: 0,
                y: 0,
                z: 8096,
                w: 1080,
            },
        };
        let test_chunk_handler = ChunkHandler::new(&render_spec, 100);
        assert_eq!(
            test_chunk_handler.get_render_specs(),
            vec![
                RenderSpecs {
                    full_region: IVector4 {
                        x: 0,
                        y: 0,
                        z: 4096,
                        w: 1080
                    },
                    render_region: IVector4 {
                        x: 0,
                        y: 0,
                        z: 3996,
                        w: 1080
                    },
                },
                RenderSpecs {
                    full_region: IVector4 {
                        x: 3896,
                        y: 0,
                        z: 7992,
                        w: 1080
                    },
                    render_region: IVector4 {
                        x: 3996,
                        y: 0,
                        z: 7892,
                        w: 1080
                    },
                },
                RenderSpecs {
                    full_region: IVector4 {
                        x: 7792,
                        y: 0,
                        z: 8096,
                        w: 1080
                    },
                    render_region: IVector4 {
                        x: 7892,
                        y: 0,
                        z: 8096,
                        w: 1080
                    },
                },
            ]
        )
    }

    #[test]
    fn test_get_multiple_render_specs_with_padding_vertical() {
        let render_spec = RenderSpecs {
            full_region: IVector4 {
                x: 0,
                y: 0,
                z: 1080,
                w: 8096,
            },
            render_region: IVector4 {
                x: 0,
                y: 0,
                z: 1080,
                w: 8096,
            },
        };
        let test_chunk_handler = ChunkHandler::new(&render_spec, 100);
        assert_eq!(
            test_chunk_handler.get_render_specs(),
            vec![
                RenderSpecs {
                    full_region: IVector4 {
                        x: 0,
                        y: 0,
                        z: 1080,
                        w: 4096
                    },
                    render_region: IVector4 {
                        x: 0,
                        y: 0,
                        z: 1080,
                        w: 3996
                    },
                },
                RenderSpecs {
                    full_region: IVector4 {
                        x: 0,
                        y: 3896,
                        z: 1080,
                        w: 7992
                    },
                    render_region: IVector4 {
                        x: 0,
                        y: 3996,
                        z: 1080,
                        w: 7892
                    },
                },
                RenderSpecs {
                    full_region: IVector4 {
                        x: 0,
                        y: 7792,
                        z: 1080,
                        w: 8096
                    },
                    render_region: IVector4 {
                        x: 0,
                        y: 7892,
                        z: 1080,
                        w: 8096
                    },
                },
            ]
        )
    }

    #[test]
    fn test_get_multiple_render_specs_with_padding_combined() {
        let render_spec = RenderSpecs {
            full_region: IVector4 {
                x: 0,
                y: 0,
                z: 8096,
                w: 8096,
            },
            render_region: IVector4 {
                x: 0,
                y: 0,
                z: 8096,
                w: 8096,
            },
        };
        let test_chunk_handler = ChunkHandler::new(&render_spec, 100);
        assert_eq!(
            test_chunk_handler.get_render_specs(),
            vec![
                RenderSpecs {
                    full_region: IVector4 {
                        x: 0,
                        y: 0,
                        z: 4096,
                        w: 4096
                    },
                    render_region: IVector4 {
                        x: 0,
                        y: 0,
                        z: 3996,
                        w: 3996
                    },
                },
                RenderSpecs {
                    full_region: IVector4 {
                        x: 3896,
                        y: 0,
                        z: 7992,
                        w: 4096
                    },
                    render_region: IVector4 {
                        x: 3996,
                        y: 0,
                        z: 7892,
                        w: 3996
                    },
                },
                RenderSpecs {
                    full_region: IVector4 {
                        x: 7792,
                        y: 0,
                        z: 8096,
                        w: 4096
                    },
                    render_region: IVector4 {
                        x: 7892,
                        y: 0,
                        z: 8096,
                        w: 3996
                    },
                },
                RenderSpecs {
                    full_region: IVector4 {
                        x: 0,
                        y: 3896,
                        z: 4096,
                        w: 7992
                    },
                    render_region: IVector4 {
                        x: 0,
                        y: 3996,
                        z: 3996,
                        w: 7892
                    },
                },
                RenderSpecs {
                    full_region: IVector4 {
                        x: 3896,
                        y: 3896,
                        z: 7992,
                        w: 7992
                    },
                    render_region: IVector4 {
                        x: 3996,
                        y: 3996,
                        z: 7892,
                        w: 7892
                    },
                },
                RenderSpecs {
                    full_region: IVector4 {
                        x: 7792,
                        y: 3896,
                        z: 8096,
                        w: 7992
                    },
                    render_region: IVector4 {
                        x: 7892,
                        y: 3996,
                        z: 8096,
                        w: 7892
                    },
                },
                RenderSpecs {
                    full_region: IVector4 {
                        x: 0,
                        y: 7792,
                        z: 4096,
                        w: 8096
                    },
                    render_region: IVector4 {
                        x: 0,
                        y: 7892,
                        z: 3996,
                        w: 8096
                    },
                },
                RenderSpecs {
                    full_region: IVector4 {
                        x: 3896,
                        y: 7792,
                        z: 7992,
                        w: 8096
                    },
                    render_region: IVector4 {
                        x: 3996,
                        y: 7892,
                        z: 7892,
                        w: 8096
                    },
                },
                RenderSpecs {
                    full_region: IVector4 {
                        x: 7792,
                        y: 7792,
                        z: 8096,
                        w: 8096
                    },
                    render_region: IVector4 {
                        x: 7892,
                        y: 7892,
                        z: 8096,
                        w: 8096
                    },
                },
            ]
        )
    }
}
