# 2026-09-22 — The dev host has no AVX2: local builds pass `DEEPC_DEFOCUS_ISA_FLAGS=-mavx;-mfma`

**Context.** The reprovisioned host is an AMD FX-8350 (Piledriver): AVX, FMA3, FMA4, XOP — no AVX2.
The scatter TU's `-mavx2 -mfma` made every render that reached the scatter kernel SIGILL.

**Decision (user, 2026-09-22).** A cache option, `DEEPC_DEFOCUS_ISA_FLAGS` (default `-mavx2;-mfma`,
so docker/release builds are unchanged), committed as `8da4f9d`. Every local configure on this host
adds `-D "DEEPC_DEFOCUS_ISA_FLAGS=-mavx;-mfma"`. `-ffp-contract=off` stays unconditional.

**Limit.** Bit-identity between the AVX and AVX2 builds cannot be *measured* here — an AVX2 binary
cannot run on this CPU. The argument for identity is structural: contraction is off, and the
scatter loop is an element-wise multiply-add with no reduction, so vector width does not change
rounding. `objdump` shows zero AVX2-only instructions in the local `.so`. Treat any future
local-vs-docker output difference as possibly ISA-caused until shown otherwise.
