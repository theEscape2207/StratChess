# Codex Project Bootstrap

Before doing repository work, read `CLAUDE.md` completely. Its project rules are mandatory.

## Required workflows

- Finishing or pushing a branch, opening or updating a pull request, handling review follow-ups, or
  cleaning up after a merge: use skill `open-pull-request` before the first related action.
- Measuring Elo, nps, search equivalence, or a performance regression: use skill
  `measure-strength`.
- Creating or changing PowerShell under `Scripts/`, `build.ps1`, or `.githooks/`: use skill
  `write-powershell`.
- Triaging, refining, scoping, labelling, or recommending closure of a GitHub issue: use skill
  `triage-issue` before the first related action.
- Reading or editing GitHub issues outside triage: read the applicable files under `Docs/agents/`
  first.

Repository skills are exposed to Codex under `.agents/skills/`; their canonical instructions remain
under `.claude/skills/`. If a required skill cannot be loaded, report the discovery failure instead
of substituting an ad-hoc workflow.

Every GitHub issue or pull-request body and every non-trivial comment must be supplied through a
body file, never an inline body argument.
