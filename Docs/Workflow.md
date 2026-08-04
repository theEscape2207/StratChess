# Workflow Reference

Detail that `CLAUDE.md` points at rather than carries. `CLAUDE.md` holds the rules that change
what you do; this file holds the background you consult when something is unexpected.

---

## Validation tiers

`Scripts\Validate-PrePR.ps1` scopes itself to what changed, so there is no judgement call to make.
`Scripts\Get-ChangeTier.ps1` is the single source of truth and is shared with CI
(`.github/workflows/build-and-test.yml`), so the two cannot drift.

| Tier | Matches | What runs |
|---|---|---|
| `Docs` | `*.md`, `Docs/**`, `.claude/plans/**` | Nothing — the pre-commit hook's fast tests already cover it |
| `Tooling` | `Scripts\Run-EloMatch.ps1`, `Run-Tests.ps1`, `Sync-Master.ps1`, `verify_mate_key.py`, `build_corpus.py`, `New-Worktree.ps1`, `Remove-Worktree.ps1`, `Get-Worktrees.ps1` | PowerShell syntax parse only — never compiled, never invoked by the engine |
| `Build` | `build.ps1`, `Scripts\Validate-*.ps1`, `New-PullRequest.ps1`, `Get-ChangeTier.ps1`, `.githooks/**`, `.github/**`, `CMakeLists.txt`, `*.cmake`, `CMakePresets.json` | Full: build + extended `[slow]` tests + tactical suite + self-play |
| `Engine` | `*.cpp`, `*.h`, `*.json`, **and anything unrecognised** | Full |

A mixed diff takes the **strictest** tier present. Two properties are deliberate and asserted by
`Get-ChangeTier.ps1 -SelfTest`: it **fails closed** (an unrecognised path gets the full run, never a
skip), and the validation machinery is itself `Build` tier — a change to `Validate-*.ps1` or to the
classifier can never take its own shortcut, since a classifier bug would otherwise be
self-concealing. `-Force` runs every gate regardless.

Background: PR #56 (a one-line `CLAUDE.md` fix) and PR #133 (a measurement-script change) both paid
a full build + extended-test + self-play cycle for a guaranteed pass — issue #124.

---

## When `search-reviewer` may be skipped

**Default is to dispatch.** Self-certification is permitted only when **every** condition below
holds; failing any one means dispatch.

Do not judge by "it's only logging". The question is whether the edit changes what the search
computes *or how fast it computes it* — a log call added inside a per-node path can cut NPS, and
under time control that means a shallower completed depth and a genuinely different move, which
neither fixed-depth tests nor a self-play PASS would catch.

1. Every changed line lies inside a `log_*` function, or is an argument expression to an existing
   `s_logger->…` / `log.…` / `spdlog::…` call.
2. **No log call is added or removed** — only the arguments of existing calls change. Adding a call
   site is the risky act, not editing one.
3. Every new argument expression is a pure read of already-computed values: no `td.`/`board.`
   mutation, no counter increment, no lazy initialisation, and no board query whose result depends
   on `DoMove`/`UndoMove` state. (`MoveFormatter::ToShort(move, board)` reads `GetPiece()`/
   `InCheck()` — never call it after a failed or unpaired `DoMove`; use the board-free `ToCoord` in
   search diagnostics.)
4. No changed line lies inside `pvs`, `quiescence`, `search_with_aspiration`, `iterative_deepening`,
   `assess_iteration_quality`, `adjustScoreForGameState`, `should_stop_early`, or
   `should_try_null_move`.
5. No numeric literal, comparison operator, or control-flow keyword changed anywhere in the file.
6. `SearchTuning` in `AIPerplex.h` is untouched. A one-character constant change there is the
   highest-Elo-density edit in the repo and the least alarming-looking diff in it — it never
   self-certifies.

When skipping, say so in the PR body: *"search-reviewer skipped: logging-only, criteria 1-6."*
That makes the skip an auditable claim rather than a silent omission.

`New-PullRequest.ps1` reminds on **any** touch to the reviewed files rather than detecting the above
automatically. The asymmetry is the point: a false positive costs one subagent dispatch, a false
negative merges an unreviewed search change that surfaces weeks later in an Elo match, if at all.
Never teach the script to suppress the reminder — escalating it is fine.

---

## Worktree removal gotchas

All three are handled by `Remove-Worktree.ps1`; they still apply when doing it by hand.

- **Never remove a worktree from inside it.** git deregisters it but cannot rmdir its own cwd, so
  the leaf survives with no `.git`, and the shell's cwd gets stuck pointing at it while git commands
  silently resolve against the *outer* repo. If you hit this, use absolute paths / `git -C <path>`
  and don't trust `pwd`.
- **A locked directory is not a failure.** On Windows git often deletes every file but cannot rmdir
  the folder while a process holds it open. Deregister and carry on to the branch deletion —
  stopping there is how orphaned branches accumulate.
- **Detached worktrees keep a sibling branch.** Claude Code auto-mode worktrees are detached with a
  `claude/<dir-name>` branch parked at the same commit; removing the directory alone leaves that
  branch behind. For a worktree created via `EnterWorktree`, `ExitWorktree(action:"remove")` is
  cleanest.

`Remove-Worktree.ps1` verifies the branch is an ancestor of `origin/main` before deleting anything,
so a **squash-merged** PR is reported rather than deleted — its commits are not ancestors even
though the content landed. Confirm with `git diff origin/main <branch> --stat` (empty means safe)
and re-run with `-Force`.

`Get-Worktrees.ps1` also reports directories under `.claude\worktrees` that are absent from
`git worktree list` — the residue of a half-succeeded removal, which no other cleanup path can see.

---

## CI

`.github/workflows/build-and-test.yml` runs an independent build + fast-test check on **Linux**,
tier-gated by `classify` on pull requests and on merges alike: Build and Engine changes only, so a
Docs or Tooling change skips it either way.

**How `classify` reads each event.** A pull request diffs against `origin/main`. A push cannot —
on a push to `main`, `origin/main` *is* `HEAD`, so the diff is empty and every merge classified as
Docs regardless of content (#185). Pushes therefore diff against `github.event.before`, the tip
`main` held before the push. If that ref is unreachable — a force push, or the all-zeros SHA on
branch creation — `Get-ChangeTier.ps1` fails closed to Engine tier. Leave that path alone.

Consequence for the deps cache: `main` now only builds on Build/Engine merges, and `actions/cache`
is branch-scoped so a PR can only restore a cache saved there. This is safe because the key is static
and changes only on a dependency bump, which is a `CMakeLists.txt` edit and so still Build tier — and
because eviction is seven days without *access*, which every PR restore refreshes.

**Windows runs on demand only**, via the `windows-ci` label on a PR. It bills at 2x and was roughly
three quarters of this repo's Actions spend while it ran on every merge. Apply the label when the
diff can change **what the Windows build produces**: `CMakePresets.json`, `build.ps1`, `Compat.h`,
the clang-cl branch of `strat_configure_target`, or any `_MSC_VER`/`_WIN32` conditional. Two of the
three clang-cl flag spellings fail *silently* when wrong (#84), so Linux cannot stand in for those.

Ordinary engine changes do **not** need it. `Validate-PrePR.ps1` builds them with clang-cl locally
and is a strict superset of the job, `build-linux` covers the clean checkout from the same
`CONFIGURE_DEPENDS` glob, and `sanitize-linux` covers the fault class MSVC's debug STL might
otherwise have caught. That reasoning holds only if the local script actually ran — a PR pushed
around `New-PullRequest.ps1` (the PR #148 failure mode) should be labelled regardless.

What stays uniquely uncovered when the label is absent: **Windows-specific Debug**.
`Validate-PrePR.ps1` never passes `-Config`, so it takes `build.ps1`'s Release default, while the
Windows job runs a Release+Debug matrix. Closing that locally instead is the open option in #187.

The label itself did not exist in the repository until 2026-08-04 — between #182 introducing the gate
and then, it referenced something nobody could apply. Nothing was missed (the only merge in that
window would not have qualified), but a gate whose label is absent fails open and silently.

**`sanitize-linux`** builds the test binary with `-fsanitize=address,undefined` and runs the fast
tier. It shares `build-linux`'s trigger exactly — Build and Engine tiers, on PRs and merges alike —
deliberately, rather than being narrowed to Engine: a Build-tier change to `CMakeLists.txt` is
precisely what can break the sanitizer wiring, and two conditions would eventually drift apart.
It is the only job that can catch a *silent* fault: an
out-of-bounds read of the magic tables, a PST, a killer/history table or the mailbox does not crash,
it returns a wrong evaluation. Debug rather than Release, so `assert()` and the `#ifndef NDEBUG`
tripwires stay live alongside the instrumentation. Linux-only — the GNU `-fsanitize=` spelling does
not survive the MSVC driver, and `CMakeLists.txt` raises a configure error rather than letting a
Windows build look instrumented when it is not.

**CI is advisory, not a gate.** Required status checks need GitHub Pro on a private repository, so
nothing here blocks a merge — the result has to be read. `build-and-test-result` is kept as a single
always-present check so that enabling protection later is a settings change rather than a rewrite.

`Check starting FEN` is path-filtered to `StratChessEvolved/game_settings.json` and does not run
otherwise.

Runner image is pinned to `windows-2025-vs2026`, not `windows-latest`, so the toolchain moves only
when it is changed deliberately — see `.claude/plans/full-build-test-ci-github-actions.md`.

Dependencies come from `FetchContent` at the versions pinned in `CMakeLists.txt`, cached as
`build/_deps` and shared by both matrix legs. The job runs `build.ps1 tests` (not `all`): CI never
runs `StratChessEvolved.exe`, and only the engine target carries `INTERPROCEDURAL_OPTIMIZATION` for
Release, so building `all` would spend most of the wall time on an LTO link of an unused binary.

Extended `[slow]` tests and self-play stay local-only (`Validate-PrePR.ps1`) — self-play's
timeout-based nondeterminism is not worth the CI flakiness.

---

## Runtime output files

All paths are relative to the **working directory**, not the exe location. Run the exe from
`StratChessEvolved/` so `game_settings.json` resolves and output lands in `StratChessEvolved/logs/`.

| File | Created by | Context | Notes |
|---|---|---|---|
| `logs/multisink.txt` | `Logger::InitDefault()` (`Logger.cpp`) | Game mode only; **not** in tests | trace→file, info→console |
| `logs/aiperplex.log` | `AIPerplex::SetVerboseLogging(true)` (`AIPerplex.cpp`) | Whenever AIPerplex is constructed | Level `off` (file stays empty) when verbose is disabled afterwards, which is what tests do |
| `logs/SimplePerfStats.txt` | `Logger::EnsurePerfLogger()` (`Game.cpp` only) | Game mode only — `Game::Init()` is the sole creator | Written per AI move by `StopTimerAndAdjustVars()`, which only writes if a logger already exists; no file in tests, the tactical runner or UCI mode |
| `logs/gamelist.txt` | `Game::CreateGameMoveFile()` (`Game.cpp`) | Game mode only | One line per move via `MoveFormatter::ToShort` |

All four are gitignored. `logs/` does **not** need to pre-exist — spdlog's `file_helper::open` calls
`os::create_dir()` on the parent path. spdlog *does* swallow a genuine `basic_file_sink` constructor
failure silently, so a permissions problem produces no file and no error message.

---

## Working in Visual Studio

Open the repo as a **folder**, not a solution — VS reads `CMakePresets.json` and offers the presets
in its configuration dropdown.

Debugger arguments and working directory live in `.vs/launch.vs.json` (gitignored). Set
`"args": ["game"]` for game mode, and **`"currentDir": "${workspaceRoot}\\StratChessEvolved"`** —
`game_settings.json`, `logs/` and the `Tests/` lookup all resolve against the working directory, and
`TacticalTestRunner` takes its *parent*, so only that directory satisfies all three. VS defaults to
the executable's own folder, which satisfies none of them.

---

## Dependency cache

`FetchContent` clones spdlog, nlohmann/json and Catch2 on the first configure of each build tree, so
**a fresh worktree's first build needs network**. The committed presets place them in
`${sourceDir}/build/_deps` — per worktree, and the path CI caches.

`build.ps1` overrides that at configure time with `-D FETCHCONTENT_BASE_DIR=<repos>/StratChessDeps`,
a single cache beside the main checkout — so only the first worktree on a machine ever clones and the
rest need no network. It is skipped when `GITHUB_ACTIONS` is set, leaving CI on `build/_deps` where
its cache key expects them.

Done via `-D` rather than a preset because `CMakeUserPresets.json` cannot redefine a preset that
`CMakePresets.json` already declares — duplicate names are a hard error — and inheriting under a new
name would change `binaryDir` too, which `Get-BuildArtifact.ps1` depends on. `CMakeUserPresets.json`
is gitignored regardless: it is CMake's per-developer override file and is machine-specific by
definition.

---

## Two CMake settings that are load-bearing

- **`CMAKE_RC_COMPILER=llvm-rc`** — the Windows SDK `rc.exe` is the only tool in this chain that is
  not long-path aware, and fails with `RC1109` under a deep build path.
- **`/clang:` prefixes** on the warning and constexpr flags. clang-cl silently mistranslates the
  plain GNU spellings (`-Wall` is read as `/Wall` → `-Weverything`; `-fconstexpr-steps=` is dropped
  before reaching the frontend), and MSVC ignores them with a D9002 warning while still producing a
  binary. A clean compile does not prove a flag arrived — check `cmake --build --verbose`.

---

## Raw CMake invocation (fallback)

`build.ps1` is the documented path: it imports the VS developer environment via `vswhere` and then
drives the presets. Use these only when driving CMake directly, from a **VS Developer PowerShell**
so the compiler, `ninja` and `cmake` are on `PATH`.

```
cmake --preset windows-clang-cl            # configure (once per build tree)
cmake --build --preset windows-clang-cl    # build every target
cmake --build --preset windows-clang-cl --target StratChessTests
```

Presets: `windows-clang-cl`, `windows-clang-cl-debug`, `windows-msvc`, `windows-msvc-debug`.
clang-cl is what ships; MSVC is for development and debugging only, never for measurement.

Configuring is only needed when the build tree does not exist — Ninja re-runs CMake by itself when
`CMakeLists.txt` or `CMakePresets.json` change. Add `--verbose` to `cmake --build` to see the real
compiler command lines, which is the only reliable way to confirm a flag actually reached the
compiler rather than being silently dropped.

---

## Reproducing a sanitizer finding

There is no preset and no `build.ps1` verb: `-fsanitize=` is refused on MSVC and clang-cl, so this
only ever runs on Linux. On WSL, the export-and-extract route in the CI job's image:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSTRAT_SANITIZE=address,undefined
cmake --build build --target StratChessTests --parallel
./build/StratChessTests '~[slow]'
```

`STRAT_SANITIZE_RECOVER=ON` is the one deliberate exception to that invocation. It drops
`-fno-sanitize-recover=all`, so the run reports everything it finds instead of aborting on the first
hit — useful when surveying, and useless as a gate, because a recovering build exits 0 with the
findings only in the log. Pair it with `ASAN_OPTIONS=halt_on_error=0`. CI leaves it off.

Building over `/mnt/c` does not work — `FetchContent` fails with `configure_file: Operation not
permitted`, because DrvFs cannot perform the permission operations CMake asks for. Extract onto
native ext4.

---

## Self-play validation

- AI vs AI is fully headless: `Game::Run()` terminates on checkmate/stalemate; no stdin needed.
- **The `game` argument is required.** No argument (or `uci`) routes into `UciHandler::run()`, which
  blocks on stdin and never runs `Game::Run()`.
- Subprocess pattern: `Start-Process ..\build\windows-clang-cl\StratChessEvolved.exe -ArgumentList "game"
  -PassThru -NoNewWindow -RedirectStandardOutput out.txt`, then `$proc.Kill()` after N seconds for a
  timed test, or `$proc.WaitForExit(msTimeout)` for a game expected to finish naturally.
- Verbose logging is on by default in game mode; each move logs `GetMove complete: move=…, depth=…,
  time=…ms, nodes=…, stable=…` to stdout.
- Use `"type": 6` for both sides to exercise AIPerplex. For changes to `PlayerAI`/`PlayerBase`,
  also verify with `"type": 3` (AIAgent).
- `game_settings.json` accepts C-style `/* */` comments via nlohmann, but PowerShell's
  `ConvertFrom-Json` rejects them — write plain JSON when generating configs programmatically.
