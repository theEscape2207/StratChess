# ELO Baseline Measurement

Roadmap item: 🟡 High — step 3 of the Near-Term Sequence (`Docs/Roadmap.md`). Must land while
the engine is still deterministic (before Lazy SMP). User-approved design decisions:
**fastchess** as match runner, **10s+0.1s** time control, **committed standard opening book**.

## Goal

Differential strength-measurement infrastructure: a pinned reference build plus a repeatable
one-command procedure that measures any candidate build against it in ELO with an error bound.
Tactical suites verify correctness only — a change can pass 100% of tactical tests and still
lose 30 ELO; this closes that gap. The documented "baseline" is the harness-sanity result
(reference vs. itself ≈ 0 ELO), proving the pipeline before it is needed in anger.

**Scope limits:**
- No absolute ELO (needs third-party engines) — differential only.
- No SPRT accept/reject gating — fastchess supports it; adopt later if wanted.
- No CI integration — matches run hours, local-only (same policy as self-play).
- **Zero engine-code changes.** If the smoke match surfaces a UCI bug (see Step 4), it is
  fixed as its own commit — or its own issue/PR if non-trivial — never silently folded in.

## Design Decisions

1. **fastchess over cutechess-cli / custom runner** — actively maintained, built exactly for
   engine-vs-engine testing (pair-swapped colors, adjudication, concurrency, pentanomial ELO
   stats, EPD/PGN books, Windows release binaries). Roadmap named cutechess-cli as "e.g.";
   fastchess is its de-facto successor. Custom runner rejected: reimplements clocks,
   adjudication, and ELO math — a new source of bugs in the measuring instrument itself.
2. **Real time control (10+0.1), not fixed depth** — fixed depth is blind to speed: a 2×
   faster engine measures 0 ELO gain at fixed depth, which defeats the purpose of baselining
   before Lazy SMP (whose entire benefit is more nodes per second at the same clock). Cost:
   games are not bit-reproducible; that is inherent to time-based strength measurement and is
   handled statistically (error bounds over many games).
3. **Opening variety is mandatory** — the engine is deterministic, so self-play from the
   starting position produces one single game. A ~250-position slice of an established
   engine-testing opening book, played `-repeat` (each opening twice, colors swapped, so
   opening bias cancels), committed as small text under `Tests/openings/` for permanent
   reproducibility without re-downloading.
4. **Binaries stay out of the repo** — `<DepsRoot>EngineTesting\` (sibling of the repo, same
   convention as spdlog/nlohmann/Catch2, resolved via `Directory.Build.props` logic) holds
   `fastchess.exe` and cached reference exes named by tag. The opening book is the one
   committed artifact (small text).
5. **Reference pinned by git tag, exe rebuilt on demand** — tag `elo-reference-v1` on current
   `main` (`da06b3c`, post-ThreadData: the last deterministic engine before Lazy SMP work
   begins). The script rebuilds the cached exe from the tag via a temporary `git worktree`
   whenever it is missing, so the procedure survives a wiped deps folder; nothing depends on
   undocumented local state.
6. **Measurement history is append-only** — every run appends one line to `Docs/EloLog.md`
   (date, candidate commit, reference tag, games, TC, ELO ± error, notes). The log is the
   "documented baseline" deliverable and the audit trail for every future search change.
7. **Per-engine working directories** — fastchess `dir=` per engine, pointed at disposable
   temp dirs, so incidental engine output (`SimplePerfStats.txt` via
   `EnsurePerfLogger` in `StopTimerAndAdjustVars`) can never collide across concurrent games.

## Files Changed

| File | Change |
|---|---|
| `StratChessEvolved/Scripts/Run-EloMatch.ps1` | **New** — the one-command match runner |
| `Tests/openings/openings-250.pgn` (or `.epd`, per book format) | **New** — committed opening slice |
| `Docs/EloLog.md` | **New** — append-only measurement history + setup record |
| `Docs/Roadmap.md` | Item status + pointer to the re-measure procedure |
| `CLAUDE.md` | Row in the validation-scripts table for `Run-EloMatch.ps1` |
| `.claude/plans/elo-baseline-measurement.md` | This plan |

Git tag `elo-reference-v1` on `da06b3c`, pushed to origin (not a file, listed for completeness).

## Step-by-Step Changes

### Step 1 — Tooling setup (one-time, outside the repo)
- Create `<DepsRoot>EngineTesting\`.
- Download the latest stable fastchess Windows release from
  `https://github.com/Disservin/fastchess/releases`; record the exact version + URL in
  `Docs/EloLog.md`'s setup section. Verify `fastchess.exe --version` runs.
- Download an established opening set — first choice: the 8-move book used by Stockfish
  testing (`official-stockfish/books`, `8moves_v3.pgn`); fallback: a UHO EPD set (Stefan
  Pohl). Take the first 250 positions (deterministic slice, `order=sequential` keeps runs
  comparable), commit under `Tests/openings/` with a header comment noting source + license.
  Record source + trim rule in `Docs/EloLog.md`.

### Step 2 — Pin the reference
- `git tag elo-reference-v1 da06b3c && git push origin elo-reference-v1`.
- Reference exe build procedure (also implemented inside the script for cache misses):
  temp `git worktree add <tmp> elo-reference-v1` → `.\build.ps1 main` in it →
  copy `x64\Release\StratChessEvolved.exe` → `<DepsRoot>EngineTesting\StratChess-elo-reference-v1.exe`
  → `git worktree remove <tmp>`.

### Step 3 — `Run-EloMatch.ps1`
Same conventions as the existing `Scripts/*.ps1` (PS7, invoked with `-File`, resolves repo
root internally, exit code 0/1 with a PASS/FAIL-style summary). Parameters:
```
-CandidateExe  <path>   default: <repo>\x64\Release\StratChessEvolved.exe (must exist — the
                        script does NOT build the candidate; building is the caller's job)
-ReferenceTag  <name>   default: elo-reference-v1
-Games         <int>    default: 500 (250 pairs → ≈ ±15 ELO at 95%)
-Tc            <string> default: 10+0.1
-Concurrency   <int>    default: 4
-Smoke                  switch: 20 games, higher log verbosity — for pipeline checks
```
Behavior:
1. Resolve `<DepsRoot>` the same way `Directory.Build.props` does (user props override,
   else sibling default); fail with a clear setup message if `EngineTesting\fastchess.exe`
   or the committed book is missing.
2. Ensure the reference exe cache (rebuild from tag per Step 2 if missing).
3. Create two temp working dirs (candidate/reference); run fastchess:
   ```
   fastchess.exe
     -engine cmd=<candidate> name=candidate-<shortsha> dir=<tmpA>
     -engine cmd=<reference> name=elo-reference-v1    dir=<tmpB>
     -each tc=<Tc>
     -rounds <Games/2> -repeat -concurrency <Concurrency> -recover
     -openings file=<repo>\Tests\openings\openings-250.pgn format=pgn order=sequential
       (format=epd + .epd filename instead if the UHO fallback book is used)
     -draw movenumber=40 movecount=8 score=10
     -resign movecount=4 score=800
     -pgnout file=<repo>\logs\elo\<timestamp>.pgn
   ```
   (Exact flag names verified against the pinned fastchess version's `--help` during
   implementation — the release docs are authoritative, not this sketch.)
4. Parse the final ELO ± error and W/L/D line from fastchess output; print it; append the
   record line to `Docs/EloLog.md`; exit non-zero if fastchess crashed or any game was lost
   on illegal move / connection stall (those indicate engine or harness bugs, not strength).

### Step 4 — Smoke match (the UCI layer's first real trial)
- `Run-EloMatch.ps1 -Smoke` (candidate = reference build → expect ≈ 50% score).
- Inspect the PGN: games complete with proper results; spot-check promotions, castling,
  en-passant appear and are accepted (MoveFormatter::FromUCI's promotion path has never
  faced a real match runner); confirm no time forfeits (TimeManager headroom at 10+0.1) and
  no adjudication anomalies.
- Any engine/UCI defect found → fix as separate commit or file an issue (scope rule above).

### Step 5 — The baseline run + docs
- Full run: `Run-EloMatch.ps1` (500 games, unattended ~4-6 h at concurrency 4) with
  candidate = reference build. Expected: 0 ELO within the error bound. If outside: harness
  bias exists — investigate (engine dirs, first-move advantage handling, adjudication)
  before declaring the baseline usable.
- Create `Docs/EloLog.md`: setup record (fastchess version, book source/trim, reference
  tag/commit, machine note), the interpretation guide (e.g. "candidate at −20 ± 15 → likely
  regression; re-run doubled games before reverting"), then the first table row (the sanity
  run).
- `Docs/Roadmap.md`: mark the item done, Near-Term Sequence step 3 ELO part done; point
  to `Docs/EloLog.md`. `CLAUDE.md`: add `Scripts\Run-EloMatch.ps1` row ("after search/eval
  changes — measure strength vs pinned reference") to the validation-scripts table.

## Validation Plan

1. Script hygiene: PS7 syntax, invoked via the canonical `cmd.exe /c "pwsh -File ..."`
   pattern from repo root; graceful failure messages when tooling is missing (test by
   renaming `fastchess.exe` temporarily).
2. Reference-rebuild path: delete the cached reference exe, re-run script, confirm it
   rebuilds from the tag and produces a working engine.
3. Smoke match (Step 4) — 20 games, PGN inspected as described.
4. Full 500-game sanity match ≈ 0 ± error (Step 5) — the acceptance test of the deliverable.
5. `Scripts\Validate-PrePR.ps1` before the PR (script/config change → full gate applies;
   engine code untouched, so this is expected to pass trivially).
6. `game_settings.json` untouched by this task, but verify FEN before commit per convention.

## Key Correctness Properties

1. **Reproducible from the repo + public downloads alone**: tag + committed book + recorded
   fastchess version reconstruct the exact measurement setup; no undocumented local state.
2. **Bias cancellation**: every opening is played by both engines with both colors
   (`-repeat` pairs); openings come from a fixed, ordered, committed slice.
3. **Sanity invariant**: reference vs. reference measures 0 ELO within the stated error
   bound — verified once at setup (the baseline) and re-checkable any time.
4. **Measuring instrument cannot silently drift**: fastchess version and book are pinned;
   any change to either gets a new EloLog setup record.
5. **Engine untouched**: `git diff` of this task contains no `StratEngine/` changes (unless
   a smoke-match bug is found, which lands as its own clearly-labeled commit).
6. **Illegal-move/stall losses are treated as harness failures** (non-zero exit), never as
   strength data.
