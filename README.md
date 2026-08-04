# StratChess Evolved

A UCI chess engine written in modern C++20, focused on playing strength while keeping the code
clear enough to keep changing.

## Features

- **Search** — iterative deepening with principal variation search, quiescence search, null-move
  pruning and late move reductions
- **Move ordering** — MVV-LVA, two killer moves per ply, history heuristic
- **Board** — bitboards with PEXT magic sliding-piece attacks, Zobrist hashing
- **Transposition table** — shared across threads, separate main and quiescence phases
- **Evaluation** — tapered between middlegame and endgame
- **Parallel search** — Lazy SMP, configurable via the UCI `Threads` option
- **Time management** — soft and hard limits derived from the clock, increment and moves-to-go

## Building

x64 only. The dependencies (spdlog, nlohmann/json, Catch2) are fetched and pinned by CMake's
`FetchContent`, so there is nothing to install first — the first build needs network access.

**Windows** — requires Visual Studio with the clang-cl component. `build.ps1` locates and imports
the developer environment itself, so a plain shell works:

```powershell
.\build.ps1                     # engine + tests, Release, clang-cl
.\build.ps1 all -Config Debug   # debug build
.\build.ps1 main -Compiler msvc # MSVC instead of clang-cl
.\build.ps1 run-tests           # build and run the fast test tier
```

**Linux** — requires GCC 13 or newer (for `std::format`) and Ninja:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/StratChessTests '~[slow]'
```

Sanitizer builds are Linux-only:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSTRAT_SANITIZE=address,undefined
```

## Running

The binary speaks UCI by default, which is what chess GUIs expect. Run it from the
`StratChessEvolved/` directory so it finds `game_settings.json` and writes logs to
`StratChessEvolved/logs/`.

```
StratChessEvolved.exe                       # UCI mode (also: 'uci')
StratChessEvolved.exe game                  # self-contained game using game_settings.json
StratChessEvolved.exe perft run <depth>     # node counts from the start position
StratChessEvolved.exe perft test            # perft suite with known-correct counts
StratChessEvolved.exe tactical test         # tactical suite from Tests/
StratChessEvolved.exe tactical stability N  # N consecutive runs, flags nondeterminism
StratChessEvolved.exe eval <fen-file>       # batch-score positions
```

In UCI mode the engine also accepts **`go perft <depth>`** (or `perft <depth>`), printing per-root-move
node counts in the usual divide format:

```
position startpos
go perft 3
a2a4: 420
...
```

That is what external move-generation validators drive.

Per-player search limits, thread count and the starting position live in
`StratChessEvolved/game_settings.json`.

## Tests

Catch2 v3, in `StratChessTests/`. The fast tier runs in seconds; `[slow]` tests are excluded by
default.

```powershell
.\build.ps1 run-tests          # fast tier
.\build.ps1 run-tests "[eval]" # one tag
.\build.ps1 extended-tests     # including [slow]
```

## Documentation

| Document | Contents |
|---|---|
| [Docs/Engine-Readme.md](Docs/Engine-Readme.md) | Engine internals: search, evaluation, data structures |
| [Docs/Workflow.md](Docs/Workflow.md) | Validation tiers, CI, review gates, runtime files |
| [Docs/TestDesign.md](Docs/TestDesign.md) | Test coverage map and how to write new tests |
| [Docs/Changelog.md](Docs/Changelog.md) | What changed and when |
| [Docs/EloLog.md](Docs/EloLog.md) | Strength measurements against pinned reference builds |
| [Docs/Roadmap.md](Docs/Roadmap.md) | Direction; the live backlog is GitHub Issues |
| [Docs/Developer Guidelines.md](Docs/Developer%20Guidelines.md) | Coding conventions |

## Licence

GPL-3.0. See [LICENSE.txt](LICENSE.txt).
