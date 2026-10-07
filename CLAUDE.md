# CLAUDE.md – StratChessEvolved

A modern C++23 chess engine focused on improving playing strength (Elo) while maintaining clarity,
efficiency, and robustness.

This file holds the rules that change what you do. Detail is pointed at, not duplicated:

| Need | Read |
|---|---|
| module ownership, state lifetimes, search lifecycle | `Docs/Architecture.md` |
| non-obvious API contracts before an engine edit | `Docs/EngineContracts.md` |
| coding standards, applied at review | `Docs/CodingStandards.md` |
| validation tiers, standing decisions, worktree gotchas | `Docs/Workflow.md` |
| what each CI workflow runs, and when | `Docs/CI.md` |
| coverage map + how to write a test | `Docs/TestDesign.md` |
| writing a test first, or diagnosing a bug or slowdown | skill `tdd` / `diagnosing-bugs`, then `Docs/TestDesign.md` → Testing and debugging traps |
| before any timing-sensitive measurement (Elo, nps, UCI latency or CPU-profile collection) | follow skill `measure-strength`, including its quiet-window rule |
| analysing games or positions for engine behaviour | skill `analyze-games` |
| recording a measurement, or reading past ones | `Measurements/README.md` |
| opening or updating a PR | skill `open-pull-request` |
| writing or changing a PowerShell script | skill `write-powershell` |
| triaging, refining or recommending closure of an issue | skill `triage-issue` |
| writing a design document | skill `write-design-doc` |
| writing or changing a skill or `CLAUDE.md` | skill `writing-for-agents` |
| reviewing another agent's artifact, or answering a review | skill `cross-agent-review` |
| issue tracker, triage labels, domain docs | `Docs/agents/` |
| history | `Docs/Changelog.md` |

Live backlog is GitHub Issues (`theEscape2207/StratChess`) via `gh`, bodies always `--body-file`.

## Build

- **Warnings are errors everywhere**, in Debug and Release on every compiler. Approved
  suppressions: `[[maybe_unused]]` for params used only in `assert()`; `static_cast<>` for
  intentional narrowing. `#pragma warning(disable)` lives only around `StdAfx.h`'s STL includes.
- `StratEngine/StdAfx.h` is the shared common-include header (no build precompiles it) — add
  frequently-used STL headers there, alphabetically inside the `#pragma warning push/pop` block, not
  in individual `.cpp` files.
- **Adding a `.cpp` needs no project edit.** `CMakeLists.txt` globs with `CONFIGURE_DEPENDS`; just
  create the file.

```powershell
.\build.ps1                          # engine + tests (Release, clang-cl)
.\build.ps1 main | tests             # one target
.\build.ps1 run-tests ["[tag]"]      # build tests, run fast tier (~[slow]), optional tag filter
.\build.ps1 extended-tests           # include [slow]
.\build.ps1 all -Config Debug        # debug build
.\build.ps1 main -Compiler msvc      # MSVC instead of clang-cl
```

From an agent shell: `pwsh -ExecutionPolicy Bypass -File <abs>\build.ps1 <target>`. It imports the
VS environment via `vswhere` itself — never hard-code a VS path.

**clang-cl is what ships**; MSVC is the second toolchain, for debugging and because it honours flags
clang-cl silently drops. **Never measure with an MSVC build** — the compiler gap shows up as a
phantom regression.

Visual Studio setup, `/clang:` flag traps, dependencies, the compiler cache and raw CMake:
`Docs/Workflow.md` → Part 3.

## Scripts

In `Scripts/`; they resolve working directory and build-output paths internally.
**They require PowerShell 7** — `powershell` (Windows PowerShell 5) fails on PS7 syntax. Invoke
`pwsh` with `-File` and an **absolute** path. Never dot-source (a dot-sourced script runs in your
scope, where its variables collide with yours and its `exit` ends your session), and never wrap in
`cmd.exe /c "..."` — that swallows output, so a failing script looks like a silent no-op.

```
pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\<name>.ps1
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
| `New-Worktree.ps1 -Name <task>` | Start a task needing its own directory |
| `New-TaskBranch.ps1 -Name <task>` | Start a task **in the current worktree** |
| `Get-Worktrees.ps1` | Session start, or before resuming an idle worktree |
| `Get-PrChecks.ps1 [-Pr n] [-Wait]` | "Is the PR green?" — exit 0 green / 1 failed / 2 running |
| `Sync-Master.ps1` | Bring `master` up to `origin/main` |

Measuring (`Run-EloMatch.ps1`, `Run-Bench.ps1`) → skill `measure-strength`. Opening a PR and
cleaning up after a merge (`New-PullRequest.ps1`, `Remove-Worktree.ps1`,
`Remove-MergedBranches.ps1`) → skill `open-pull-request`. **Writing or changing one → skill
`write-powershell`**, which carries the language traps that fail silently.

## Standing rules

**Speed serves strength; the goal is measured positive Elo, not nps.** Anything adding per-node work
— evaluation terms as much as compiler flags — gets a bench pass, and a measured slowdown needs a
stated benefit that outweighs it: skill `measure-strength`.

**CI is a gate** — `build-and-test-result` is required on `main` and a red run blocks the merge.
Linux Debug + sanitizers is the primary correctness gate; Windows CI covers the shipping toolchain.
Changing a workflow: `Docs/CI.md`.

**Threat model**: not network-facing, no privilege boundary, no attacker. External-input work aims at
robustness — a clear diagnostic and a clean exit — not security. Exploit mitigations need a reason
beyond sounding prudent: `Docs/Workflow.md` → Threat model.

## Engine contracts

`Docs/EngineContracts.md` carries the non-obvious API contracts, indexed by what you are editing —
read the relevant section before touching moves, the search service, search internals or
configuration. One tripwire is repeated here because violating it fails *silently*:

- **An aborted frame keeps no results.** Once a move's recursive search sequence is done — `pvs()`
  may run a reduced, a full-depth and a PV re-search first — the board is restored and `IsAborted()`
  checked, before any persistent write. So no TT store, PV row, killer or history write survives a
  child that never finished. Node counters and the quiescence stand-pat cutoff store are the
  documented exemptions; a write added above that guard must justify itself the same way.

## Dependencies

`spdlog`, `nlohmann/json` and `Catch2`; bumping one: `Docs/Dependencies.md`. **A new external
dependency needs explicit approval from the project owner** — ask, with a rationale.

## Commit & PR Conventions

PRs stay script-mediated — `New-PullRequest.ps1`, never `gh pr create` or a bare push.

- Every task forks fresh from `origin/main`; PRs target `main`. Two ways to run one, both enforcing
  that: a **per-task worktree** (`New-Worktree.ps1`) when work must be parked or run alongside
  another task, or a **task branch in the current worktree** (`New-TaskBranch.ps1`) for a run of
  small sequential PRs. In-place is sequential only — one worktree holds one branch.
- **One slice per session.** When an issue is split into several PRs, end the session at each
  slice's PR and hand off a one-line start prompt carrying the PR's CI state and any cleanup still
  due; the next slice starts fresh from the issue or plan. Every request re-reads the whole context,
  so earlier slices are paid for on every call.
- Local `master` is a personal scratch branch — safe to commit to, safe to let drift. Never fork a
  worktree from it. `origin/master` is retired; nothing should reference it.
- Keep commit messages short — detail goes in the PR body or chat. Commit each fix as it lands
  rather than reverse-splitting a combined diff at the end.
- Stage named files, never `git add -A` — it sweeps tool-downloaded trees into the commit.
- If a branch carries unrelated commits, cherry-pick the relevant ones onto a fresh branch from
  `origin/main`.

## Design Documents

Write `.claude/plans/<kebab-name>.md` before implementing when **either** the change has a decision
that could reasonably go more than one way *and* materially affects a contract, the architecture,
correctness, strength, performance or maintenance cost, or it rests on an assumption you cannot
verify from the code in front of you. File count is not the trigger: a ten-file mechanical rename
needs nothing, a one-line change to `replacementScore()` needs one. Writing one: skill
`write-design-doc`.

**The need only ratchets up**: a decision or unverifiable assumption surfacing mid-implementation
means stop and write the document, however simple the change looked. A spike's output is an answer;
keeping its code is a new change.

## Subagent Dispatch

- **Do exploratory and verification work in the controller session**, then hand the implementer a
  closed list. Don't delegate open-ended exploration.
- **Always give an explicit worktree-relative binary path.** A `..` path pointing outside the
  worktree can be satisfied by the main repo's stale binary while producing wrong results, and
  "file not found" guards do not catch a stale one. Build from current sources first.

## Shell Notes

- Run PowerShell in a PowerShell shell or from a `.ps1` file; PS7 syntax inlined into the Git Bash
  tool fails silently.
