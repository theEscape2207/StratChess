# StratChess Evolved

A UCI chess engine written in modern C++23, focused on playing strength while keeping the code
clear enough to keep changing.

This README owns the project introduction and first build/run steps. Continue with the
[Engine guide](Docs/EngineGuide.md) for usage examples, configuration and output interpretation;
the documentation map below routes implementation and contributor questions to their owners.

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

**x86-64 with BMI2 only** — Intel Haswell (2013) or AMD Excavator (2015) onwards. The sliding-piece
attacks are indexed with the PEXT instruction and there is no portable fallback, so ARM, including
Apple Silicon, cannot build or run the engine; CMake stops with a message saying so rather than
failing somewhere obscure. 32-bit x86 is not maintained either.

On AMD, PEXT is microcoded until **Zen 3 (2020)**. Zen 1 and Zen 2 run the engine correctly but
search considerably slower than the same core otherwise would.

The dependencies (spdlog, nlohmann/json, Catch2) are fetched and pinned by CMake's `FetchContent`,
so there is nothing to install first — the first build needs network access.

**Windows** — requires Visual Studio with the clang-cl component. `build.ps1` locates and imports
the developer environment itself, so a plain shell works:

```powershell
.\build.ps1                     # engine + tests, Release, clang-cl
.\build.ps1 all -Config Debug   # debug build
.\build.ps1 main -Compiler msvc # MSVC instead of clang-cl
.\build.ps1 run-tests           # build and run the fast test tier
```

**Linux** — GCC 15, which CI builds and tests with (older versions are not checked), and Ninja:

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
```

For a first UCI search, game configuration, perft, tactical tests and batch evaluation, follow the
[Engine guide](Docs/EngineGuide.md#build-and-run).

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
| [Docs/EngineGuide.md](Docs/EngineGuide.md) | Run and configure the engine, use the search API, interpret output |
| [Docs/Architecture.md](Docs/Architecture.md) | Module responsibilities, dependencies, state ownership and execution flows |
| [Docs/EngineContracts.md](Docs/EngineContracts.md) | Non-obvious API contracts to read before an engine edit |
| [Docs/Workflow.md](Docs/Workflow.md) | Standing decisions (validation strategy, speed/nps, threat model), validation tiers, review gates, runtime files |
| [Docs/CI.md](Docs/CI.md) | What each GitHub Actions workflow runs, and when |
| [Docs/TestDesign.md](Docs/TestDesign.md) | Test coverage map and how to write new tests |
| [Docs/Changelog.md](Docs/Changelog.md) | What changed and when |
| [Measurements/](Measurements/) | Every strength measurement taken, plus the setup and the recording convention |
| [CLAUDE.md](CLAUDE.md) | Contributor rules and routing to required workflows |

Keep detailed examples and references in their owning documents; maintain this short entry point
and its links when the build, launch interface or documentation structure changes.

## Licence

GPL-3.0. See [LICENSE.txt](LICENSE.txt).
