# DeepC

![DeepCAnglerfish](https://raw.githubusercontent.com/charlesangus/DeepC/master/icon/anglerfish_icon_medium_black.png?token=AEfZ8WAwpM_lo1i8A9D2iJY7ZDRI6tonks5cIuu-wA%3D%3D)

Welcome to the DeepC, a suite of Deep compositing plugins for Foundry's Nuke. While I'm sure large studios have something similar to these tools, this open-source project makes them available to small and mid-size studios.


## Plugins

What are the tools offered in the DeepC toolkit? You can read a quick description of each one below, and then head over to the [Examples](https://github.com/charlesangus/DeepC/wiki/Examples) page for some visuals.


### DeepCGrade

The familiar Grade node, updated to work with Deep data. Grading without masking isn't much use, so DeepCGrade supports both Deep and flat masks working in tandem, for maximum flexibility. Use DeepCPMatte or DeepCPNoise to create Deep masks upstream of DeepCGrade, or pipe in a roto in the side, just like you're used to doing.

### DeepCWorld

Generates world position data from straight Deep renders and a Camera node. Useful if your 3D render doesn't come with a Deep position pass. Just pipe in the Camera, choose the channels to stick the World Position data in, and there you are! Can output premultiplied position data (like most renderers), or unpremultiplied position data (like the ScanlineRender node).

Because DeepCWorld uses the NDK, it can add channels freely to the Deep stream without using hacks (like the Constant->Shuffle->DeepToImage->DeepMerge trick), and it can easily grab the data it needs from the Camera node just by plugging it in.

### DeepCPMatte

Generates a spherical or cubic mask from Deep position data, for use in other DeepC nodes. Outputs the mask in the channel of your choice. Like DeepCWorld, can create channels on the fly.

### DeepCPNoise

Generates a 3D or 4D noise mask from Deep position data, for use in other DeepC nodes. Much better than the builtin Nuke noise options, this is based on the FastNoise library. Critically, it offers 4D fractal noise, so your 3D noise pass can evolve across a 4th dimension over time without sliding.

Includes:

- 4D simplex noise
- 3D cellular noise (many varieties)
- 3D value noise
- 3D perlin noise
- 3D cubic noise

### DeepCID

Easily manipulate IDs in Deep data, combine with other masks, etc. Can take integer object IDs (better for Deep) or traditional colour mattes.

### DeepCAddChannels

Ever wanted to add a channel or channelset to a Deep stream? Well, now you can! And without doing that gross DeepToImage->DeepMerge hack.

### DeepCRemoveChannels

Ever had extra channels in a Deep stream, cluttering things up? Wish you could get rid of them? Look no further...

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

### DeepCSat

Coming soon...

A clone of the regular Saturation node, but Deep. Takes Deep masks in the B-stream and flat masks in the side input.

### DeepCShuffle

Coming soon...

Simplified version of the Shuffle node for Deep images.

### DeepCompress

Coming soon...

Much like how samples can be compressed in the renderer by merging samples closer than a certain threshold, this node will allow merging of samples in the Deep stream to cut down on processing time when the render is too heavy.

## Build

Build has been tested on Centos 6.8 with devtoolset-2 (as recommended in NDK dev guide). Compiles against 11.0-11.3, tested really only on 11.1. Let me know how it goes...

### Linux

Install prerequisites:

```bash
sudo wget http://people.centos.org/tru/devtools-2/devtools-2.repo -O /etc/yum.repos.d/devtools-2.repo
sudo yum install devtoolset-2-gcc devtoolset-2-binutils devtoolset-2-gcc-c++ mesa-libGLU-devel
```

You'll also need my fork of FastNoise to compile DeepCPNoise - instructions coming soon for that.

Add to .bashrc on dev machine, or run before each build:

```bash
# enable devtoolset-2
source /opt/rh/devtoolset-2/enable
```

### Windows

Possible someday...

### Mac

Unlikely...
