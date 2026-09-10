# The plan branch stays local and is never pushed

**2026-09-10.** The `plan` orphan branch checked out at `.plan/` is not pushed to `origin`. This
overrides PLAN-FORMAT.md §9, which has the PM push the plan branch at each milestone gate
alongside the code PR.

The user's call: planning artifacts for this project stay on the machine. Code branches and PRs are
unaffected — the milestone branch is still pushed and the PR still opened and merged normally.

State when the rule was made: `origin/plan` already existed, holding plan history through
`6245660`, pushed by an earlier session before this rule. It was left in place rather than deleted;
removing it is a separate call the user has not made. The local branch still tracks `origin/plan`,
so `git status` in the worktree will report it as "ahead" indefinitely — that is expected, not
drift to be resolved by pushing.

Consequence worth knowing: the plan then exists in exactly one place. `git clean -xdf` at the
project root deletes the `.plan/` worktree but not the branch (re-attach per PLAN-FORMAT.md §1a);
losing the repo itself loses the board and every milestone file.
