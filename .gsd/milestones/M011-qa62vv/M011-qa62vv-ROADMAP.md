# M011-qa62vv: 

## Vision
Fork the opendefocus convolution kernel to evaluate holdout transmittance inside the gather loop — for each output pixel, each gathered input sample is attenuated by T(output_pixel, sample_depth) before accumulation. Sharp holdout edges, depth-correct, full bokeh disc coverage.

## Slice Overview
| ID | Slice | Risk | Depends | Done | After this |
|----|-------|------|---------|------|------------|
| S01 | Fork opendefocus-kernel with holdout gather | high | — | ✅ | Forked kernel compiles to SPIR-V and native Rust with holdout binding. CPU path unit test shows T lookup attenuates gathered samples correctly at output pixel per sample depth. |
| S02 | Wire holdout through full stack and integration test | medium | S01 | ✅ | DeepCOpenDefocus in Nuke: holdout test shows z=5 disc cut sharply by holdout at z=3, z=2 disc passes through uncut. Docker build exits 0. |
