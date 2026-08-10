# CLAUDE.md – StratChessEvolved

A modern C++20 chess engine focused on improving playing strength (Elo) while maintaining clarity,
efficiency, and robustness.

Reference detail is pointed at, not duplicated: `Docs/Workflow.md` (standing decisions — what
validates what, speed/nps, threat model — plus validation tiers, review gate, runtime files,
worktree gotchas), `Docs/CI.md` (what each workflow runs), `Docs/TestDesign.md` (coverage map +
writing tests), `Docs/Changelog.md` (history). Live backlog is GitHub Issues.

## Repository Structure
- `StratChessEvolved/` – Application entry point, `game_settings.json`, `Scripts/`
- `StratEngine/` – Core engine (search, evaluation, move generation, AI agents)
- `StratEngine/Utils/` – `TimeManager` (soft/hard limits), `TimeUtils` (budget formula), `Logger`, `FENParser`
- `StratEngine/Archived/` – Legacy algorithms kept for reference, not built
- `StratChessTests/` – Catch2 v3 unit test project
- `Docs/` – Design documents and reference

## Build
- **Only `x64` builds work** — the x86/Win32 configuration is not maintained for C++20.
- **Warnings are errors everywhere** — `/W4 /WX` on MSVC and clang-cl, `-Wall -Wextra -Werror` on
  GCC, in both Debug and Release. Approved suppressions: `[[maybe_unused]]` for params used only in
  `assert()`; `static_cast<>` for intentional narrowing. Never `#pragma warning(disable)` in source.
- Dependencies (spdlog, nlohmann/json, Catch2) are fetched and pinned by `FetchContent` into
  `build/_deps`, shared by every preset. There is nothing to install and no sibling checkout to keep
  in step.
- `StratEngine/StdAfx.h` is the shared common-include header (no build precompiles it) — add
  frequently-used STL headers there, alphabetically inside the `#pragma warning push/pop` block, not
  in individual `.cpp` files.
- **Adding a `.cpp` needs no project edit.** `CMakeLists.txt` globs with `CONFIGURE_DEPENDS`; just
  create the file. `StratEngine/Archived/` is excluded and never built.

```powershell
.\build.ps1                          # engine + tests (Release, clang-cl)
.\build.ps1 main | tests             # one target
.\build.ps1 run-tests ["[tag]"]      # build tests, run fast tier (~[slow]), optional tag filter
.\build.ps1 extended-tests           # include [slow]
.\build.ps1 all -Config Debug        # debug build
.\build.ps1 main -Compiler msvc      # MSVC instead of clang-cl
```

`build.ps1` drives the presets in `CMakePresets.json` and imports the VS developer environment
itself via `vswhere`, so it works from a plain shell, a git hook or an agent session — never
hard-code a VS path. It also sets `core.hooksPath` to `.githooks` on first run, so any clone or
worktree gets the tracked pre-commit hook automatically.

**clang-cl is what ships**; MSVC is for development and debugging (it has Edit and Continue, which
clang-cl does not). **Never measure with an MSVC build** — `Run-Bench` and `Run-EloMatch` compare
only within one compiler, and mixing them shows the compiler gap as a phantom regression.
`Get-BuildArtifact.ps1` defaults to the shipping build for that reason.

**A fresh worktree's first build needs network**: it clones the pinned dependencies (~1 min). Visual
Studio setup, the `/clang:` flag traps, and the shared deps cache that avoids re-cloning per
worktree: `Docs/Workflow.md`.

## Scripts

In `StratChessEvolved/Scripts/`. They resolve working directory and build-output paths internally.
Invoke with `-File` (never dot-sourced — `$PSScriptRoot` is `$null` under dot-source):

```
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\<name>.ps1"
```

| Script | When |
|---|---|
| `Run-Tests.ps1 [tag]` | Any test verification |
| `Validate-PreCommit.ps1` | Before every commit — FEN check + fast tests (the pre-commit hook runs this) |
| `Validate-PrePR.ps1` | Before a PR — scopes itself to the change tier; `-Force` to run everything |
| `Run-EloMatch.ps1 [-Smoke]` | After search/eval/time changes — strength vs the pinned reference (method: `Docs/EloMeasurement.md`, results: `Docs/EloLog.md`) |
| `Run-Bench.ps1 -Exe <path>` | Search speed (nps) at fixed depth — before/after any optimisation, or comparing two builds |
| `Run-PerftCheck.ps1` | Move generation against a 142,953-position corpus (~25 min) — after `MoveGenerator`, make/unmake or FEN-parser work. A clean sweep is not zero failures; the script classifies them (`Docs/TestDesign.md`) |
| `New-Worktree.ps1 -Name <task>` | Starting a task that needs its own directory — forks fresh from `origin/main` at the correct path |
| `New-TaskBranch.ps1 -Name <task>` | Starting a task **in the current worktree** — forks a branch from `origin/main`, refusing on a dirty tree |
| `New-PullRequest.ps1 -Title "…"` | Sync → validate → push → create/update PR. `-Draft`, `-NoPr`, `-BodyFile` |
| `Remove-Worktree.ps1 -Name <task> -SyncMaster` | After a PR merges — worktree, local and remote branch, then syncs `master` |
| `Remove-MergedBranches.ps1 [-SyncMaster]` | After PRs merge, working in-place — deletes branches **verified** merged into `origin/main` |
| `Get-Worktrees.ps1` | Session start, or before resuming an idle worktree — drift, PR state, and unregistered directories |
| `Sync-Master.ps1` | Bring `master` up to `origin/main` — runs from anywhere; finds the worktree holding `master` |

**Measurement**: use `-Sprt NonRegression` / `-Sprt Gain` for anything expected to be worth less
than ~25 Elo — a fixed 500-game batch cannot resolve it, and recording the resulting "±26" row as a
measurement is how false confidence accumulates. An SPRT that hits the `-Games` cap without crossing
a bound is **inconclusive**, not a measured zero; record it as such. An SPRT needs a reference that
isolates the change, so `Run-EloMatch.ps1` refuses `-Sprt` against the fixed anchor (`-AnchorSprt`
overrides it for a deliberate cumulative reading). A full 500-game batch is ≈40 min
unattended at the default `-Concurrency 6`. Measurement budget is the user's call — report what
deciding would cost and let them choose.

**Opening book**: 250 openings = 500 distinct games, so a 500-game batch exhausts the committed book
exactly and anything larger replays openings. `Run-EloMatch.ps1` prints the count and warns. For
bigger batches drop a large book at `EngineTesting\openings-large.pgn|.epd` (not committed — public
repo, third-party data) or pass `-Book`.

**Speed vs strength**: `Run-Bench.ps1` measures nps, not Elo — use it when the change is meant to be
faster, and `Run-EloMatch.ps1` when it is meant to be stronger. Compare **nps**, never node counts at
fixed depth: the node count is a property of the search, not of the machine code. It is still the
right *equivalence* check, because two builds of identical source must visit identical nodes and
return identical best moves at `Threads=1` — if they do not, the builds are not searching the same
tree and any nps comparison is meaningless. Take repeat runs before quoting a delta; a single pass
has already produced a 12% outlier on this hardware.

**Never bypass `New-PullRequest.ps1` with a bare `git push`** to update an open PR: the push
succeeds but `Validate-PrePR.ps1` never runs, leaving the merged state covered only by the
pre-commit hook (this happened on PR #148).

CI is a **gate** — `build-and-test-result` is a required check on `main` and a red run blocks the
merge. A SKIPPED leg reports success on purpose, so Docs and Tooling PRs are not blocked by jobs that
correctly never ran. Build/Engine tiers only; Docs and Tooling skip the build jobs on PRs and merges
alike. Self-play stays local. What each workflow runs, and when: `Docs/CI.md`.

**Linux Debug + sanitizers is the primary correctness gate; Windows CI covers the shipping
toolchain.** They answer different questions and neither replaces the other — clang-cl silently drops
flags Linux can never observe. Do not add a Windows Debug configuration to a gate. The reasoning, and
what would change it: `Docs/Workflow.md` → Standing decisions.

**The goal is measured positive Elo, not nps.** Below ~5% use `Run-Bench.ps1`; an Elo match cannot
resolve an effect that small at any affordable game count (1% nps ≈ 1.7 Elo, against the lab's ±4).
Anything adding per-node work — evaluation terms as much as compiler flags — gets a bench pass, and a
measured slowdown needs a stated benefit that outweighs it. Sizing and instrument choice:
`Docs/Workflow.md` → Speed and nps.

**Threat model**: not network-facing, no privilege boundary, no attacker. External-input work aims at
robustness — a clear diagnostic and a clean exit — not security. Exploit mitigations need a reason
beyond sounding prudent; CFG was declined on exactly that basis (#218). Full statement:
`Docs/Workflow.md` → Threat model.

## Engine Summary
IDS + PVS + quiescence; Zobrist-hashed TT; bitboards with PEXT magic sliding attacks; killers
(2/ply) + history; null-move pruning; LMR; tapered evaluation; Lazy SMP.

## Key Source Facts

Non-obvious API contracts — the rest of the layout is discoverable.

- `Move` is a pure 2-byte value (from/to/flags). The moving and captured pieces are **not** stored:
  use `Board::GetEffectiveMovPiece(m)` (pre-move only) and `Board::GetCapturedPiece(m)`. After
  `DoMove`, identify the moved piece with `board.GetPiece(m.to())`.
- `Move` equality compares from/to only and **ignores flags** — two moves differing only in
  promotion piece compare equal.
- Move formatting lives entirely in `MoveFormatter`: `ToCoord` (coordinate-only, no board),
  `ToShort` (piece-prefixed; the `Board` overload appends `+` and reads the board, so never call it
  after a failed or unpaired `DoMove`), `ToUCI`, `ToVerbose`, `FromUCI`.
- `MoveHelper` predicates (`IsCapture`, `IsPawnMove`, `IsKingMove`, `Value`) take an `ePiece`, never
  a `Move&`.
- `ThreadData&` is the **first parameter of every search method**. The search runs on `td.board`,
  never the game board; root state is propagated back in `GetMove()`. The TT is a separate shared
  parameter — Lazy SMP helpers each get their own `ThreadData`.
- `SearchLimits` carries every per-call constraint (clock/movetime/depth/infinite, all optional);
  `Engine::resolve_limits()` resolves it and `PlayerAiBase::ApplyLimits()` arms the timer. Every
  `GetMove(info, limits)` call is self-contained — there is no pre-call ordering contract.
- `Engine::compute_budget(remaining, increment, moves_to_go)` → `TimeBudget{soft, hard}` is pure.
- Null-move pruning is gated by `tuning_.null_move_enabled` via `should_try_null_move()` (covers
  zugzwang, mate-score contamination, consecutive nulls, PV/in-check, min-depth).
- `game_settings.json` holds per-player `"search_limits"`; it accepts C-style `/* */` comments via
  nlohmann, but PowerShell's `ConvertFrom-Json` does not.

## Development Guidelines
- C++20; favour `constexpr`, RAII, move semantics, strong types.
- Approved external dependencies only: `spdlog`, `nlohmann/json`.
- All changes must be thread-safe, especially around the transposition table.
- No regressions in search accuracy or Elo without explicit justification; benchmark before and
  after any optimisation, and keep behaviour deterministic.
- English, unambiguous naming and comments. **Comments describe the code as it stands** — not what
  it replaced, not task/gate labels, not point-in-time measurements. Narrative belongs in the PR
  body or `Docs/Changelog.md`.
- Run the exe from `StratChessEvolved/` — both for `game_settings.json` and so logs land in
  `StratChessEvolved/logs/`.
- Before committing, check `game_settings.json` is back at the starting FEN; test sessions leave
  custom positions behind.

## Testing

`Docs/TestDesign.md` is the coverage map and the guide to writing tests — check it before adding
any. Two rules matter enough to repeat here, because both are silent failures:

- **Every tactical FEN needs its side-to-move field** (` w - - 0 1`). Omitting it drops below the
  parser's four-field floor, so the FEN is rejected and the position is never applied — the board
  keeps whatever it held and the engine answers for that instead.
- **Never measure an MSVC-built binary against a clang-built one.** Both run and both look healthy;
  the compiler gap alone is worth tens of Elo and gets credited to whatever change is under test.

Execute validation steps autonomously; flag any step needing user assistance (interactive GUI,
manual input) rather than skipping it silently.

## Commit & PR Conventions

- Every task forks fresh from `origin/main`; PRs target `main`. Two ways to run one, both enforcing
  that: a **per-task worktree** (`New-Worktree.ps1`) when work must be parked or run alongside
  another task, or a **task branch in the current worktree** (`New-TaskBranch.ps1`) for a run of
  small sequential PRs. In-place is sequential only — one worktree holds one branch — and the script
  refuses to start a task on a tree with uncommitted tracked changes, which is the guard separate
  directories give for free. Details: `Docs/Workflow.md`.
- Local `master` is a personal scratch branch — safe to commit to, safe to let drift. Never fork a
  worktree from it. `origin/master` is retired; nothing should reference it.
- Keep PRs small and logically scoped; include motivation, design reasoning and expected impact for
  anything non-trivial. Keep commit messages short — detail goes in the PR body or chat.
- If a worktree branch carries commits unrelated to the task, cherry-pick the relevant ones onto a
  fresh branch from `origin/main` rather than PRing the branch directly.
- Commit only what was explicitly asked for.

### Pre-PR checklist

`New-PullRequest.ps1` performs steps 1, 2 and 4 and stops at the first failure. Step 3 is manual by
design.

1. **Sync** — `git fetch origin main`, then `git merge origin/main`. When a conflict is just "two
   PRs added unrelated declarations at the same anchor", keep both sides. Applies to docs too.
2. **Validate** — run `Validate-PrePR.ps1`. It scopes itself to the change tier; there is no
   judgement call to make. Tier table and its fail-closed guarantees: `Docs/Workflow.md`.
3. **Dispatch a specialised reviewer** if the diff touches its domain
   (`git diff --name-only origin/main...HEAD`):
   - `Eval.cpp` → `eval-reviewer`
   - `AIPerplex.cpp/.h`, `ThreadData.h` (killers/history), `Sort.cpp/.h` (MVV-LVA) → `search-reviewer`

   **Default is to dispatch.** A narrow self-certification carve-out exists for logging-only diffs;
   its six conditions are in `Docs/Workflow.md` — read them before claiming a skip, and state the
   skip in the PR body so it is auditable. Address findings before opening the PR.
4. Only after 1-3 pass, create or update the PR.

### Cross-agent review

Separate from step 3: a second agent reviews selected artifacts and comments on the PR or issue
**before merge**. The user routes it — it is not dispatched from here — so a pushed PR is *awaiting
review*, not done. Say so when reporting one.

- **Review**: issues and specs before work starts, design docs, measurement and validation plans, and
  documents making provenance claims. These are where a bad premise is expensive and invisible to CI.
  Aim it hardest at the design doc's "assumptions I cannot verify from the code" section.
- **Skip**: mechanical changes where CI is the real gate, and artifacts that have already converged.
- **Division of labour**: `eval-reviewer` / `search-reviewer` review the **diff**; the cross-agent
  reviewer reviews the **design doc**. Putting both on one artifact is where cost blows up for little
  added signal. That split leaves a seam, so **the PR body must state which approved decisions
  changed during implementation, and why** — otherwise nobody checks the diff still matches the
  design. The Harvest table is the natural place to notice it.
- **One round per artifact** unless it finds something blocking. Signal density falls off sharply
  after the first pass.
- **Rank findings** when reviewing — unranked findings force the author to re-triage before acting.
  **Blocking**: merging without it risks a wrong or unverifiable result. **Add**: a real gap worth
  closing, but the change is sound without it. **Clarify**: wording or framing, no behaviour at stake.
- **A blocking finding is closed with evidence proportionate to the claim**, not with an assertion.
  Measurement when the claim is about runtime or external behaviour — as PR #263 did for fastchess's
  `ucinewgame` — but source inspection, an authoritative specification, a focused test or explicit
  reasoning all qualify where they actually settle the question.

Its strengths are provenance (who actually measured a number) and logical form (dichotomies that do
not hold); it is weak at judging what is worth changing versus leaving alone. **Adjudicate on the
merits** — push back with reasoning where a point does not hold rather than complying with all of
them, and record the disposition so the exchange stays auditable.

### After a PR merges

`Remove-Worktree.ps1 -Name <task> -SyncMaster`, or `Remove-MergedBranches.ps1 -SyncMaster` when
working in place. Treat cleanup as part of finishing the task, not an optional extra the user has to
ask for. Squash-merges and locked directories need care — `Docs/Workflow.md`.

## Design Documents

Write `.claude/plans/<kebab-name>.md` before implementing when **either** the change has a decision
that could reasonably go more than one way *and* materially affects a contract, the architecture,
correctness, strength, performance or maintenance cost, or it rests on an assumption you cannot
verify from the code in front of you. File count is not the trigger: a ten-file mechanical rename
needs nothing, a one-line change to `replacementScore()` needs one. Start from
`.claude/plans/TEMPLATE.md`, and **name the file after its content**, never an auto-generated string.

**Write for a future maintainer arriving cold**, not for the agent doing the work, and keep it
proportional — a document longer than the diff it describes means either the change is riskier than
it looks or the document is padding.

**Durable decisions and rationale get committed; execution detail does not.** Ordering, file-by-file
edit lists and implementation checklists belong in the scratchpad unless the task is complex, paused,
or handed to someone else. A separate implementation plan is rarely worth writing and almost never
worth committing.

**Lifecycle.** Land the doc in one logical commit before first publishing it for review. After that
the branch is reviewed history: **never force-push it just for tidiness** — add a normal follow-up
commit and squash at merge if compact history is wanted. Keep the document through design review,
then delete it in the same PR once Harvest is complete. Git history preserves it, so a link from an
old comment stays resolvable.

Its Harvest section names where each durable decision ends up. Prefer a source comment, Key Source
Facts, or `Docs/Changelog.md` for anything that matters — a PR body is fine for working detail but is
editable and lives outside Git, so important measurements should also be reachable from the tree.
Anything durable living only in the plan has not been harvested yet.

**Delete only when all three hold**: no inbound references, no deliberate spec/ADR role, and every
durable item has a discoverable destination. Plans for **unstarted** work are specs and stay. So do
records whose rationale is too substantial to inline — `.claude/plans/tsan-lazy-smp.md` is one, cited
from `Docs/CI.md` for survey and cost analysis with no other home.

## Subagent Dispatch

- **Do exploratory and verification work in the controller session**, then hand the implementer a
  closed list. Don't delegate open-ended exploration.
- **Always give an explicit worktree-relative binary path.** A `..` path pointing outside the
  worktree can be satisfied by the main repo's stale binary while producing wrong results, and
  "file not found" guards do not catch a stale one.
- Build from current sources before dispatching anything that verifies against binary output.
- **Long background waits do not reliably resume a subagent's turn.** Check in every 15-20 minutes
  on anything long-running rather than waiting for a notification.

## Shell Notes

- The `Bash` tool is Git Bash, not PowerShell. PS7 syntax (`$var`, backtick escapes,
  `Where-Object`/`Select-String`, multi-line strings) fails silently when inlined into bash. Write
  non-trivial PowerShell to a `.ps1` file and run it with `pwsh -ExecutionPolicy Bypass -File`.
- **Editing `.ps1` files**: multi-line `sed`/bash substitutions mangle backslashes and
  line-continuation backticks — use a small Python script written to a temp file. Validate without
  executing: `[System.Management.Automation.Language.Parser]::ParseInput($c, [ref]$t, [ref]$errors)`.
