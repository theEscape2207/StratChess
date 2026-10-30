# Issue tracker: GitHub

Issues and specs for this repo live as GitHub issues. Use the `gh` CLI for all operations.

## Conventions

**Every body or comment goes through `--body-file`, never inline `--body "..."`.** An inline body
run from Bash executes backtick spans as shell commands and posts the mangled result publicly. Write
the text to a file first: your harness's scratchpad directory if it gives you one, otherwise
`build/` in your own worktree, which is gitignored and writable in every sandbox (create it if
missing). `.codex/` can be read-only.

- **Create an issue**: `gh issue create --title "..." --body-file <path>`
- **Read an issue**: `gh issue view <number> --json number,title,body,labels,comments`.
- **List issues**: `gh issue list --state open --json number,title,body,labels,comments --jq '[.[] | {number, title, body, labels: [.labels[].name], comments: [.comments[].body]}]'` with appropriate `--label` and `--state` filters.
- **Make an issue a sub-issue of a parent**: `gh issue create --parent <parent> ...`, or `gh issue edit <parent> --add-sub-issue <child>` afterwards (`gh` 2.94+). Older `gh`: `gh api --method POST repos/<owner>/<repo>/issues/<parent>/sub_issues -F sub_issue_id=<child-db-id>` (database id, as in **Blocking** below). Without sub-issues, put `Part of #<parent>` at the top of the child body.
- **Blocking**: use native issue dependencies: `gh issue create --blocked-by <n>,<n> ...` or `gh issue edit <child> --add-blocked-by <blocker>` (`gh` 2.94+). Older `gh`: `gh api --method POST repos/<owner>/<repo>/issues/<child>/dependencies/blocked_by -F issue_id=<blocker-db-id>`, where the database id comes from `gh api repos/<owner>/<repo>/issues/<n> --jq .id`, not the issue number or node id. Where unavailable, use `Blocked by: #<n>, #<n>` at the top of the child body. A ticket is unblocked when every blocker is closed.
- **Count open blockers**: `gh api repos/<owner>/<repo>/issues/<n> --jq .issue_dependencies_summary.blocked_by` (open blockers only; 0 means startable).
- **Comment on an issue**: `gh issue comment <number> --body-file <path>`
- **Apply / remove labels**: `gh issue edit <number> --add-label "..."` / `--remove-label "..."`
- **Close**: post the explanation with `gh issue comment <number> --body-file <path>`, then `gh issue close <number>`. On an already-closed issue, `gh issue close --comment` drops the comment and still exits 0.

Infer the repo from `git remote -v`; `gh` does this automatically when run inside a clone.

Before the first `gh issue create` in a session, check `gh repo view --json nameWithOwner`:
the target must be `theEscape2207/StratChess`. Specify `--repo theEscape2207/StratChess` if the
default differs. These skills' upstream repository, `mattpocock/skills`, is not this project's tracker.

## Pull requests are script-mediated

**Never open or update a PR with `gh pr create`, `gh pr edit --body`, or a bare `git push`.** PRs go
through `Scripts\New-PullRequest.ps1 -Title "..." -BodyFile <path>`, which syncs with `origin/main`,
runs `Validate-PrePR.ps1` at the right change tier and then pushes. A bare push succeeds while
silently skipping that gate (this happened on PR #148). The script also checks the body against the
headings of `.github/pull_request_template.md`. `gh pr create` does neither.

Two consequences worth stating for any skill that finishes a piece of work:

- A pushed PR is **awaiting merge**, not done: the user merges it.
- Auto-closing an issue needs GitHub's exact keywords in the PR body — `Closes #N` / `Fixes #N` /
  `Resolves #N`. Prose like "closing #N" does not close anything.

Read a PR with `gh pr view <number>`, then `gh pr view <number> --comments`, and
`gh pr diff <number>`. Off a TTY, `--comments` prints only comments, so fetch the body separately.

**For CI status, use `Scripts\Get-PrChecks.ps1 [-Pr n] [-Wait]`, not `gh pr checks` in a poll loop.**
It gives one line when the run is green and, when it is not, the failing step and its errors without
a second call; `-Wait` polls to completion. Exit 0 green / 1 failed / 2 still running.

## Pull requests as a triage surface

**PRs as a request surface: no.** _(Set to `yes` if this repo treats external PRs as feature requests; `/triage` reads this flag.)_

When set to `yes`, PRs run through the same labels and states as issues, using the `gh pr` equivalents:

- **Read a PR**: fetch its body, comments and diff as above.
- **List external PRs for triage**: `gh api --paginate 'repos/theEscape2207/StratChess/pulls?state=open' --jq '.[] | select(.author_association | IN("OWNER","MEMBER","COLLABORATOR") | not) | {number, title, author: .user.login, author_association, labels: [.labels[].name]}'`.
- **Comment / label / close**: `gh pr comment`, `gh pr edit --add-label`/`--remove-label`, `gh pr close`.

GitHub shares one number space across issues and PRs, so a bare `#42` may be either: resolve with `gh pr view 42` and fall back to `gh issue view 42`.

## When a skill says "publish to the issue tracker"

Create a GitHub issue.

## When a skill says "fetch the relevant ticket"

Read it as in **Read an issue** above.

## Wayfinding operations

Used by the vendored `/wayfinder`. Maps and tickets carry only `wayfinder:` labels, separate
from implementation triage labels.

- **Map**: an issue labelled `wayfinder:map`, holding Notes / Decisions-so-far / Fog.
- **Child ticket**: link it to the map as a sub-issue (see Conventions), and label it
  `wayfinder:<type>` (`research`/`prototype`/`grilling`/`task`). Without sub-issues, add it to a task
  list in the map body and put `Part of #<map>` at the top of the child body.
- **Frontier query**: list the map's open children, discard any with an open blocker (see
  **Count open blockers**) or assignee; first in map order wins.
- **Claim**: `gh issue edit <n> --add-assignee @me`, the session's first write.
- **Resolve**: comment via `--body-file`, close the ticket, then append a context pointer
  (gist + link) to the map's Decisions-so-far.
