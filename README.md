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

What are the tools offered in the DeepC toolkit? The [Wiki](https://github.com/charlesangus/DeepC/wiki) has a description for each and an overview of all available [plugins](https://github.com/charlesangus/DeepC/wiki#nodes).

Also, head over to the [Examples](https://github.com/charlesangus/DeepC/wiki/Examples) page for some visuals.

You can read a quick description of some below.


- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCAdd.png)  [DeepCAdd](https://github.com/charlesangus/DeepC/wiki/DeepCAdd)
- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCAddChannels.png)  [DeepCAddChannels](https://github.com/charlesangus/DeepC/wiki/DeepCAddChannels)
- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCAdjustBBox.png)  [DeepCAdjustBBox](https://github.com/charlesangus/DeepC/wiki/DeepCAdjustBBox)
- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCClamp.png)  [DeepCClamp](https://github.com/charlesangus/DeepC/wiki/DeepCClamp)
- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCColorLookup.png)  [DeepCColorlookup](https://github.com/charlesangus/DeepC/wiki/DeepCColorlookup)
- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCConstant.png)  [DeepCConstant](https://github.com/charlesangus/DeepC/wiki/DeepCConstant)
- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCCopyBBox.png)  [DeepCCopyBBox](https://github.com/charlesangus/DeepC/wiki/DeepCCopyBBox)
- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCGamma.png)  [DeepCGamma](https://github.com/charlesangus/DeepC/wiki/DeepCGamma)
- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCGrade.png)  [DeepCGrade](https://github.com/charlesangus/DeepC/wiki/DeepCGrade)
- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCID.png)  [DeepCID](https://github.com/charlesangus/DeepC/wiki/DeepCID)
- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCHueShift.png)  [DeepCHueShift](https://github.com/charlesangus/DeepC/wiki/DeepCHueShift)
- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCInvert.png)  [DeepCInvert](https://github.com/charlesangus/DeepC/wiki/DeepCInvert)
- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCKeymix.png)  [DeepCKeymix](https://github.com/charlesangus/DeepC/wiki/DeepCKeymix)
- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCMatrix.png)  [DeepCMatrix](https://github.com/charlesangus/DeepC/wiki/DeepCMatrix)
- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCMultiply.png)  [DeepCMultiply](https://github.com/charlesangus/DeepC/wiki/DeepCMultiply)
- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCPMatte.png)  [DeepCPMatte](https://github.com/charlesangus/DeepC/wiki/DeepCPMatte)
- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCPNoise.png)  [DeepCPNoise](https://github.com/charlesangus/DeepC/wiki/DeepCPNoise)
- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCPosterize.png)  [DeepCPosterize](https://github.com/charlesangus/DeepC/wiki/DeepCPosterize)
- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCRemoveChannels.png)  [DeepCRemoveChannels](https://github.com/charlesangus/DeepC/wiki/DeepCRemoveChannels)
- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCSaturation.png)  [DeepCSaturation](https://github.com/charlesangus/DeepC/wiki/DeepCSaturation)
- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCShuffle.png)  [DeepCShuffle](https://github.com/charlesangus/DeepC/wiki/DeepCShuffle)
- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepThinner.png)  [DeepThinner](https://github.com/bratgot/DeepThinner) — Deep sample optimisation by [Marten Blumen](https://github.com/bratgot). MIT licensed.
- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCWorld.png)  [DeepCworld](https://github.com/charlesangus/DeepC/wiki/DeepCworld)

## Why DeepC?

DeepC has several advantages over DeepExpression-based solutions to working with Deep data.

### Unique Features

World-position noise in Nuke has always been limited by Nuke's 3D noise algorithm. It's fine for a 2D image, but when working with 3D noise, your really need that fourth dimension so the noise can change over time. DeepCPNoise brings 4D noise to Nuke for the first time, by including and extending the open-source FastNoise library. 4D simplex noise in Nuke!

### Familiar Tools, Familiar Power

DeepCGrade works just like the regular Grade node. You can mask it with rotos from the side input (incredibly, this is not available in the standard toolset, or with the DeepExpression node). You can use Deep masks coming in from the top. You can easily configure the channels to process and those to use as masks, just as you normally would.

### Endlessly Extensible

The power of DeepC is limited only by the power of the NDK. Features like DeepCAddChannels and DeepCShuffle were made possible using the NDK.

### Speed

Now that basic functionality is implemented, the focus will be on accelerating the speed of the toolset using the NDK.

## Get Them!

[Check the releases page for this repo](https://github.com/charlesangus/DeepC/releases) to get compiled versions of the plugins for Linux and Windows.

## Build

DeepC uses [NukeDockerBuild](https://github.com/charlesangus/NukeDockerBuild) for containerised cross-platform builds targeting **Nuke 16+**.

### Prerequisites

- [Docker](https://docs.docker.com/get-docker/)
- `git` and `zip` (for release archive creation)

> **Note:** On first run, NukeDockerBuild will download and build its Docker images (~1 GB Nuke installer download). You must accept the Foundry EULA when prompted.

### Usage

Build for both Linux and Windows (default Nuke 16.0):

```bash
./docker-build.sh
```

Build for a specific platform:

```bash
./docker-build.sh --linux
./docker-build.sh --windows
```

Build for specific Nuke versions:

```bash
./docker-build.sh --versions 16.0 16.1
```

### Output

Release archives are placed in the `release/` directory:

```
release/DeepC-Linux-Nuke16.0.zip
release/DeepC-Windows-Nuke16.0.zip
```

## Examples
We created a repository which includes some example deep render scenes to try/test/use this plugin.<br>
In futur we will add nuke project files to show how the plugins work.<br>
https://github.com/charlesangus/DeepCExamples

## Contributing

Currently, DeepC is maintained by [me](https://github.com/charlesangus) and [Falk Hofmann](https://github.com/falkhofmann). I'd love your contributions, though!

If you'd like to contribute, please fork the project make a new feature branch for your contribution. Ideally, also let me know what you're up to so we don't duplicate efforts!

Once you're happy with your work, submit a pull request so I can merge your work back in.

### Conventions

- 4-space hard tabs
- private/protected variable names start with '_' (one underscore)
- variable/function names use lowerCamelCase
