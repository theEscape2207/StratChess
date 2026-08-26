# Issue tracker: GitHub

Issues and specs for this repo live as GitHub issues. Use the `gh` CLI for all operations.

## Conventions

**Every body or comment goes through `--body-file`, never inline `--body "..."`.** An inline body
run from Bash executes backtick spans as shell commands and posts the mangled result publicly. Write
the text to a file first — the scratchpad directory is the right place.

- **Create an issue**: `gh issue create --title "..." --body-file <path>`
- **Read an issue**: `gh issue view <number> --comments`, filtering comments by `jq` and also fetching labels.
- **List issues**: `gh issue list --state open --json number,title,body,labels,comments --jq '[.[] | {number, title, body, labels: [.labels[].name], comments: [.comments[].body]}]'` with appropriate `--label` and `--state` filters.
- **Comment on an issue**: `gh issue comment <number> --body-file <path>`
- **Apply / remove labels**: `gh issue edit <number> --add-label "..."` / `--remove-label "..."`
- **Close**: `gh issue close <number> --comment "..."` — short, backtick-free comments only; anything longer is a `gh issue comment --body-file` followed by a bare `gh issue close <number>`.

Infer the repo from `git remote -v`; `gh` does this automatically when run inside a clone.

## Pull requests are script-mediated

**Never open or update a PR with `gh pr create`, `gh pr edit --body`, or a bare `git push`.** PRs go
through `Scripts\New-PullRequest.ps1 -Title "..." -BodyFile <path>`, which syncs with `origin/main`,
runs `Validate-PrePR.ps1` at the right change tier and then pushes. A bare push succeeds while
silently skipping that gate (this happened on PR #148), and `gh pr create` also bypasses
`.github/pull_request_template.md`, so format the body as Summary / Test plan / Notes yourself.

Two consequences worth stating for any skill that finishes a piece of work:

- A pushed PR is **awaiting cross-agent review**, not done. See `Docs/Workflow.md` → Cross-agent review.
- Auto-closing an issue needs GitHub's exact keywords in the PR body — `Closes #N` / `Fixes #N` /
  `Resolves #N`. Prose like "closing #N" does not close anything.

Reading PRs is unrestricted: `gh pr view <number> --comments`, `gh pr diff <number>`.

**For CI status, use `Scripts\Get-PrChecks.ps1 [-Pr n] [-Wait]`, not `gh pr checks` in a poll loop.**
It gives one line when the run is green and, when it is not, the failing step and its errors without
a second call; `-Wait` polls to completion. Exit 0 green / 1 failed / 2 still running.

## Pull requests as a triage surface

**PRs as a request surface: no.** _(Set to `yes` if this repo treats external PRs as feature requests; `/triage` reads this flag.)_

When set to `yes`, PRs run through the same labels and states as issues, using the `gh pr` equivalents:

- **Read a PR**: `gh pr view <number> --comments` and `gh pr diff <number>` for the diff.
- **List external PRs for triage**: `gh pr list --state open --json number,title,body,labels,author,authorAssociation,comments` then keep only `authorAssociation` of `CONTRIBUTOR`, `FIRST_TIME_CONTRIBUTOR`, or `NONE` (drop `OWNER`/`MEMBER`/`COLLABORATOR`).
- **Comment / label / close**: `gh pr comment`, `gh pr edit --add-label`/`--remove-label`, `gh pr close`.

GitHub shares one number space across issues and PRs, so a bare `#42` may be either: resolve with `gh pr view 42` and fall back to `gh issue view 42`.

## When a skill says "publish to the issue tracker"

Create a GitHub issue.

## When a skill says "fetch the relevant ticket"

Run `gh issue view <number> --comments`.

## Wayfinding operations

`/wayfinder` is not installed here, so its map/child-ticket conventions are omitted. If it is ever
installed, re-run `/setup-matt-pocock-skills` to restore this section and namespace its
`wayfinder:*` labels alongside the repo's existing `category:` / `priority:` / `type:` scheme.
