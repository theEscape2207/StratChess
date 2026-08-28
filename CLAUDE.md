# CLAUDE.md – StratChessEvolved

A modern C++20 chess engine focused on improving playing strength (Elo) while maintaining clarity,
efficiency, and robustness.

This file holds the rules that change what you do. Detail is pointed at, not duplicated:

| Need | Read |
|---|---|
| non-obvious API contracts before an engine edit | `Docs/EngineContracts.md` |
| validation tiers, standing decisions, worktree gotchas | `Docs/Workflow.md` |
| what each CI workflow runs, and when | `Docs/CI.md` |
| coverage map + how to write a test | `Docs/TestDesign.md` |
| measuring Elo or nps | skill `measure-strength` |
| opening or updating a PR | skill `open-pull-request` |
| issue tracker, triage labels, domain docs | `Docs/agents/` |
| history | `Docs/Changelog.md` |

Live backlog is GitHub Issues (`theEscape2207/StratChess`) via `gh`, bodies always `--body-file`.

## Build

- **Only `x64` builds work** — the x86/Win32 configuration is not maintained for C++20.
- **Warnings are errors everywhere** — `/W4 /WX` on MSVC and clang-cl, `-Wall -Wextra -Werror` on
  GCC, in both Debug and Release. Approved suppressions: `[[maybe_unused]]` for params used only in
  `assert()`; `static_cast<>` for intentional narrowing. Never `#pragma warning(disable)` in source.
- Dependencies (spdlog, nlohmann/json, Catch2) are fetched and pinned by `FetchContent` into
  `build/_deps`, shared by every preset. Nothing to install, no sibling checkout to keep in step. A
  fresh worktree's first build needs network (~1 min).
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

The `.\` form above is the interactive one; from an agent shell invoke it the same way as everything
in `Scripts/` — `pwsh -ExecutionPolicy Bypass -File <abs>\build.ps1 <target>`.

`build.ps1` imports the VS developer environment itself via `vswhere`, so it works from a plain
shell, a git hook or an agent session — never hard-code a VS path. It also sets `core.hooksPath` to
`.githooks` on first run, so every worktree gets the tracked hook.

**clang-cl is what ships**; MSVC is the supported second toolchain, for interactive debugging, as the
one-word fallback if an install lacks the VS Clang component, and because it honours flags clang-cl
accepts and silently drops. **Never measure with an MSVC build** — the compiler gap shows up as a
phantom regression. `Get-BuildArtifact.ps1` defaults to the shipping build for that reason.

Visual Studio setup, `/clang:` flag traps, the shared deps cache and raw CMake: `Docs/Workflow.md`
→ Part 3.

## Scripts

In `StratChessEvolved/Scripts/`; they resolve working directory and build-output paths internally.
**They require PowerShell 7** — `powershell` (Windows PowerShell 5) fails on PS7 syntax. Invoke
`pwsh` with `-File` and an **absolute** path. Never dot-source (`$PSScriptRoot` is `$null` under
dot-source), and never wrap in `cmd.exe /c "..."` — that swallows output, so a failing script looks
like a silent no-op.

```
pwsh -ExecutionPolicy Bypass -File C:\...\StratChessEvolved\Scripts\<name>.ps1
```

Each script's `-?` help carries its flags and traps. Use the one in **your own worktree** — they
target the repo of their own `$PSScriptRoot`.

| Script | When |
|---|---|
| `Run-Tests.ps1 [tag]` | Any test verification |
| `Run-Lint.ps1` | Blocking clang-format and clang-tidy (`-Fix` to auto-fix) |
| `Validate-PreCommit.ps1` | Before every commit — the pre-commit hook runs it |
| `Validate-PrePR.ps1` | Before a PR — scopes itself to the change tier |
| `Compare-SearchEquivalence.ps1 -After <exe>` | The gate for a change claiming to preserve behaviour |
| `Measure-UciLatency.ps1 -Command <cmd>` | Protocol-level round-trip cost of one UCI command |
| `Run-PerftCheck.ps1` | Move generation vs a 142,953-position corpus (~25 min) |
| `New-Worktree.ps1 -Name <task>` | Start a task needing its own directory |
| `New-TaskBranch.ps1 -Name <task>` | Start a task **in the current worktree** |
| `Get-Worktrees.ps1` | Session start, or before resuming an idle worktree |
| `Get-PrChecks.ps1 [-Pr n] [-Wait]` | "Is the PR green?" — exit 0 green / 1 failed / 2 running |
| `Sync-Master.ps1` | Bring `master` up to `origin/main` |

Measuring (`Run-EloMatch.ps1`, `Run-Bench.ps1`) → skill `measure-strength`. Opening a PR and
cleaning up after a merge (`New-PullRequest.ps1`, `Remove-Worktree.ps1`,
`Remove-MergedBranches.ps1`) → skill `open-pull-request`.

## Standing rules

**Speed serves strength; the goal is measured positive Elo, not nps.** Anything adding per-node work
— evaluation terms as much as compiler flags — gets a bench pass, and a measured slowdown needs a
stated benefit that outweighs it. Compare **nps**, never node counts at fixed depth: node count is a
property of the search, not the machine code, which is what makes it the right *equivalence* check
(two builds of identical source must visit identical nodes at `Threads=1`). Choosing an instrument
and reading its error bar: skill `measure-strength`.

**CI is a gate** — `build-and-test-result` is required on `main` and a red run blocks the merge. A
SKIPPED leg reports success on purpose, so Docs and Tooling PRs are not blocked by jobs that
correctly never ran. **Linux Debug + sanitizers is the primary correctness gate; Windows CI covers
the shipping toolchain.** Neither replaces the other — clang-cl silently drops flags Linux can never
observe — so do not add a Windows Debug configuration to a gate. Details: `Docs/CI.md`.

**Threat model**: not network-facing, no privilege boundary, no attacker. External-input work aims at
robustness — a clear diagnostic and a clean exit — not security. Exploit mitigations need a reason
beyond sounding prudent; CFG was declined on exactly that basis (#218). Full statement:
`Docs/Workflow.md` → Threat model.

## Engine contracts

`Docs/EngineContracts.md` carries the non-obvious API contracts, indexed by what you are editing —
read the relevant section before touching moves, `Board`, the search service or search internals.
Three tripwires are repeated here because violating them fails *silently*:

- **`Move` equality is exact** — it compares the raw 2 bytes, flags included. Moves differing only in
  promotion piece, or quiet vs. capture on the same squares, compare unequal.
- **`ThreadData&` is the first parameter of every search method.** Search runs on `td.board`, never
  the game board, and writes nothing back to it.
- **An aborted frame keeps no results.** `pvs()`/`quiescence()` check `IsAborted()` immediately after
  each recursive call returns **and the board is restored**, so no TT store, PV row, killer or
  history write survives a child that never finished. Node counters are the deliberate exception —
  they measure work done, not results kept. A write added above that guard must justify itself.

## Development Guidelines

- C++20; favour `constexpr`, RAII, move semantics, strong types.
- Current external dependencies are `spdlog`, `nlohmann/json` and `Catch2`. **No new external
  dependency without explicit approval from the project owner** — ask, with a rationale.
- All changes must be thread-safe, especially around the transposition table.
- No regressions in search accuracy or Elo without explicit justification; keep behaviour
  deterministic.
- English, unambiguous naming and comments. **Comments describe the code as it stands** — no task or
  PR references, no point-in-time measurements, no describing what the code used to be. Keep them to
  1–2 lines unless they record a key fact or tripwire; history goes in the PR body or
  `Docs/Changelog.md`.

## Testing

`Docs/TestDesign.md` is the coverage map and the guide to writing tests — check it before adding any.
Execute validation steps autonomously; flag any step needing user assistance (interactive GUI, manual
input) rather than skipping it silently.

## Commit & PR Conventions

Opening or updating a PR, reviewer dispatch and post-merge cleanup: skill `open-pull-request`.
PRs stay script-mediated — `New-PullRequest.ps1`, never `gh pr create` or a bare push.

- Every task forks fresh from `origin/main`; PRs target `main`. Two ways to run one, both enforcing
  that: a **per-task worktree** (`New-Worktree.ps1`) when work must be parked or run alongside
  another task, or a **task branch in the current worktree** (`New-TaskBranch.ps1`) for a run of
  small sequential PRs. In-place is sequential only — one worktree holds one branch.
- Local `master` is a personal scratch branch — safe to commit to, safe to let drift. Never fork a
  worktree from it. `origin/master` is retired; nothing should reference it.
- Keep PRs small and logically scoped. Keep commit messages short — detail goes in the PR body or
  chat. Commit each fix as it lands rather than reverse-splitting a combined diff at the end.
- Stage named files, never `git add -A` — it sweeps tool-downloaded trees into the commit.
- Commit only what was explicitly asked for. If a branch carries unrelated commits, cherry-pick the
  relevant ones onto a fresh branch from `origin/main`.

## Design Documents

Write `.claude/plans/<kebab-name>.md` before implementing when **either** the change has a decision
that could reasonably go more than one way *and* materially affects a contract, the architecture,
correctness, strength, performance or maintenance cost, or it rests on an assumption you cannot
verify from the code in front of you. File count is not the trigger: a ten-file mechanical rename
needs nothing, a one-line change to `replacementScore()` needs one. Start from
`.claude/plans/TEMPLATE.md` and **name the file after its content**.

**Write for a future maintainer arriving cold**, and keep it proportional — a document longer than
the diff it describes means either the change is riskier than it looks or the document is padding.
**Durable decisions and rationale get committed; execution detail does not** — ordering, file-by-file
edit lists and checklists belong in the scratchpad. Landing the doc, the Harvest section and the
three conditions for deleting one: `Docs/Workflow.md` → Design document lifecycle.

## Subagent Dispatch

- **Do exploratory and verification work in the controller session**, then hand the implementer a
  closed list. Don't delegate open-ended exploration.
- **Always give an explicit worktree-relative binary path.** A `..` path pointing outside the
  worktree can be satisfied by the main repo's stale binary while producing wrong results, and
  "file not found" guards do not catch a stale one. Build from current sources first.
- **Long background waits do not reliably resume a subagent's turn.** Check in every 15–20 minutes
  rather than waiting for a notification.

## Shell Notes

- The `Bash` tool is Git Bash, not PowerShell. PS7 syntax (`$var`, backtick escapes,
  `Where-Object`/`Select-String`, multi-line strings) fails silently when inlined into bash. Write
  non-trivial PowerShell to a `.ps1` file and run it with `pwsh -ExecutionPolicy Bypass -File`.
- **Editing `.ps1` files**: multi-line `sed`/bash substitutions mangle backslashes and
  line-continuation backticks — use a small Python script written to a temp file. Validate without
  executing: `[System.Management.Automation.Language.Parser]::ParseInput($c, [ref]$t, [ref]$errors)`.
