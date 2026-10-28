# Engine guide

**Purpose:** Help a reader build, run and use the engine, complete a first search, and interpret
its output. This document owns practical usage examples and the diagnostic output reference.

[Architecture](Architecture.md) owns module responsibilities, dependencies and state lifetimes;
[EngineContracts](EngineContracts.md) owns non-obvious obligations when changing the engine.
[Workflow](Workflow.md) and [measure-strength](../.claude/skills/measure-strength/SKILL.md) own
validation and measurement procedures. Keep detailed field declarations and defaults in their
source headers and configuration catalogue; link to them from examples here.

## Build and run

StratChess is a C++23 UCI engine with an interactive game mode and diagnostic CLI commands.
Search uses iterative deepening, PVS, quiescence and Lazy SMP; see the
[search mechanism index](Architecture.md#search-mechanism-index) for implementation entry points.

Start with [README building instructions](../README.md#building) for platform prerequisites and
the first build. [Workflow](Workflow.md#part-3--the-environment) covers the development
environment; [Dependencies](Dependencies.md) covers dependency maintenance.

On Windows, from the repository root:

```powershell
.\build.ps1 main
Set-Location StratChessEvolved
& ..\build\windows-clang-cl\StratChessEvolved.exe
```

On Linux, after the README's build, run `../build/StratChessEvolved` from `StratChessEvolved/`.
Configuration and log paths resolve from the **working directory**, not the executable location.

### A first UCI search

Enter these commands into the running engine. Wait for `uciok` after `uci`, `readyok` after
`isready`, and `bestmove` after `go` before starting another example or quitting.

```text
uci
setoption name Threads value 1
setoption name Hash value 128
isready
ucinewgame
position startpos moves e2e4 e7e5
go depth 8
```

For a timed search use `go movetime 1000` (milliseconds); `go infinite` searches until `stop`.
`go nodes 10000` requests a node-limited search; its polling granularity and thread-count semantics
are documented in [SearchLimits.h](../StratEngine/SearchLimits.h). Send `quit` to exit.

### Game and diagnostic modes

Use the same executable with one of these argument lists:

| Arguments | Task |
|---|---|
| `game` | Play using `game_settings.json`; player type selects human or search |
| `perft run 3` | Count legal move paths from the starting position |
| `perft divide 3` | Split that count by root move |
| `perft test` | Run the built-in perft suite |
| `tactical test` | Run the tactical suite |
| `tactical stability 3` | Repeat the tactical suite to check stability |
| `eval positions.fen` | Evaluate a file of FEN positions |
| `uci --log-commands` | Run UCI with received-command logging |

In UCI, `position fen <FEN>` sets an analysis position, `eval` prints its evaluation breakdown,
and `go perft 3` prints per-root-move counts. These observations have different uses; consult
[Workflow](Workflow.md#what-validates-what) before choosing validation for a change.

## Configure a run

**UCI:** `uci` lists the options available in this build, including their defaults and ranges.
Apply `setoption` while idle. UCI starts from its own defaults and does not load
`game_settings.json`. For example, `setoption name Threads value 4` enables four search workers.
`Hash` budgets TT entry storage; allocation rounds down to a power-of-two bucket count, so a
request can allocate less than its nominal size. Read the engine's `info string hash` response
for the actual entry memory and bucket count.

**Game mode:** edit [game_settings.json](../StratChessEvolved/game_settings.json). Under
`game.players.white` and `.black`, `search_limits` supplies per-move constraints (required for a
Search player), `search_tuning` supplies search parameters, and `threads` selects worker count.
`game.FEN` sets the starting position; absent, the game starts from the standard one. The file
supports C-style comments through nlohmann/json; PowerShell 7's `ConvertFrom-Json` also accepts
those comments.

[SearchTuning.def](../StratEngine/SearchTuning.def) is the catalogue of fields, defaults, ranges
and JSON/UCI names. JSON and UCI expose different subsets. See
[configuration contracts](EngineContracts.md#configuration) when changing that binding or handling
rejected updates, and [configuration ownership](Architecture.md#configuration-and-input-boundaries)
to locate the responsible modules.

## Use the search service from C++

Inside a target that compiles the engine sources, include `AIPerplex.h` and supply a root position
and per-call limits:

```cpp
Board board;
board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

AIPerplex ai(AIPerplexConfig{.default_depth = 20, .threads = 4});
SearchResult result = ai.Search(board, SearchLimits::fixed_time(std::chrono::milliseconds(1000)));
Move best = result.best_move;
```

Inspect `result.game_state` and whether the move is empty before playing it. The returned value
also carries score, depth, elapsed time, node counts and telemetry; see
[SearchResult.h](../StratEngine/SearchResult.h) for fields and
[SearchLimits.h](../StratEngine/SearchLimits.h) for other constraints.

To supply tuning, create a `SearchTuning` value and pass it through `AIPerplexConfig::tuning`.
For iteration updates, pass an `IterationObserver` as the third argument to `Search`.
[AIPerplex.h](../StratEngine/AIPerplex.h) declares the synchronous and asynchronous interfaces;
read [search-service contracts](EngineContracts.md#the-search-service) before integrating
`StartAsync`, callbacks or concurrent stop requests.

## Interpret search output

During search, UCI `info depth` lines describe accepted iterations: score, cumulative nodes,
elapsed time and principal variation (PV). The final `info depth` line is a summary with aggregate
counts and the accepted iteration's full PV. `seldepth` is the deepest ply the main thread reached
in that iteration, quiescence included, and never less than `depth`; `nps` is `nodes` per second of
`time`. `score cp` is in centipawns and `score mate` is a mate distance in moves,
from the root side's perspective. `bestmove` completes the search; `0000` indicates no move.
`hashfull` reports the fraction of sampled TT entries written since this search began, in
permille. Older occupied entries do not count; it measures neither total occupancy nor usefulness.

Iteration node counts describe the main worker. Final counts include helpers and work from a
rejected trailing iteration, so subtracting the last iteration's nodes from the final total only
isolates rejected work at `Threads=1`. Diagnostic counts describe work, not playing strength.

### Always-available telemetry

These payloads follow `info string`; zero-only categories can be absent:

| Payload | Meaning |
|---|---|
| `treenodes main N qs N` | Final main-tree and quiescence counts, summed over workers; together they equal the final UCI `nodes` |
| `singular eligible N verified N extended N verifynodes N` | Eligible singular candidates, verification searches, granted extensions and work inside verification searches |
| `singular multicut N` | Non-PV nodes that returned `beta` because the singular verification failed high at or above `beta` |
| `frontier skips N` | Moves skipped by frontier futility |
| `lmp skips N` | Moves skipped by late move pruning |
| `aspiration iterations N faillow N failhigh N fullwindow N failnodes N` | Iterations, failed windows, full-window fallbacks and nodes spent in failed windows |

For node units and comparisons across revisions, see [Workflow](Workflow.md#speed-and-nps).

Aspiration `failnodes` includes both main and quiescence trees but excludes the full-window
fallback's own work. Telemetry aggregates all workers; helpers also aspirate their first depth
around zero, so use one thread when interpreting iteration costs. Definitions and emission
conditions live in [SearchTelemetry.h](../StratEngine/SearchTelemetry.h).

### Enable additional counters

Configure CMake with `-DSTRAT_TT_STATS=1` for TT counts or `-DSTRAT_SEARCH_PROFILE=1` for search
profiles, then rebuild the executable. Run the following from the repository root in a
**Visual Studio Developer PowerShell**, so raw CMake can find the compiler and SDK:

```powershell
cmake --preset windows-clang-cl -B build/windows-clang-cl-profile -DSTRAT_TT_STATS=1 -DSTRAT_SEARCH_PROFILE=1
cmake --build build/windows-clang-cl-profile --target StratChessEvolved
```

The instrumented executable is `build/windows-clang-cl-profile/StratChessEvolved.exe`; pass that
path explicitly to diagnostic tools. This separate directory leaves the shipping build used by
bench and match scripts unchanged. The flags stay cached in the profile directory; the test
target always enables both regardless of these settings. For experimental setup, comparison
scripts and interpretation limits, use
[measure-strength](../.claude/skills/measure-strength/SKILL.md).

### TT statistics

`hashfull` samples entries written during this search; probe/store statistics help explain how
the table is used. A build configured with `-DSTRAT_TT_STATS=1` prints one line after each search:

```
info string ttstats mainprobes .. mainhits .. maincutoffs .. qsprobes .. qshits .. qscutoffs ..
                    stores .. declined .. filled .. refreshed .. evictstale .. evictcurrent ..
```

A hit is a key match; a cutoff is a hit the node returned on. `evictcurrent` counts stores that
overwrote a different position written during this search — the pressure a larger table would
relieve — and `evictstale` those that overwrote an older search's. The counting code is compiled out
of the default build; counter storage remains. Enabling TT statistics preserves the search tree.

**Read them from a game-like workload.** `bench` searches each position from a cleared or barely
filled table, so every `Hash` size reads low and near-identical — a false null. Use a self-play
game, or one long `go movetime` from a middlegame position, and compare sizes on the same workload.
[measure_tt_capacity.py](../Scripts/measure_tt_capacity.py) does that: it replays strength-lab games at fixed nodes per size.

### Search profile

A build configured with `-DSTRAT_SEARCH_PROFILE=1` prints per-node counters for
move ordering, LMR, node types, null move, pruning and quiescence after each search, each line only
when its first field is non-zero (`pruning`: when either field is):

```
info string ordering cuts N index I0/I1/I2/I3to5/I6plus latecut H/C/K/Q hashnodes N hashcuts N latenodes N latebands B/B/B
info string lmr reduced N reducednodes N researched N confirmed N researchnodes N
info string lmrhistory capped N less N more N killer N
info string nodetypes pv B/B/B cut B/B/B all B/B/B cutfaillow B/B/B
info string nullmove tried N cutoffs N failed N failnodes N
info string pruning rfp D1/D2/D3/D4/D5/D6plus floorbinds N
info string qsearch roots N delta N see N maxdepth N
```

- `cuts` are `pvs()` fail-highs below the root, singular verification frames excluded. `index` bins the cutting move's legal index.
- `latecut` classifies cuts at index > 0 by the cutting move: hash move, else capture or promotion,
  else killer, else quiet.
- `hashnodes` counts cut nodes that had a hash move, and `hashcuts` those where it made the cut.
- `latenodes` counts the nodes, both trees, spent on the moves searched before a late cut.
  `latebands` splits it by the cut node's depth: 1-2, 3-6, 7+. A late cut nested inside another's
  earlier moves is counted once.
- `researched` counts reduced searches that beat alpha and ran again at full depth; `confirmed`,
  those that still beat alpha.
- `reducednodes` and `researchnodes` count the nodes inside the outermost search of each kind. A
  reduced search inside a re-search counts in both, so the two must not be summed.
- `lmrhistory` prints whenever `lmr` does, and its counts are over `reduced`. `capped` counts
  reductions whose base sat at the `depth - 2` cap, under whichever base `LmrLogDivisor` selects.
  `less` and `more` count ordinary quiet scores that moved R through `LmrHistoryDivisor`. `killer` counts a displaced killer's killer-tier score moving R.
  `Compare-SearchProfile.ps1` reports a side without the line as n/a, not 0.
- `nodetypes` counts `pvs()` frames past the quiescence hand-off, by depth band and by
  expected Knuth-Moore type: PV when searched as one, otherwise what the parent expected (a cut
  node's first move and a null-move child fail low, every other null-window move cuts; a singular
  verification is all-node). `cutfaillow` counts expected-cut frames that searched moves and failed
  low.
- `nullmove`: `tried` counts attempts, `cutoffs` those at or above beta, `failed` those completed
  below it, so an aborted attempt is in `tried` only. `failnodes` counts the nodes inside failed
  attempts, a nested failure once.
- `rfp` counts reverse-futility cutoffs by the node's depth; the last bin is open. `floorbinds`
  counts frontier fail-low floors that raised a node's best value. A node without two non-pawn
  pieces is frontier-pruned but never reverse-futility pruned, so the line prints on either field.
- `roots` counts main-search leaves handed to quiescence; `delta` and `see` count pseudo-legal moves
  each pruner skipped. `maxdepth` is the deepest quiescence frame entered; above the budget plus one
  only through checks. It combines across threads by max, not sum.

The counting code is compiled out of the default build. With the tie-break seed unset or zero,
profiling preserves the search tree. See [telemetry contracts](EngineContracts.md#telemetry-and-output)
before changing output or instrumentation.

A profile build also reads `STRAT_PROFILE_TIEBREAK_SEED` once at startup. A non-zero seed breaks
`MoveSorter` score ties by a seeded hash of the move instead of generation order, and the engine
prints `info string tiebreak seed N` before anything else; a value that is not an unsigned 32-bit
integer exits with a diagnostic. It is a neutral reordering, the noise source that
`Compare-SearchProfile.ps1 -Seeds` averages over. Unset or 0 leaves the build node-identical. The
test binary is a profile build too, so a seed left set in the shell reorders every search test.

## Versions and requirements

The engine reports its version in the reply to `uci`, as `id name StratChess <version>`:

| `<version>` | Meaning |
|---|---|
| `0.8.0` | A release build of version 0.8.0 |
| `0.8.0+3bd1ae39` | Built from commit `3bd1ae39`, after 0.8.0. Uncommitted changes are not marked |
| `0.8.0+unknown` | Built after 0.8.0 where Git could not name the commit, such as from a source archive |

The number lives in `project(... VERSION ...)` in [CMakeLists.txt](../CMakeLists.txt); only a
build configured with `-DSTRAT_RELEASE=ON` drops the `+` suffix. The version says which source a
binary came from. It is not the `info string benchcontract` number, which versions what a
measurement means.

The engine runs on x64 Windows with a CPU that has AVX2 and BMI2. It is a single `.exe` that links
the C++ runtime statically and takes the Universal CRT from Windows, so it needs Windows 10 or
later but no VC++ Redistributable or other runtime install. It imports only `KERNEL32.dll` and
Windows API sets. `Scripts/Test-StandaloneExe.ps1` checks both
and the files below.

At startup, before any engine code runs, the engine checks the CPU for AVX2 and BMI2. Without either it
prints one `Fatal:` line on stderr that names the missing feature and exits with code 1, instead of
dying on an illegal instruction. AMD Zen 1 and Zen 2 CPUs pass the check but run the engine well
below its normal speed: PEXT is microcoded on them.

Released builds are on the repository's GitHub Releases page as
`StratChess-X.Y.Z-windows-x64.exe`. To be notified, use Watch → Custom → Releases, or subscribe to
`https://github.com/theEscape2207/StratChess/releases.atom`. How a release is made:
[Releasing.md](Releasing.md).

### Supported UCI contract

Version numbers follow these compatibility rules. From 1.0, a breaking change is MAJOR. Before 1.0,
a breaking change, an additive feature or a measured strength gain is MINOR, and a fix is PATCH.
The rules cover:

- the `id` lines;
- the commands `uci`, `isready`, `ucinewgame`, `position`, `go` (with the parameters this guide
  documents), `stop` and `quit`;
- the `Threads` and `Hash` options: their name, type, range and default as advertised, and their
  `setoption` behaviour;
- the `info` fields that [Interpret search output](#interpret-search-output) defines;
- the `bestmove` line.

Every other advertised option, meaning the search tuning options `LmrLogBase` …
`StabilityTimeFloor`, is an **unsupported tuning control**. Any MINOR may rename or remove one, so a UI must not depend on it. `info string` lines
are diagnostics and not part of the contract.

## Logging and runtime files

For a UCI command trace, launch with `uci --log-commands` or
`uci --log-commands=logs/session.log`. Logging is opt-in, and received commands are flushed one
line at a time. Failure to open this command log produces a diagnostic on stderr and exits.
UCI output stays on the protocol channel; general console logging is disabled in UCI mode.

Without `--log-commands`, UCI mode needs no file beside the exe and leaves none: run alone from an
empty directory, it plays a search and the directory still holds only the exe. That is an observed
result, not a filesystem trace. `--log-commands` writes `logs/uci_commands_<pid>.log`, and
`--log-commands=<path>` writes that path verbatim, which may be outside `logs/`.

For an embedded service, enable verbose search logging at construction:

```cpp
AIPerplex ai(AIPerplexConfig{.verbose_logging = true});
```

Game mode enables this for search players. Debug search messages go to the file; info and higher
also go to the console. The default service leaves verbose logging disabled.

All paths below are relative to the working directory. `logs/` is created as needed; these files
are gitignored. Logger setup is best effort, so an unwritable location can leave general or search
logs absent. The explicit UCI command-log option reports failure as described above.

| File | Created by / when | Contents |
|---|---|---|
| `logs/multisink.txt` | `Logger::InitDefault()`, used by Game and verbose service setup | General messages, debug and higher; console sink starts at info |
| `logs/aiperplex.log` | Verbose `AIPerplex` construction initializes a shared sink | Iteration diagnostics and search summaries; each service gates its own writes |
| `logs/SimplePerfStats.txt` | `Game::Init()` | Per-move performance rows written by Game |
| `logs/gamelist.txt` | `Game::CreateGameMoveFile()` | Played moves |
| `logs/uci_commands_<pid>.log` | `uci --log-commands` | Received commands; an explicit path replaces this default |

Implementation: [Logger.cpp](../StratEngine/Utils/Logger.cpp),
[AIPerplex.cpp](../StratEngine/AIPerplex.cpp), [Game.cpp](../StratEngine/Game.cpp) and
[UCIHandler.cpp](../StratEngine/UCIHandler.cpp).

## Further reading and maintenance

- [Architecture](Architecture.md): locate responsibilities and follow execution/state flows.
- [EngineContracts](EngineContracts.md): preserve non-obvious obligations when changing behaviour.
- [Workflow](Workflow.md), [CI](CI.md), [TestDesign](TestDesign.md): choose and understand validation.
- [Measurements](../Measurements/README.md): recorded measurements and their provenance.
- [CLAUDE.md](../CLAUDE.md): contributor rules and required workflows.

Update an example or output definition in the same PR that changes its interface. Check examples
against source and keep parsed telemetry names aligned with their producers and consumers.
Before adding material, ask whether it helps a reader perform a task or interpret output; place
ownership explanations in Architecture and exact obligations in EngineContracts. Brief linked
context is useful; duplicated field catalogues, defaults and algorithm descriptions drift.
