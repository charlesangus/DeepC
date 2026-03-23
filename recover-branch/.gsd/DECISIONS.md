# Decisions Register

<!-- Append-only. Never edit or remove existing rows.
     To reverse a decision, add a new row that supersedes it.
     Read this file at the start of any planning or research phase. -->

| # | When | Scope | Decision | Choice | Rationale | Revisable? |
|---|------|-------|----------|--------|-----------|------------|
| D001 | M002 | arch | DeepCBlur base class | Direct DeepFilterOp subclass | DeepPixelOp prohibits spatial ops; DeepCWrapper can't expand input bbox; DeepFilterOp gives full control over getDeepRequests() and doDeepEngine() | No |
| D002 | M002 | arch | Blur filter type | Gaussian only | User specified Gaussian only. Keeps implementation focused and knob layout simple. Non-Gaussian types out of scope. | Yes — if user requests box/triangle later |
| D003 | M002 | arch | Size knob layout | Separate width and height float knobs | User specified separate w/h for non-uniform (anisotropic) blur. Matches Nuke built-in Blur pattern. | No |
| D004 | M002 | arch | Channel selection | Blur all channels, no channel selector knob | Deep images carry all channels per-sample; blurring a subset creates inconsistency between blurred and unblurred channels within the same sample. User explicitly rejected channel selection. | Yes — if a valid per-sample-channel-subset use case emerges |
| D005 | M002 | arch | Sample propagation | Propagate samples into empty neighboring pixels | Without propagation, blur is invisible in sparse regions. Propagation creates new samples in empty pixels with Gaussian-weighted color from source samples. Depth values propagate from source. | No |
| D006 | M002 | arch | Sample optimization location | Per-pixel inside doDeepEngine(), not as a separate downstream node | Doing optimization per-pixel before writing to the output plane avoids materializing inflated sample counts in Nuke's tile cache. Avoids memory spike between DeepCBlur and downstream nodes. | Yes — if a post-blur optimization node proves more flexible |
| D007 | M002 | pattern | Sample optimization modularity | Shared DeepSampleOptimizer.h header, reusable by future deep spatial ops | User emphasized modularity so future ops (DeepDefocus, etc.) can reuse the same optimization logic without code duplication. | No |
| D008 | M002/S01 | arch | Gaussian kernel implementation | Full 2D non-separable kernel | Separable (horizontal then vertical) decomposition is complex for variable-length sample arrays per pixel — samples at different depths can't be trivially split into two 1D passes. Full 2D is simpler and correct for v1. O(radius²) cost accepted; large radii will be slow. | Yes — if performance at large radii becomes a user complaint |
| D009 | M002/S01 | arch | SampleRecord channel storage in DeepSampleOptimizer.h | std::vector<float> channels instead of fixed rgb[3] | DeepThinner's fixed rgb[3] only handles 3-channel merge. Deep images may carry arbitrary channel sets (rgba, deep, motion vectors, etc.). Vector-based storage makes the optimizer work for any channel configuration without recompilation. | No |
