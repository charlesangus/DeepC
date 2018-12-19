# DeepC

Suite of Deep compositing plugins for Foundry's Nuke.

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

Build has been tested on Centos 6.8 with devtoolset-2 (as recommended in NDK dev guide) and Nuke 11.1.

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
