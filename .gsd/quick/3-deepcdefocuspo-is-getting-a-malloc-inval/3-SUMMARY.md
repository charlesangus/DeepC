---
task: 3
slug: deepcdefocuspo-is-getting-a-malloc-inval
status: complete
commit: 7e95ed2
branch: gsd/quick/3-deepcdefocuspo-is-getting-a-malloc-inval
---

# Quick Task 3 — DeepCDefocusPO malloc(): invalid size (unsorted)

## Root Cause

`renderStripe` called `imagePlane.writableAt(x, y, channel)` for all four RGBA channels unconditionally. `ImagePlane` only allocates memory for the channels in its `ChannelSet`; writing a channel outside that set computes an invalid stride-based offset, silently corrupting adjacent heap. The crash — `malloc(): invalid size (unsorted)` — manifests at the *next* allocation after `renderStripe` returns, not at the write site.

Nuke can call `renderStripe` requesting any subset of RGBA (e.g., just alpha for a downstream depth-keying node), so this is a load-bearing check, not a defensive one.

## Fix

Two guards added in `renderStripe`:
- `if (!chans.contains(rgb_chans[c])) continue;` before each RGB channel splat.
- `if (chans.contains(Chan_Alpha))` wrapping the alpha splat.

`chans` is the `const ChannelSet& chans = imagePlane.channels()` already in scope at the top of `renderStripe`.

## Side Fix

`ChannelSet` mock in `scripts/verify-s01-syntax.sh` was missing `bool contains(Channel) const`. Added stub. The method is used in many other plugins but was never exercised by those plugins' syntax checks with the mock.

## Files Changed

- `src/DeepCDefocusPO.cpp` — writableAt guards
- `scripts/verify-s01-syntax.sh` — ChannelSet::contains() stub
