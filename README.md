# DeepC

![DeepCAnglerfish](https://raw.githubusercontent.com/charlesangus/DeepC/master/icon/anglerfish_icon_medium_black.png?token=AEfZ8WAwpM_lo1i8A9D2iJY7ZDRI6tonks5cIuu-wA%3D%3D)

Welcome to the DeepC, a suite of Deep compositing plugins for Foundry's Nuke. While I'm sure large studios have something similar to these tools, this open-source project makes them available to small and mid-size studios.

## Get Them!

![Check the releases page for this repo](https://github.com/charlesangus/DeepC/releases) to get the compiled versions of the plugins. Currently, I've only compiled them for Linux, but that's presumably the most useful for folks, anyway. Windows versions as and when I can, if I can. If you'd like to compile for Mac and contribute back, that would be much appreciated, as I don't have a Mac for development.

## Plugins

### DeepCGrade

A clone of the regular Grade node, but with added options for Deep. Integrates with DeepCPMatte and DeepCPNoise by optionally taking Deep masks from the B-stream. Takes a flat image as a mask in the side input.

### DeepCWorldPosition

Generates world position data from straight Deep renders. Useful if your 3D render doesn't come with a Deep position pass.

### DeepCPMatte

Generates a spherical or cubic mask from Deep position data, for use in other DeepC nodes.

### DeepCPNoise

Generates a 3D or 4D noise mask from Deep position data, for use in other DeepC nodes. Much better than the builtin Nuke noise options, this is based on the FastNoise library. Critically, it offers 4D fractal noise, so your 3D noise pass can evolve across a 4th dimension over time without sliding.

Includes:

- 4D simplex noise
- 3D cellular noise (many varieties)
- 3D value noise
- 3D perlin noise
- 3D cubic noise

## Future Plans

### DeepCAddChannels

Coming soon...

### DeepCRemoveChannels

Coming soon...

### DeepCSat

Coming soon...

A clone of the regular Saturation node, but Deep. Takes Deep masks in the B-stream and flat masks in the side input.

### DeepCID

Coming soon...

Easily manipulate IDs in Deep data, combine with other masks, etc.

### DeepCShuffle

Coming soon...

Simplified version of the Shuffle node for Deep images.


## Build

Build has been tested on Centos 6.8 with devtoolset-2 (as recommended in NDK dev guide). Compiles against 11.0-11.3, tested really only on 11.1. Let me know how it goes...

### Linux

Install prerequisites:

```bash
sudo wget http://people.centos.org/tru/devtools-2/devtools-2.repo -O /etc/yum.repos.d/devtools-2.repo
sudo yum install devtoolset-2-gcc devtoolset-2-binutils devtoolset-2-gcc-c++ mesa-libGLU-devel
```

Add to .bashrc on dev machine, or run before each build:

```bash
# enable devtoolset-2
source /opt/rh/devtoolset-2/enable
```

### Windows

Possible someday...

### Mac

Unlikely...
