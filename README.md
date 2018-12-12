# DeepC

Suite of Deep compositing plugins for Foundry's Nuke.

## Plugins

### DeepCWorldPosition

Generates world position data from straight Deep renders.

### DeepCPMatteGrade

Grades Deep images based on a spherical or cubic mask from a position pass.

## DeepCPNoiseGrade

Grades Deep images based on noise generated from a position pass.

### DeepCGrade

Coming soon...

A clone of the regular Grade node, but with added options for Deep. Improves on the DeepColorCorrect node by providing a simpler, more familiar interface, as well as extended masking options.

### DeepCAddChannels

Coming soon...

### DeepCRemoveChannels

Coming soon...

### DeepCID

Coming soon...

Easily manipulate IDs in Deep data, combine with other masks, etc.

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
