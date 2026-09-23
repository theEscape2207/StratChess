# File-type skill gate — Design

**Issue:** none. This is weakness 7 of the skill audit: skills tied to a file type don't trigger
themselves.

## Goal

Two skills cover a file type, and both are skipped most of the time:
- `write-powershell` loaded in 6 of 34 sessions that edited a `.ps1` (18%).
- `writing-for-agents` loaded in 3 of 9 sessions that edited a skill or `CLAUDE.md` (33%), and only
  when the user asked for it.

Their traps fail silently, so a skipped load shows up late or never. A skill description is matched
against the task, not the file being edited, so better wording cannot fix this. A hook that fires on
the edit itself can.

## Scope

**This change will:**

- add one PowerShell hook script. It runs before a file edit in both Claude Code and Codex, and names
  the skill the edit needs;
- register it in `.claude/settings.json` and in Codex's project hook config;
- add a self-test that feeds the script recorded hook inputs.

**This change will not:**

- cover edits made through a shell (`sed`, a heredoc, `Set-Content`) or through MCP tools such as
  lean-ctx's `ctx_patch`. The routing lines in `CLAUDE.md` and `AGENTS.md` still cover those. D4 has
  the opt-in for `ctx_patch`;
- check whether the skill was followed, only whether it was loaded;
- add gates for other skills (`measure-strength`, `analyze-games`). They key on commands, not file
  types.

## Decisions

### D1: deny the first matching edit per session, then allow

The first edit to a matching file in a session is denied with the reason "load skill
`write-powershell`, then retry". The script writes a marker keyed by session and skill, so the retry
and every later edit pass.

- **Rejected: a non-blocking reminder (`additionalContext`).** It is advice the agent can pass over,
  which is the same failure as a skill description, one step later.
- **Rejected: scanning the transcript for an earlier skill load.** That would spare an agent that had
  already loaded the skill one retry. But it couples the hook to transcript formats that are not
  documented and that differ between Claude and Codex.

The cost is one retried edit per skill per session, which an agent that loads the skill anyway also
pays.

### D2: file sets

| Skill | Paths (repo-relative) |
|---|---|
| `write-powershell` | `*.ps1` |
| `writing-for-agents` | `CLAUDE.md`, `AGENTS.md`, `.claude/skills/**`, `.agents/skills/**`, `.claude/agents/**`, `.codex/agents/**` |

The repo tracks no `.psm1` files. `.githooks/pre-commit` is `sh`, so it is not included.

### D3: filter in the config where the host can, parse in the script everywhere

A `pwsh -NoProfile` start costs about 530 ms on this machine (median of 5 runs; `python` 133 ms,
`node` 44 ms).
- **Claude:** each handler gets an `if` rule (`Edit(*.ps1)`, `Edit(**/SKILL.md)`, …), so only
  matching edits start `pwsh`.
- **Codex:** a matcher cannot filter by path, so the hook runs on every `apply_patch`. The script
  reads every `*** Update File:`, `*** Add File:` and `*** Delete File:` line in `tool_input.command`,
  since one patch can name several files.

The script matches paths itself either way, so a wrong `if` rule only costs time.

- **Rejected: Python or Node for speed.** Repo tooling is PowerShell (`write-powershell`); Node is not
  a declared dependency. About 0.5 s per Codex edit is the price, and it is measured in Validation.

### D4: path fields

The script reads `tool_input.file_path` (Claude `Edit`/`Write`), `tool_input.command` (Codex
`apply_patch`) and `tool_input.path` (lean-ctx `ctx_patch`).
- `ctx_patch` is a per-user MCP server, so the tracked config does not match it. A user who routes
  edits through it can add the matcher to their own `~/.claude/settings.json`, pointing at the same
  script.
- MCP tools cannot take an `if` path rule, so that matcher costs the `pwsh` start on every patch.

### D5: fail open

The script allows the edit in these cases:
- the input has no session id;
- the JSON does not parse;
- the marker cannot be written;
- the script throws.

A broken gate must never stop edits. The self-test covers each of these paths.

## Assumptions I cannot verify from the code

1. **Codex honours a deny.** It is not known whether a Codex `PreToolUse` hook denies through the same
   JSON (`hookSpecificOutput.permissionDecision: "deny"`) or through exit code 2 with stderr.
   Codex's answer so far covers firing, the matcher and `tool_input.command`, not the output. To be
   settled by Codex in review. Not yet verified.
2. **Codex's project hook config.** Its location and schema, and whether Codex needs the project
   marked trusted before the hooks run. To be settled by Codex in review. Not yet verified.
3. **Codex hook input carries a session id.** If it doesn't, D5 turns the Codex gate into a no-op. To
   be settled by Codex in review, then by a live Codex edit. Not yet verified.
4. **Claude's `Edit(<pattern>)` rule matches the `Write` tool too, and `*.ps1` matches at any depth.**
   The hooks docs say `if` uses permission-rule syntax; the depth semantics are taken on trust. To be
   verified by a live session: create and edit a `.ps1` under `Scripts/` and check that each is
   denied once.

Verified: Codex's hook fires on `apply_patch`, and the file names are only in the patch text (Codex,
2026-09-23). Claude's `PreToolUse` input carries `session_id` and `tool_input.file_path`, and its
`permissionDecision: "deny"` shows `permissionDecisionReason` to the model (Claude Code hooks docs).

## Invariants

- A non-matching edit is never denied.
- A matching edit is denied at most once per skill per session.
- Any error allows the edit (D5).
- Only edits are gated; reads, shell commands and other tools are untouched.

## Validation

**Tier:** Tooling for the script. `.claude/settings.json` and the Codex config fall to the fail-closed
default, so the PR runs the full gate. No Elo or nps measurement: nothing reaches the engine binary.

- **Self-test:** `Scripts/Test-SkillGate.ps1` feeds recorded inputs and checks the output of each:
  - Claude `Edit` and `Write` on a `.ps1` file;
  - a Codex `apply_patch` naming two files, one of which matches;
  - a non-matching file;
  - the second matching edit in the same session;
  - a missing session id;
  - malformed JSON.

  Checks: the decision is correct, the reason names the right skill, and the marker is written once.
- **Live, Claude:** one session that edits a `.ps1` and a `SKILL.md`. Each edit is denied once, then
  allowed. An edit to `Docs/Workflow.md` passes untouched. This closes assumption 4.
- **Live, Codex:** the same sequence through `apply_patch`, run by Codex. This closes assumptions 1–3.
- **Cost:** the time the hook adds to a non-matching Codex `apply_patch`, median of 5 runs.
- **Outcome:** consistency for both skills at the next re-audit, over sessions from the merge onward.
  Today's figures are 18% and 33%.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| deny-once over remind (D1), fail open (D5), the gaps (shell edits, `ctx_patch`) | the hook script's comment-based help |
| file sets (D2) | the script, as data |
| the `ctx_patch` opt-in for users (D4) | the script's help |
| the hook exists and why | `Docs/Changelog.md`, and one line in `Docs/Workflow.md` |
