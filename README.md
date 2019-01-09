# DeepC

![DeepCAnglerfish](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/anglerfish_icon_medium_black.png)

Welcome to the DeepC, a suite of Deep compositing plugins for Foundry's Nuke. While I'm sure large studios have something similar to these tools, this open-source project makes them available to small and mid-size studios.

## TOC

- [Plugins](https://github.com/charlesangus/DeepC#plugins)
- [Why DeepC?](https://github.com/charlesangus/DeepC#why-deepc)
- [Get Them!](https://github.com/charlesangus/DeepC#get-them)
- [Build](https://github.com/charlesangus/DeepC#build)
- [Contributing](https://github.com/charlesangus/DeepC#contributing)

## Plugins

What are the tools offered in the DeepC toolkit? You can read a quick description of each one below, and then head over to the [Examples](https://github.com/charlesangus/DeepC/wiki/Examples) page for some visuals.


### ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCGrade.png) DeepCGrade

The familiar Grade node, updated to work with Deep data. Grading without masking isn't much use, so DeepCGrade supports both Deep and flat masks working in tandem, for maximum flexibility. Use DeepCPMatte or DeepCPNoise to create Deep masks upstream of DeepCGrade, or pipe in a roto in the side, just like you're used to doing.

### ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCSaturation.png) DeepCSaturation

Simple rec709 saturation function on Deep data. Same masking options as DeepCGrade.

### ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCWorld.png) DeepCWorld

Generates world position data from straight Deep renders and a Camera node. Useful if your 3D render doesn't come with a Deep position pass. Just pipe in the Camera, choose the channels to stick the World Position data in, and there you are! Can output premultiplied position data (like most renderers), or unpremultiplied position data (like the ScanlineRender node).

Because DeepCWorld uses the NDK, it can add channels freely to the Deep stream without using hacks (like the Constant->Shuffle->DeepToImage->DeepMerge trick), and it can easily grab the data it needs from the Camera node just by plugging it in.

### ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCPMatte.png) DeepCPMatte

Generates a spherical or cubic mask from Deep position data, for use in other DeepC nodes. Outputs the mask in the channel of your choice. Like DeepCWorld, can create channels on the fly.

### ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCPNoise.png) DeepCPNoise

Generates a 3D or 4D noise mask from Deep position data, for use in other DeepC nodes. Much better than the builtin Nuke noise options, this is based on the FastNoise library. Critically, it offers 4D fractal noise, so your 3D noise pass can evolve across a 4th dimension over time without sliding.

Includes:

- 4D simplex noise
- 3D cellular noise (many varieties)
- 3D value noise
- 3D perlin noise
- 3D cubic noise

### ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCID.png) DeepCID

Easily manipulate IDs in Deep data, combine with other masks, etc. Can take integer object IDs (better for Deep) or traditional colour mattes.

### ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCAddChannels.png) DeepCAddChannels

Ever wanted to add a channel or channelset to a Deep stream? Well, now you can! And without doing that gross DeepToImage->DeepMerge hack.

### ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCRemoveChannels.png) DeepCRemoveChannels

Ever had extra channels in a Deep stream, cluttering things up? Wish you could get rid of them? Look no further...

### ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCShuffle.png) DeepCShuffle

Very basic Shuffle node for copying/blanking channels.

### DeepCBlink

Just a toy at the moment which performs a gain on the image, but demonstrates an approach to getting Blink working on Deep images. This approach works for kernels which need only the current pixel to operate. Experimental proof-of-concept, not really "useful" as a node.

## Why DeepC?

DeepC has several advantages over DeepExpression-based solutions to working with Deep data.

### Unique Features

World-position noise in Nuke has always been limited by Nuke's 3D noise algorithm. It's fine for a 2D image, but when working with 3D noise, your really need that fourth dimension so the noise can change over time. DeepCPNoise brings 4D noise to Nuke for the first time, by including and extending the open-source FastNoise library. 4D simplex noise in Nuke!

### Familiar Tools, Familiar Power

DeepCGrade works just like the regular Grade node. You can mask it with rotos from the side input (incredibly, this is not available in the standard toolset, or with the DeepExpression node). You can use Deep masks coming in from the top. You can easily configure the channels to process and those to use as masks, just as you normally would.

### Endlessly Extensible

The power of DeepC is limited only by the power of the NDK. Upcoming features like DeepCAddChannels and DeepCShuffle are only possible using the NDK.

### Speed

Now that basic functionality is implemented, the focus will be on accelerating the speed of the toolset using the new NDK funcitonality available in Nuke 11.3.

## Get Them!

[Check the releases page for this repo](https://github.com/charlesangus/DeepC/releases) to get the compiled versions of the plugins. Currently, I've only compiled them for Linux, but that's presumably the most useful for folks, anyway. Windows versions as and when I can, if I can. If you'd like to compile for Mac and contribute back, that would be much appreciated, as I don't have a Mac for development.

## Future Plans

### DeepCompress

Coming soon...

Much like how samples can be compressed in the renderer by merging samples closer than a certain threshold, this node will allow merging of samples in the Deep stream to cut down on processing time when the render is too heavy.

## Build

Build has been tested on Centos 6.8 with devtoolset-2 (as recommended in NDK dev guide). Compiles against 11.2-11.3, tested really only on 11.2. Let me know how it goes...

### Linux

Install prerequisites:

```bash
sudo wget http://people.centos.org/tru/devtools-2/devtools-2.repo -O /etc/yum.repos.d/devtools-2.repo
sudo yum install devtoolset-2-gcc devtoolset-2-binutils devtoolset-2-gcc-c++ mesa-libGLU-devel
```

Clone:

```bash
git clone --recurse-submodules https://github.com/charlesangus/FastNoise
```

Add to .bashrc on dev machine, or run before each build:

```bash
# enable devtoolset-2
source /opt/rh/devtoolset-2/enable
```

Then, you should be able to run 

```bash
make release -jX
```

Where ```X``` is the number of cores you have available, so make can run parallelized.

### Windows

Possible someday...

### Mac

For Nuke 11, you must have Xcode 8.2 installed with clang (you may be able to use a slightly different version) - see [here for details from Foundry](https://learn.foundry.com/nuke/developers/113/ndkdevguide/appendixa/osx.html). Installing the smaller "Xcode Command Line" also appears to work well, using the command `xcode-select --install`

For older versions of Nuke (10.5 and earlier) the plugins must be built with GCC, not Clang. [This page](https://learn.foundry.com/nuke/developers/105/ndkdevguide/appendixa/osx.html) recommends you use GCC 4.0, but GCC 4.2 appears to work perfectly. However acquiring either of these ancient versions can be a challenge (building the compiler from source is slow but may be the easiest way)

With the appropriate compiler installed, to build:

    mkdir build
    cd build
    cmake -D CMAKE_INSTALL_PREFIX=inst -D NUKE_INSTALL_PATH=/Applications/Nuke11.3v1/Nuke11.3v1.app/Contents/MacOS/ ..
    make install

The path must be changed to the installed version of Nuke. It should be the folder *which contains* the `include/` folder.

## Contributing

Currently, DeepC is maintained by only me. I'd love your contributions, though!

If you'd like to contribute, please fork the project make a new feature branch for your contribution. Ideally, also let me know what you're up to so we don't duplicate efforts!

Once you're happy with your work, submit a pull request so I can merge your work back in.

### Conventions

- 4-space hard tabs
- private/protected variable names start with '_' (one underscore)
- variable/function names use lowerCamelCase
