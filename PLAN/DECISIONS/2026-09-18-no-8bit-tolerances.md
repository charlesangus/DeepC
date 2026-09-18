# No 8-bit-derived tolerances; solid alpha means exactly 1

**Decision (user ruling, 2026-09-18, M6 planning):** no test, gate, plan brief, or acceptance bar in
this project cites `1/255` or any other 8-bit-derived figure as a tolerance. The pipeline is 32-bit
float end to end; "solid alpha" means `alpha == 1.0`. Where an accumulation cannot be bit-exact, the
slack is stated as an ulp bound derived from the term count (`N·2⁻²⁴`, the m1/l6 idiom), with the
derivation in the pin, and it is measured against an independent oracle — never against the new
output.

**Rationale:** an 8-bit tolerance is a display-era idea leaking into float image processing; it
would silently accept the class of defect M6/M7 exist to remove (alpha dips of a few thousandths on
opaque geometry) and it has no relation to the arithmetic that produces the error.

**Applies to:** every future scene cell and doctest; the M6 scene (o) pins and M7's acceptance are
written to it. Existing pins that already use ulp bounds are unaffected.
