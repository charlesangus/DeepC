# Evidence lives inside the project directory

Decided 2026-09-26 (PM, after the second loss). Only `/home/bosley/git/DeepC` survives between sessions on this host;
`~/deepc-validation/`, `~/deepc-baselines/`, `~/DeepC-T0` and `~/deepc-scratch/` were wiped twice. All evidence,
baselines and auxiliary worktrees now live under `.evidence/` in the project root (listed in `.git/info/exclude`, so never
committed). Every existing `~/deepc-validation/…`, `~/deepc-baselines/…`, `~/deepc-scratch/…` path in the plan stays valid
because each session first recreates the symlinks
`for d in validation baselines scratch; do ln -sfn /home/bosley/git/DeepC/.evidence/$d ~/deepc-$d; done`.
The M8-T0 worktree moves from `~/DeepC-T0` to `.evidence/DeepC-T0`. Evidence lost before this date (P1.T2 REPORT, P2.T7
DESIGN.md, M8-T0 a–o logs) is not recoverable; the figures recorded in the milestone files are the surviving record.
