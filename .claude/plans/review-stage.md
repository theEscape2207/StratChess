# A default code review for every PR that changes code — Design

**Issue:** none. This is weakness 4 of the 2026-09-23 skill audit.

## Goal

No review runs by default. Four reviewers exist, and each runs only when someone asks for it:
- Claude's `/code-review`;
- Codex's `/review`;
- mattpocock's `code-review`;
- `search-reviewer`/`eval-reviewer`, dispatched for search/eval paths only.

`cross-agent-review` checks the design, not the diff. So whether a diff gets reviewed depends on which
agent wrote it and whether anyone remembered. None of the four looks for the failures seen most often
in agent-written code:
- detail beyond what the change needs;
- comments that break the CLAUDE.md comment rule;
- nearby debt that goes unmentioned: code the change touches that is already duplicated, dead or
  stale.

Separately, design documents never ask whether the plan adds to that debt.

## Scope

**This change will:**

- add a default review to `open-pull-request` step 1 for every PR outside the Docs tier;
- write the project's **simplify** lens as a standards file that the review reads;
- set a rule for nearby debt: file it as a new issue instead of fixing it in the PR;
- have the PR's Test plan record how every finding was handled;
- add a simplify check to `write-design-doc`'s self-review.

**This change will not:**

- change `search-reviewer`/`eval-reviewer`, when they are dispatched, or their skip criteria;
- make `New-PullRequest.ps1` enforce the review (see D5);
- review Docs-tier PRs, which already have `writing-for-agents` and `cross-agent-review`;
- add Codex parity checks (#428) or the file-type PreToolUse hooks (audit weakness 7). They touch
  other files and have their own open questions, so each gets its own document.
- run a review in CI.

## Decisions

### D1: The review engine is mattpocock's `code-review`

The review uses mattpocock's `code-review` against `origin/main`. It runs two parallel subagents: one
checks the diff against the repo's standards, the other against the issue or design doc it came from.
- **One copy for both agents:** Claude loads it from the plugin (1.2.3), Codex from
  `.agents/skills/code-review` (#616). The two copies differ only in line endings.
- **Hands-off:** the controller supplies the fixed point and the spec, so the skill never has to ask
  the user. The spec is the issue the PR body or commits cite (`Closes`/`Refs #N`) plus any
  `.claude/plans/` document on the branch. When there is neither, the controller tells the skill
  "no spec available". The Spec agent is then skipped, and the Review line says `Spec: skipped (no
  spec)`. The skill's step 2 would otherwise ask the user where the spec is.
- **Execution:** each axis runs in its own subagent, in parallel or one after the other. If an agent
  cannot spawn subagents, it runs both axes in its own context. That loses the isolation between the
  axes, so the Review line records it as `inline`.

Rejected:
- **Claude `/code-review` and Codex `/review`:** each runs in one agent only, which is the
  inconsistency this change removes.
- **A project reviewer agent:** it would repeat the skill's smell baseline and need a Codex copy, both
  kept up to date by hand.
- **A CI review action:** it needs an API secret and costs money on every push, and the user already
  routes the cross-agent review.

### D2: simplify is a standards source, not a third axis

`code-review` takes the repo's standards as input, and the repo's rule wins over its baseline. The
lens goes in `Docs/agents/simplify.md`, beside the files mattpocock's skills already read. The
controller passes that file and CLAUDE.md → Development Guidelines as the standards sources. The
lens asks four questions; questions 1–3 are asked of each hunk:

1. **Comments:** does a comment restate the code, refer to a task or to history, or run past two
   lines without recording a tripwire? (The rule is CLAUDE.md's; this checks against it.)
2. **Over-detail:** does the diff contain anything the change does not need? Examples:
   - an option or parameter only ever given one value;
   - an abstraction with a single caller;
   - validation against a caller that cannot exist (see the threat model);
   - diagnostics left over from debugging.
3. **Reuse:** does new code repeat a helper that already exists in the codebase or the standard
   library?
4. **Nearby debt:** in the functions and files the diff touches, what is already duplicated, dead or
   stale, or kept alive only by a workaround? This is reported separately from findings (D3).

The skill's Standards brief matches against the diff only. To cover question 4, the controller adds
one instruction to that brief: scan the whole of every function the diff changes, and the rest of
each touched file, but no other files. Report what that scan finds under a separate `Nearby debt`
heading, with `file:line` evidence. These items are not findings. The rest of the two-axis review is
unchanged.

Rejected: a third parallel axis. It would need our own copy of `code-review` kept in step with
upstream, and a standards file gets the same result without one.

### D3: Nearby debt becomes a new issue unless the change makes it worse

- **The change makes the debt worse**, for example by copying the duplication or extending the
  workaround: this is a finding against this PR.
- **Otherwise:** the author files it as a new issue.
  - **Duplicates:** search open issues first (`gh issue list --search`).
  - **Labels:** `needs-triage`.
  - **Body:** gives `file:line` and the PR where the debt was found.
- **Worth filing:** only debt the author would accept a PR to fix. Anything smaller is dropped.

This keeps each PR to its stated scope and keeps the debt from being lost.

### D4: Which PRs get reviewed: every tier except Docs

The Engine, Build and Tooling tiers get the review, per `Get-ChangeTier.ps1`, whichever agent wrote
the change. It runs alongside any `search-reviewer`/`eval-reviewer` dispatch, before step 2. The
reviewer's instructions carry no size threshold. A one-line engine change can carry a large nearby
debt, and deciding by file count is exactly what CLAUDE.md warns against.

### D5: A Review line records the dispositions; nothing enforces it

The author decides what to do with every finding: fix it, reject it with a reason, or file it as an
issue (#n). The Test plan then gets one line: `Review: code-review, Standards n / Spec m: x fixed,
y rejected (reasons), filed #a #b`. This makes the review auditable, as the `search-reviewer` skip
statement does. `New-PullRequest.ps1` does not check for the line: a check would be a Build-tier
change needing a self-test, for a skill that already loads 84% of the time. A reminder can be added
if PRs outside the Docs tier arrive without the line.

### D6: The design phase gets the same question

`write-design-doc`'s self-review gains a **Simplify** bullet. It asks what nearby debt the change
touches, whether the plan makes that debt worse, and whether every Decision is needed now. It points
to the same lens file.

## Assumptions I cannot verify from the code

- **Codex runs each axis in its own subagent.** Codex supports subagents (#612), but whether it
  spawns them from this skill has not been verified. It will be settled by the first Codex PR under
  the new step, whose Review line records the execution mode (D1). Parallel or one after the other
  is fine. An `inline` run is a degraded review and gets a follow-up issue.
- **The Standards agent follows the nearby-debt instruction (D2).** Not verified. The planted
  fixture below checks it.
- **The findings are worth what they cost.** #387 found lint to be the lowest-value gate here, and a
  noisy review would add that cost to every PR. Not verified. The retrospective runs and the pilot
  settle it; the Validation table below gives the stop rule.

## Invariants

- Specialised reviewer dispatch is unchanged, including its skip criteria.
- Every finding gets a disposition, and every PR outside the Docs tier has a Review line.
- Nearby debt is either a finding against the PR (D3) or a new issue. It is never an unrecorded fix
  in the same PR.
- The skill runs without asking the user anything. The fixed point is `origin/main`, and the spec
  is either supplied or declared absent.

## Validation

Docs tier: the change touches only skill and Markdown files, so no build, Elo or nps check applies.

| Risk | Evidence that closes it |
|---|---|
| The lens misses what it should find | **Planted fixture:** a throwaway branch off `origin/main` with two commits. Commit A plants pre-existing debt: a dead helper and a stale comment in a touched file. Commit B makes a small change that plants one example for each of lens questions 1–3: a restating comment, a parameter only ever given one value, and a copy of an existing helper. The review runs from A, so only B is the diff. Expected: B's three plants come back as findings, and A's two come back under Nearby debt with `file:line`. Any other finding is judged on merit. |
| The lens never looks past the hunk | The same fixture: A's plants lie outside the diff, so recovering them is the evidence. |
| The lens is noisy on real code | **Retrospective runs** on merged #607 (Engine) and #581 (Tooling). Each finding is judged on merit, with no minimum count. The judgements and token cost go in the PR body. |
| The no-spec path stops to ask | The fixture run gets no spec: it completes without a prompt and records `Spec: skipped (no spec)`. |
| Noise over time | Pilot on the next five PRs outside the Docs tier: 50% or more of the findings must be fixed or filed, otherwise trim the lens or drop the step. |
| Codex cannot run it | First Codex PR: the Review line is present and records the execution mode (subagents, or `inline`). |

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1–D5: the review step, its tier rule, debt rule and Review line | `open-pull-request` step 1 and step 3 |
| D2: the four lens questions | `Docs/agents/simplify.md` |
| D6 | `write-design-doc` self-review |
| Rejected engines (D1) and the retrospective results | PR body |
| Pilot outcome (keep, trim, drop) | `Docs/Changelog.md` once decided |
