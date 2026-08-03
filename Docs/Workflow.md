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

`.github/workflows/build-and-test.yml` runs an independent build + fast-test check on **Linux** for
every PR into `main` and every push to `main`. The push trigger both validates post-merge and warms
the deps cache on the default branch — `actions/cache` scopes per-branch, so a PR from a differently
named branch cannot restore a cache only ever saved under another branch.

**Windows runs on demand only**, via the `windows-ci` label on a PR. It bills at 2x and was roughly
three quarters of this repo's Actions spend while it ran on every merge. Apply the label to any PR
touching `StratEngine/**`, the CMake files, or the workflow itself — Windows is the only job that
compiles the clang-cl branch of `strat_configure_target`, the `_MSC_VER` sites in `Compat.h` and
friends, and the MSVC standard library, and the only one that builds what ships. Two of the three
clang-cl flag spellings fail *silently* when wrong (#84), so Linux cannot stand in for it.
`Validate-PrePR.ps1` covers the same ground locally and is a strict superset.

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
