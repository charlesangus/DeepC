<h6 align="center">
  <br>
  <picture>
    <source media="(max-width: 768px)" srcset="https://codeberg.org/gillesvink/opendefocus/media/branch/main/resources/header_mobile.png">
    <img src="https://codeberg.org/gillesvink/opendefocus/media/branch/main/resources/header.png" alt="OpenDefocus" style="width: 100%;">
  </picture>
  <br></br>
  <p>Logo thanks to <a href="https://www.instagram.com/welmaakt/">Welmoed Boersma</a>!</p>
</h6>


<h4 align="center">An advanced open-source convolution library for image post-processing</h4>

<p align="center">
    <a href="https://crates.io/crates/opendefocus">
        <img src="https://img.shields.io/crates/l/opendefocus" alt="License" />
    </a>
    <a href="https://crates.io/crates/opendefocus">
        <img src="https://img.shields.io/crates/v/opendefocus" alt="Version" />
    </a>
    <a href = "https://codeberg.org/gillesvink/opendefocus/issues">
      <img alt="Gitea Issues" src="https://img.shields.io/gitea/issues/open/gillesvink/opendefocus?gitea_url=https%3A%2F%2Fcodeberg.org">
    </a>
      <a href="https://ci.codeberg.org/repos/15835">
        <img src="https://ci.codeberg.org/api/badges/15835/status.svg" alt="Tests" />
    </a>
    <a href="https://opendefocus.codeberg.page/download.html">
        <img src="https://img.shields.io/badge/nuke-13%2B-yellow?logo=nuke" alt="Nuke Versions" />
    </a>
</p>


---

<p align="center">
  <a href="https://donate.gillesvink.com" target="_blank">Donate</a> •
  <a href="#features">Features</a> •
  <a href="https://opendefocus.codeberg.page/download.html" target="_blank">Download</a> •
  <a href="https://opendefocus.codeberg.page">Documentation</a> •
  <a href="https://codeberg.org/gillesvink/opendefocus/issues" target="_blank">Issues</a> •
  <a href="./CHANGELOG.md" target="_blank">Changelog</a>
</p>

---

## Features

### User
* Entirely free! ([but please consider donating!](https://donate.gillesvink.com))
* Native integration for camera data to match convolution to real world camera data.
* GPU accelerated (Vulkan/Metal)
* Both simple 2D defocus as well as depth based (1/Z, real or direct math based)
* Custom quality option, for quick renders with less precision or heavier higher precision renders.
* Lots of non uniform artifacts:
  * [Catseye](https://opendefocus.codeberg.page/detailed/non_uniform/catseye.html)
  * [Barndoors](https://opendefocus.codeberg.page/detailed/non_uniform/barndoors.html)
  * [Astigmatism](https://opendefocus.codeberg.page/detailed/non_uniform/astigmatism.html)
  * [Axial aberration](https://opendefocus.codeberg.page/detailed/non_uniform/axial_aberration.html)
  * [Inversed bokehs in foreground](https://opendefocus.codeberg.page/detailed/non_uniform/inverse_foreground.html)
* Easy to use bokeh creator or use your own image
* Foundry Nuke native plugin (through [CXX](https://cxx.rs) FFI). Basically a wrapper around the Rust crate ([serves](./crates/opendefocus-nuke/) as a good developer reference on how to integrate it in other DCC's or applications!).


### Technical
* Process each pixel coordinate and channel on the image with a custom filter kernel. For a simple `RGBA` 1920x1080 image, that is at least 8.294.400 custom kernels!
* 100% written in pure Rust (stable channel) without external library dependencies.
* Same algorithm on GPU and CPU with same source code (thanks to Rust-GPU spirv compiler).
* Easy to use and open API to hook into your own application or DCC.
* Lots of control over the output, [take a look at all options available](https://docs.rs/opendefocus/latest/opendefocus/datamodel/index.html).

## Examples
### Simple convolution

```rust
use anyhow::Result;
use image::{DynamicImage, Rgba32FImage};
use image_ndarray::prelude::ImageArray;
use opendefocus::OpenDefocusRenderer;
use opendefocus::datamodel::Settings;

#[tokio::main]
async fn main() -> Result<()> {
    let mut settings = Settings::default();
    // set the defocus size in pixel radius, you can change whatever you want
    settings.defocus.circle_of_confusion.size = 25.0;

    // initialize a new renderer, this contains the runner instance (wgpu for example)
    // its fine to throw away if you only use one image, else its good to reuse to prevent initializing all the time
    let renderer = OpenDefocusRenderer::new(true, &mut settings).await?;

    // load an example image
    let image = image::load_from_memory(include_bytes!("../../examples/simple/toad.png"))?.to_rgba32f();
    let mut array = image.to_ndarray();

    // then here we actually render
    renderer
        .render(settings, array.view_mut(), None, None)
        .await?;

    // just some loading to the image crate once again after rendering and storing it
    let image: Rgba32FImage = Rgba32FImage::from_ndarray(array)?;
    let image = DynamicImage::from(image).to_rgba8();
    image.save("./result.png")?;
    Ok(())
}

```

## Structure
The project has multiple crates defined at the crates directory:

| Crate Name                                                       | Description                                                                                                                   |
| ---------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------- |
| [opendefocus](./crates/opendefocus)                              | The actual public library itself. The `main` crate.                                                                           |
| [opendefocus-datastructure](./crates/opendefocus-datastructure/) | Datastructure bindings to protobuf and implementations.                                                                       |
| [opendefocus-kernel](./crates/opendefocus-kernel/)               | Kernel (`no-std`) source code. Runs on both GPU and CPU.                                                                      |
| [opendefocus-nuke](./crates/opendefocus-nuke/)                   | Nuke specific source code. Includes both C++ and Rust.                                                                        |
| [opendefocus-shared](./crates/opendefocus-shared/)               | Code that can be used by both the [kernel](./crates/opendefocus-kernel/) and main [opendefocus](./crates/opendefocus/) crate. |
| [spirv-cli-build](./crates/spirv-cli-build/)                     | Wrapper around the SPIR-V from Rust-GPU to compile using nightly for `opendefocus-kernel`.                                    |


Besides that, these crates have been located outside of this repository as they have a bigger scope than just convolution:
| Crate Name                                                                 | Description                                                                                                                                             |
| -------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------- |
| [circle-of-confusion](https://codeberg.org/gillesvink/circle-of-confusion) | Circle of confusion algorithm to calculate the actual circle of confusion from real world camera data or create a nice depth falloff based on selection |
| [bokeh-creator](https://codeberg.org/gillesvink/bokeh-creator)             | Filter kernel generation library                                                                                                                        |
| [inpaint](https://codeberg.org/gillesvink/inpaint)                         | Telea inpainting algorithm in pure Rust                                                                                                                 |




## Build
You need to have Rust installed:
Both the stable version (1.92+) as well as the nightly version listed in [spirv-cli-build/rust-toolchain.toml](./crates/spirv-cli-build/rust-toolchain.toml)

That's it basically.

All compilation is handled through xtasks. Call `cargo xtask --help` for more information.

### Nuke
For Nuke building you need additional dependencies. As the linking process needs to have the DDImage library and headers installed, the xtask fetches these sources automatically.

For extracting of the archives, `msiextract` needs to be installed for Windows installs.

In theory C++11 is supported so it would work with Nuke 10 and higher, but that is not tested.

When compiling Nuke and fetching sources, to speed up downloading it is recommended to either use a machine in the USA or use a VPN. Because its an AWS bucket in the US-East region which does not have geo-replication enabled.

Example to compile Nuke 15:
```bash
cargo xtask \
  --compile \
  --nuke-versions 15.1,15.2 \
  --target-platform linux \
  --output-to-package
```

This will create the package with Linux compiled binaries in the [package](./package/).

Now you are able to copy this folder to some other location, or add it to your `NUKE_PATH` and it should show up.

```bash
export NUKE_PATH=$(pwd)/package

# launch your nuke etc...
```
