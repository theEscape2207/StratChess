# UCI latency probe and received-command log — Design

**Issue:** #269

## Goal

Three sessions have written a throwaway harness to time a UCI command or to see what a client sent,
because neither capability exists. #213's ad-hoc probe repeated `position startpos`, so every sample
measured a TT clear — it published ~23 ms of invented per-move overhead before it was caught. #263
had to edit the engine, run a 20-game match and remove the instrumentation again, just to confirm
fastchess sends `ucinewgame`. #266 rebuilt the timing probe a third time.

Two small committed capabilities close both recurrences: a reusable command-latency probe with the
known measurement trap built into it, and a debug-gated log of the commands the engine receives.

## Scope

**This change will:**

- Add `StratChessEvolved/Scripts/Measure-UciLatency.ps1` — median/min/max round trip for an arbitrary
  UCI command against a control case, fresh process per case.
- Add an opt-in log of **received** commands to `UciHandler`, enabled by `uci --log-commands[=path]`.
- Register the new script in `Get-ChangeTier.ps1`'s Tooling allowlist (it is a fail-closed allowlist,
  so an unlisted script classifies as Engine), and document both additions in `CLAUDE.md` and
  `Docs/Workflow.md`.

**This change will not:**

- Log outgoing responses. The issue's question is "did the GUI actually send X?"; `send()` is one
  line away if a full transcript is ever wanted.
- Add a UCI option, a `game_settings.json` key or an environment variable to control the log — one
  mechanism, one contract.
- Change search behaviour, timing or protocol output. Enabling the log must be the only observable
  difference, and even then only in a file.
- Replace `uci_race_probe.py` or `Run-Bench.ps1`; they answer different questions.

## Decisions

### D1: The command log gets its own file-only logger; the default logger is never used

In UCI mode `main()` calls `spdlog::set_level(off)` and nothing calls `Logger::InitDefault()` —
`AIPerplex::SetVerboseLogging(false)` returns before `ensure_logger_initialized()` when no logger
exists yet. So the spdlog default logger is still spdlog's built-in **stdout** console sink, silent
only because the level is off. Logging through it, or raising the global level so a `spdlog::debug`
becomes visible, writes onto the protocol channel. `InitDefault()` is equally unusable here: it
attaches a `stdout_color_sink_mt` and installs it as the default.

So the log is written through a logger built on a single `basic_file_sink_mt`, with the global level
untouched. Rejected: `spdlog::debug()` on the default logger (stdout), and a stderr sink (a GUI
merging the streams reorders it, since stderr is unbuffered).

### D2: The logger is owned by the handler and never registered in spdlog's registry

`EnsurePerfLogger`'s shape — `std::call_once` plus `spdlog::register_logger` under a fixed name —
pins the first filename for the whole process and holds that file open until exit. That is right for
its single caller and wrong here: two tests with different temporary paths would silently share the
first one, and Windows refuses to delete a file whose sink handle is still open.

`Engine::Logger::CreateUciCommandLogger(filename)` therefore constructs and returns an
**unregistered** logger with no `once_flag`; `UciHandler` holds the only `shared_ptr` and drops it in
its destructor. Each handler instance gets its own file, and a fixture's temp file is deletable as
soon as the fixture goes out of scope. No registry name also means no `spdlog::get()` back door and
no duplicate-name throw. On sink-construction failure (spdlog swallows these) the factory returns
`nullptr`, and `EnableCommandLog` reports it on stderr and returns false.

### D3: The default path carries the process id

A match at `-Concurrency 6` runs six engine processes from one working directory, and
`basic_file_sink_mt` is thread-safe, not process-safe. Default `logs/uci_commands_<pid>.log`, relative
to the working directory like every other runtime file. An explicit `--log-commands=<path>` is used
verbatim: a caller naming the file has chosen to own that.

### D4: The line is logged before dispatch, and flushed per line

The diagnostic case is a command that hangs or crashes the engine — exactly the case a buffered,
after-the-fact write loses. One file write per command, only while enabled, never on the search path.

### D5: The exact CLI contract

```
StratChessEvolved.exe uci                        UCI mode, no log            (unchanged)
StratChessEvolved.exe uci --log-commands         logs/uci_commands_<pid>.log
StratChessEvolved.exe uci --log-commands=<path>  <path>, verbatim
```

- The flag is recognised **only** on an exact match of `--log-commands`, or on the prefix
  `--log-commands=` with a non-empty remainder. Deliberately not a `starts_with("--log-commands")`
  test, which would accept `--log-commandsfoo`.
- A token that begins with `--log-commands` and matches neither form (`--log-commandsfoo`,
  `--log-commands=`) is a near miss, i.e. a deliberate attempt to use the flag: diagnostic on
  **stderr**, exit 1. Silently ignoring it costs a whole debugging session. Note `--log-commands foo`
  is not malformed — it is the bare flag followed by an unrecognised token.
- Any other extra token after `uci` keeps today's behaviour (UCI mode starts anyway) with a one-line
  stderr note. Never stdout.
- Last occurrence wins if the flag is repeated. The flag is recognised in UCI mode only; `game`,
  `perft`, `tactical` and `eval` argument handling is untouched.

### D6: `run()` splits into a read loop plus `dispatch(line)`

The command loop is untestable today because it reads `std::cin`. Extracting the if/else chain into
`bool dispatch(std::string_view line)` (false = `quit`) makes routing and logging testable through the
existing `UciHandlerTestFixture`. Behaviour-preserving: `run()` becomes read-line → `dispatch`.

### D7: The probe's protocol — barrier, timed window, and what the control controls

`position` and `setoption` produce no response on success, so nothing can be "drained" by reading;
the only barrier the protocol offers is `isready`/`readyok`. Each repetition is therefore:

1. **Reset + setup:** send `ucinewgame`, then the `-Setup` lines, then `isready`; read until
   `readyok`. This both proves the setup landed and guarantees no unread output is pending when the
   stopwatch starts.
2. **Timed window:** start the stopwatch, send the command under test, read lines until the
   completion marker, stop. Lines that are not the marker (`info …`) are consumed and discarded.
3. Repeat. The first `-WarmUp` repetitions (default 1) are discarded, per #266's method.

Every read has a timeout (`-TimeoutMs`, default 30000); on expiry the script aborts naming the phase
and the last line it read, rather than hanging.

**Marker rules.** Default `-CompletionMarker readyok`, with `isready` appended after the command
under test. `-CompletionMarker bestmove` is for `go`, and appends nothing.

**Why the marker is not always `readyok`.** The command loop is single-threaded and dispatches
serially, so for anything that completes inside that thread — `ucinewgame`, `position`, `setoption` —
a following `readyok` is exactly the completion signal, which is what makes #266's method valid. But
`cmd_go` spawns the search thread and returns immediately, and `cmd_isready` has no in-search guard
(`refuse_while_searching` covers only `position` and `setoption`), so `readyok` after `go` comes back
mid-search and measures nothing. `cmd_go`'s thread emits exactly one `info` line and then `bestmove`.

**The control case** runs the identical reset+setup phase, then times the same read path with the
command under test omitted. Same preconditions, same code path, so harness and IPC overhead appear in
both and the *difference* is the command's own cost. One asymmetry, stated in the output: when
`-CompletionMarker` is not `readyok`, the control is still the `isready`/`readyok` round trip, so it
reports harness overhead rather than a baseline that can be subtracted.

### D8: Reset per repetition, so repetitions are independent

Repetitions share one process, so without a reset the TT and search state accumulate: a `go` at
repetition N runs against a table repetition N−1 filled, and the median then describes the harness
rather than the engine. The reset phase in D7 gives every repetition — and the control — the same
preconditions.

Two consequences, both documented in the script's help: when the command under test *is*
`ucinewgame`, what is measured is a `ucinewgame` that follows another one, which is the steady state
a GUI actually produces; and a `go` measured this way is deliberately a cold-TT search. `-NoReset`
skips the reset for anyone deliberately measuring warm-state behaviour, and the report states which
mode ran.

### D9: Fresh process per case, repetitions inside the process

Matches `Run-Bench.ps1`'s isolation. Process startup stays outside the measured window; a fresh
process per *repetition* would put it back in.

### D10: The drive-past-move-1 trap is built in, not just documented

Default `-Setup` is `position startpos moves e2e4 e7e5`, so every timed repetition runs from move 2.
`GetMove()` used to clear the TT at `fullMoveCount == 1` — that is what invalidated #213 — and #259
removed that specific clear, but "the first move is not a representative code path" is the general
hazard. The script warns when a `go`-style command is measured with a `-Setup` that establishes no
moves, and its comment-based help states the hazard and why.

### D11: The probe never enables the command log

It measures the shipping default. The log is a debugging aid, not part of the measured path.

## Assumptions I cannot verify from the code

- **A launcher can pass `--log-commands` to the engine.** fastchess takes per-engine `args` and GUIs
  expose an arguments field, but neither is verified here. Settled by a two-game `-Smoke` match with
  the flag set, checking the log appears — worth doing once, since the #263 scenario is the
  capability's main use.
- **PowerShell's stopwatch plus `ReadLineAsync().Wait(timeout)` resolves sub-millisecond round
  trips.** #266 measured 0.03 ms for `isready` with a Python probe. If the control reads near 1 ms
  instead, harness overhead dominates and no figure below ~1 ms from this script means anything.
  Settled by the control reading on the first run — record it either way.
- **#266's 37 ms is no longer reproducible on `main`,** because PR #274 removed the rebuild. The
  acceptance criterion is reproducing the *method*, not the number; the calibration case below is
  what proves the instrument still has resolution.

## Invariants

- Nothing new writes to stdout in UCI mode, with logging on or off.
- No log file exists unless `--log-commands` was passed.
- Fixed-depth node counts and best moves are identical at `Threads=1`, before and after.
- `quit` still terminates the loop; unknown commands are still ignored silently.

## Files

| File | Change |
|---|---|
| `StratChessEvolved/Scripts/Measure-UciLatency.ps1` | New. `Run-Bench.ps1`'s `ProcessStartInfo` setup and `Get-BuildArtifact.ps1` default-exe resolution; params `-Exe -Command -Setup -Repetitions -WarmUp -CompletionMarker -NoReset -Threads -TimeoutMs -Csv` |
| `StratChessEvolved/Scripts/Get-ChangeTier.ps1` | One line in the Tooling allowlist |
| `StratEngine/Utils/Logger.{h,cpp}` | `CreateUciCommandLogger(filename)` beside `EnsurePerfLogger`, carrying D1/D2 as its header comment |
| `StratEngine/UCIHandler.{h,cpp}` | `EnableCommandLog(path)`, `dispatch(line)` extracted from `run()`, one log call at the top of `dispatch` |
| `StratChessEvolved/StratChessEvolved.cpp` | D5's parser, applied only on the `uci` path |
| `StratChessTests/UCITests.cpp` | Tests via the existing `UciHandlerTestFixture` |
| `CLAUDE.md`, `Docs/Workflow.md`, `Docs/Changelog.md` | Script table row; runtime output files row; changelog line |

## Validation

**Engine tier** — `UCIHandler.cpp` is in the diff, so `Validate-PrePR.ps1` runs the full Engine tier
regardless of the script being Tooling.

**Equivalence (the gate).** Two binaries cannot share one output directory, so the `origin/main`
build is produced in a throwaway worktree and copied out before that worktree is removed — the
mechanism `Run-EloMatch.ps1` already uses for reference builds (lines 337-384):

```powershell
git -C <repo> worktree add --detach <main-repo>\.claude\worktrees\bench-main-269 origin/main
pwsh -File <tmp>\build.ps1 main                       # its own build\windows-clang-cl\
Copy-Item <tmp>\build\windows-clang-cl\StratChessEvolved.exe EngineTesting\bench-main-<sha>.exe
git -C <repo> worktree remove --force <tmp>
```

Then `Run-Bench.ps1 -Threads 1 -Depth 12` against `EngineTesting\bench-main-<sha>.exe` and against
this branch's `Get-BuildArtifact.ps1` path, one `-Csv` each: **the Nodes and Best columns must match
row for row across all eight positions.** Both are clang-cl Release. nps is secondary — the change
adds no per-node work.

Other checks:

- **Unit tests** (`[uci]`): logging disabled → no file created; enabled → the file contains the
  received line, including for a command the loop ignores; each fixture uses its own temp path and
  can delete it after the handler is destroyed (D2); `dispatch` returns false only for `quit`.
  Run them from the **Debug** build — Release hides what Debug catches on engine changes.
- **Probe self-calibration:** `-Command 'go movetime 200' -CompletionMarker bestmove` must report
  ≈200 ms, a known duration proving the instrument has resolution. Then `-Command ucinewgame` on
  `main`, expected near the control post-#274, and the control itself, expected ≈0.03 ms as #266
  measured. Record all three in the PR body.
- **Entry points:** `main()`'s argument handling changes, so exercise `perft run 3`, `eval <file>`, a
  `uci` handshake with and without the flag, each malformed-flag form from D5, and confirm an unknown
  arg still falls through to game mode. Automated checks drive UCI only, so a mis-parse here would
  otherwise go unseen.
- **No Elo match.** Nothing changes search decisions; a fixed-depth difference would be a bug, not a
  result.
- `search-reviewer` / `eval-reviewer`: not applicable — the diff touches neither `AIPerplex`, `Sort`
  or `ThreadData` nor `Eval.cpp`. State that in the PR body.

## Review dispositions

Cross-agent review of this design, [issue #269 comment](https://github.com/theEscape2207/StratChess/issues/269#issuecomment-5251029856).
All findings accepted; none required a change of approach.

| Finding | Disposition |
|---|---|
| Blocking 1 — response barrier and line consumption | Accepted → D7: `isready`/`readyok` fence, non-marker lines consumed, `bestmove` marker for `go`, per-read timeout |
| Blocking 2 — equal preconditions for control and measured path | Accepted → D8: reset phase per repetition, applied identically to the control; `-NoReset` for warm-state work |
| Blocking 3 — reproducible two-binary comparison | Accepted → Validation: throwaway worktree, copy the exe out, per `Run-EloMatch.ps1` |
| Add 1 — logger ownership and reset semantics | Accepted → D2: handler-owned, unregistered, no `once_flag`; the `EnsurePerfLogger` shape was the wrong model |
| Add 2 — exact CLI contract | Accepted → D5 |
| Clarify 1 — "serial" is inaccurate for `go` | Accepted → D7 states the loop is serial but `cmd_go` returns immediately and `isready` is unguarded mid-search |

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1/D2 — the UCI-mode default logger has a stdout sink, and why this logger is handler-owned | source comment on `CreateUciCommandLogger`, plus the `Docs/Workflow.md` runtime-files row |
| D3 — pid in the default filename, for concurrent match processes | source comment where the default path is built |
| D5 — the CLI contract, and that the log exists at all | `main()` comment; discoverability comes from the `Docs/Workflow.md` runtime-files row, which names what creates each log file. **Not** the `CLAUDE.md` script table — that table is for `Scripts/*.ps1`, and the flag is an engine argument |
| D6 — why `dispatch()` exists as a separate function | one-line comment on `dispatch` |
| D7/D8/D10 — barrier, reset semantics, and the drive-past-move-1 hazard | `Measure-UciLatency.ps1` comment-based help (the acceptance criterion's home) |
| The new script, and when to reach for it | `CLAUDE.md` script-table row |
| **How to build the `origin/main` binary a `Run-Bench` before/after needs** | `Docs/Workflow.md` → Speed and nps, as a short recipe block. This is the review's Blocking 3 and it has no in-tree home today: `EloMeasurement.md` and Workflow both say to *build the merge base*, but only `Run-EloMatch.ps1` knows how, and only for tag-resolved references — `-ReferenceExe` takes a path the caller produced by unwritten means. Generic to any bench comparison, so it does not belong in this issue's PR body alone |
| Review dispositions, and any approved decision that changes during implementation | PR body (CLAUDE.md's cross-agent review contract); the table above stays reachable through this file's git history |
| Measured control and calibration figures | PR body, and a `Docs/Changelog.md` line |
