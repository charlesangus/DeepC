# M012-kho2ui: DeepCOpenDefocus Performance

## Vision
Eliminate the CPU render bottlenecks in DeepCOpenDefocus — singleton renderer, hoisted per-layer prep, full-image PlanarIop cache, and quality knob — so that a 256×256 single-layer deep scene renders in under 10 seconds on CPU.

## Slice Overview
| ID | Slice | Risk | Depends | Done | After this |
|----|-------|------|---------|------|------------|
| S01 | Singleton renderer + layer-peel dedup | high | — | ✅ | Same 256×256 test renders significantly faster — renderer created once, filter/mipmap/inpaint prep hoisted out of layer loop. cargo test passes. Timed comparison before/after. |
| S02 | Full-image PlanarIop cache + quality knob | medium | S01 | ✅ | DeepCOpenDefocus renders full deep image in one shot. Quality knob visible in Nuke. 256×256 single-layer renders under 10s. Docker build exits 0. |
