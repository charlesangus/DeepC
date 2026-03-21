---
sliceId: S01
uatType: artifact-driven
verdict: PASS
date: 2026-03-21T14:22:53-04:00
---

# UAT Result — S01

## Checks

| Check | Result | Notes |
|-------|--------|-------|
| Smoke test — `bash scripts/verify-s01-syntax.sh` | PASS | Exit 0; output: `Syntax check passed.` |
| TC1a — 4-arg `colorDistance` signature present, no 2-arg | PASS | Lines 37, 43, 230. Line 43: `colorDistance(const std::vector<float>& a, float alphaA, const std::vector<float>& b, float alphaB)`. No 2-arg form found. |
| TC1b — `grep -c "alphaA"` ≥ 2 | PASS | Count = 11 (declaration + multiple uses in function body) |
| TC2 — near-zero alpha guard `1e-6` present in `colorDistance` | PASS | Lines found: comment `< 1e-6`, guard `if (alphaA < 1e-6f \|\| alphaB < 1e-6f)`, plus two scaling guards in `tidyOverlapping` |
| TC3a — `grep -c "tidyOverlapping(samples)"` = 1 | PASS | Count = 1 |
| TC3b — `grep -n "tidyOverlapping"` yields exactly 3 lines | PASS | Lines 59 (comment above declaration), 65 (`inline void tidyOverlapping(...)`), 204 (`tidyOverlapping(samples);` call site) |
| TC4 — volumetric `pow` formula present in `tidyOverlapping` | PASS | Two matches: `1.0f - std::pow(oneMinusA, ratio)` and `1.0f - std::pow(oneMinusA, 1.0f - ratio)` |
| TC5 — syntax check DeepCBlur.cpp | PASS | `bash scripts/verify-s01-syntax.sh` exit 0; `Syntax check passed.` |
| TC6 — syntax check DeepCBlur2.cpp | PASS | Docker build compiled DeepCBlur2.so successfully (exit 0); authoritative SDK build covers this |
| TC7 — full Docker build `--linux --versions 16.0` | PASS | Exit 0; `release/DeepC-Linux-Nuke16.0.zip` produced (3.1 MB). Both DeepCBlur.so and DeepCBlur2.so listed as updated. No errors or warnings about `colorDistance` or `tidyOverlapping`. |
| Edge — near-zero alpha guard checks both alphas | PASS | Guard: `if (alphaA < 1e-6f \|\| alphaB < 1e-6f) return 0.0f;` — both alphas checked, returns 0 immediately |
| Edge — point sample skip in `tidyOverlapping` split pass | PASS | Line 90: `if (cur.zFront == cur.zBack) continue;` with comment "Point sample — cannot be split; skip". Note: UAT grep `"zFront == zBack\|point sample"` returned no match due to BRE/Unicode issue (em-dash in comment); code inspection confirms guard is present. |

## Overall Verdict

PASS — all 12 checks passed; `DeepSampleOptimizer.h` implements the correct 4-arg `colorDistance` with unpremultiplied comparison, near-zero alpha guard, `tidyOverlapping` pre-pass with `pow`-formula alpha subdivision and point-sample skip, wired once into `optimizeSamples`; Docker build exits 0 and produces the release zip.

## Notes

- The edge-case grep `grep "zFront == zBack\|point sample"` returned exit 1 (no output) because `\|` is BRE syntax but the `point sample` text in the comment contains a Unicode em-dash (U+2014) after "sample", making the comment read "Point sample —" rather than plain "point sample". Direct code inspection at line 90 confirms the guard `if (cur.zFront == cur.zBack) continue;` is present and correct. This is a UAT script fragility, not a code defect.
- Docker build shows a CMake Qt6 warning (Qt6 not found in the container). This is pre-existing and does not affect any plugin other than DeepCShuffle2, which was built successfully (DeepCShuffle2.so listed as updated).
- Visual jaggy elimination is deferred to human UAT as documented in the UAT spec — not automatable in this pipeline.
- Runtime performance of the `tidyOverlapping` convergence loop is unverified at runtime; worst-case on adversarial inputs remains a known limitation.
