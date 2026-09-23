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

**Provenance.** The rates come from the skill audit (2026-09-23): 157 Claude Code transcripts from
2026-06-26 to 2026-09-23, each skill counted only in sessions after it existed.
- **"Edited" means:** an `Edit` or `Write` tool call on a matching path.
- **"Loaded" means:** a `Skill` tool call naming the skill.

These are rough tool-call matches, and the audit's method notes carry the caveats. The re-audit in
Validation uses the same definitions.

## Scope

**This change will:**

- add one PowerShell hook script. It runs before a file edit in both Claude Code and Codex, and
  names the skill the edit needs;
- register it in `.claude/settings.json` and `.codex/hooks.json`;
- add a self-test that feeds the script recorded hook inputs, and checks that the Claude config
  covers the script's file sets;
- classify both new scripts as Tooling in `Scripts/Get-ChangeTier.ps1`.

**This change will not:**

- prove that the skill was loaded, or followed. The gate interrupts the first matching edit and names
  the skill (D1); the re-audit measures whether loads follow;
- cover edits made through a shell (`sed`, a heredoc, `Set-Content`) or through MCP tools such as
  lean-ctx's `ctx_patch`. The routing lines in `CLAUDE.md` and `AGENTS.md` still cover those. D4 has
  the opt-in for `ctx_patch`;
- add gates for other skills (`measure-strength`, `analyze-games`). They key on commands, not file
  types.

## Decisions

### D1: interrupt the first matching edit per session, then allow

The first edit to a matching file in a session is denied with "load skill `write-powershell`, then
retry". The script records a marker keyed by session and skill, so the retry and every later edit
pass. The marker records the interruption, not a skill load: a retry without loading passes too.

- **Rejected: a non-blocking reminder (`additionalContext`).** It is advice the agent can pass over,
  which is the same failure as a skill description, one step later.
- **Rejected for now: a load gate** that allows only after a positive load signal, such as a Claude
  `PostToolUse` hook on the `Skill` tool writing the marker.
  - Codex loads a skill by reading its file, so it has no equivalent event.
  - A missed signal would block every edit. That conflicts with D5.
  - Worth revisiting if the re-audit shows retries that skip the load.
- **Rejected: scanning the transcript for an earlier skill load.** It couples the hook to transcript
  formats that are not documented and that differ between Claude and Codex.

The cost is one retried edit per skill per session, which an agent that loads the skill anyway also
pays.

### D2: file sets

| Skill | Paths (repo-relative) |
|---|---|
| `write-powershell` | `*.ps1` |
| `writing-for-agents` | `CLAUDE.md`, `AGENTS.md`, `.claude/skills/**`, `.agents/skills/**`, |
| | `.claude/agents/**`, `.codex/agents/**` |

The repo tracks no `.psm1` files. `.githooks/pre-commit` is `sh`, so it is not included.

### D3: filter in the Claude config, parse in the script everywhere

A `pwsh -NoProfile -NonInteractive -Command exit` start costs 534 ms on this machine. That is the
median of 5 `Measure-Command` runs through `cmd /c`, which adds a little; `python -c pass` took
133 ms and `node -e 0` 44 ms, measured the same way.

- **Claude:** an `if` rule matches one tool, so each file-set pattern gets two handlers,
  `Edit(<pattern>)` and `Write(<pattern>)`. Only matching edits start `pwsh`.
  - A wrong or missing rule skips the hook silently. So the self-test reads `.claude/settings.json`
    and fails unless every pattern in the script's file sets has both handlers.
- **Codex:** a matcher cannot filter by path, so the hook runs on every `apply_patch`. The script
  reads every `*** Update File:`, `*** Add File:` and `*** Delete File:` line in
  `tool_input.command`, since one patch can name several files.

- **Rejected: one unfiltered `Edit|Write` matcher in Claude.** It is simpler, but it pays about
  0.5 s on every edit in every session.
- **Rejected: Python or Node for speed.** Repo tooling is PowerShell (`write-powershell`), and Node
  is not a declared dependency. About 0.5 s per Codex edit is the price, and it is measured in
  Validation.

### D4: path fields

The script reads `tool_input.file_path` (Claude `Edit`/`Write`, absolute), `tool_input.command`
(Codex `apply_patch`) and `tool_input.path` (lean-ctx `ctx_patch`). It then makes each path relative
to the repo root before matching.
- `ctx_patch` is a per-user MCP server, so the tracked config does not match it. A user who routes
  edits through it can add the matcher to their own `~/.claude/settings.json`, pointing at the same
  script.
- MCP tools cannot take an `if` path rule, so that matcher costs the `pwsh` start on every patch.

### D5: fail open, and mark atomically

The script allows the edit in these cases:
- the input has no session id;
- the JSON does not parse;
- the marker cannot be written;
- the script throws.

A broken gate must never stop edits.

The marker is created with `[IO.File]::Open(<path>, 'CreateNew')`, which is atomic create-if-absent.
Only the call that creates it denies. A call that finds the marker already there allows. So two
concurrent matching edits produce one denial.

## Codex hook contract

From the Codex hooks documentation, confirmed by Codex in review:
- `PreToolUse` fires on `apply_patch`, and the patch text is in `tool_input.command`.
- The input carries `session_id`.
- It accepts the same deny JSON as Claude (`hookSpecificOutput.permissionDecision: "deny"`), or
  exit code 2 with stderr.
- Project hooks load from `.codex/hooks.json`, or inline in `.codex/config.toml`, only when the
  project is trusted.

## Assumptions I cannot verify from the code

1. **Claude's `*.ps1` path rule matches at any depth.** The docs say `if` uses permission-rule syntax;
   the depth semantics are taken on trust. To be verified by the live Claude run: create and edit a
   `.ps1` under `Scripts/`. Not yet verified.
2. **This repo's Codex config actually loads.** The contract above is documented. Whether this repo is
   trusted and the config is read is settled only by the live Codex run. Not yet verified.

## Invariants

- A non-matching edit is never denied.
- A matching edit is denied at most once per skill per session, concurrent edits included.
- Any error allows the edit (D5).
- Only edits are gated; reads, shell commands and other tools are untouched.
- Every file-set pattern has both an `Edit` and a `Write` handler in `.claude/settings.json`.

## Validation

**Tier:** Build. The PR edits `Scripts/Get-ChangeTier.ps1`, which is Build tier itself.
- After it merges, edits to the two scripts are Tooling.
- `.claude/settings.json` and `.codex/hooks.json` stay on the fail-closed default. They change
  rarely, and a hook config is not engine-inert by inspection.

No Elo or nps measurement: nothing reaches the engine binary.

- **Self-test:** `Scripts/Test-SkillGate.ps1` feeds recorded inputs and checks the output of each:
  - Claude `Edit` and `Write` on a `.ps1` file;
  - a Codex `apply_patch` naming two files, one of which matches;
  - a non-matching file;
  - the second matching edit in the same session;
  - two matching edits started in parallel with the same session;
  - a missing session id;
  - malformed JSON.

  Checks: the decision is correct, the reason names the right skill, and the parallel pair yields
  exactly one denial. It also checks that `.claude/settings.json` covers every file-set pattern for
  both tools.
- **Live, Claude:** one session that does the following:
  - `Write`s a new `.ps1` under `Scripts/`;
  - `Edit`s an existing `.ps1`;
  - `Edit`s a `SKILL.md`.

  Expected: the first `.ps1` edit and the `SKILL.md` edit are each denied once, and everything else
  is allowed. An edit to `Docs/Workflow.md` passes untouched. This closes assumption 1.
- **Live, Codex:** the same sequence through `apply_patch`, run by Codex. This closes assumption 2.
- **Cost:** the time the hook adds to a non-matching Codex `apply_patch`, median of 5 runs.
- **Outcome:** loads per session that edited a matching file, for both skills, at the next re-audit.
  It uses the definitions in Goal, over sessions from the merge onward. Today's figures are 18% and
  33%.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| interrupt, not load gate (D1); fail open, atomic marker (D5); the gaps | the hook script's help |
| file sets (D2) | the script, as data; the self-test ties them to the Claude config |
| the `ctx_patch` opt-in for users (D4) | the script's help |
| the hook exists and why | `Docs/Changelog.md`, and one line in `Docs/Workflow.md` |
