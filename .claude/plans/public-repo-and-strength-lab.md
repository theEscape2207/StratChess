# Public repository and the CI strength lab

## Goal

Remove the constraint that makes measurement expensive, then spend the freed capacity on the one
thing this engine cannot currently do: resolve a change worth less than ~25 Elo.

Making the repository public gives unlimited GitHub-hosted standard-runner minutes and 20 concurrent
jobs. The first consequence is that every rationing decision in the current CI can be undone. The
second, and the reason this is worth doing at all, is that a 20-job matrix plays roughly 60
concurrent games against the 6 a local run manages — turning a 20,000-game batch (≈ ±4 Elo) from
"not attemptable" into an unattended background run.

The payload that justifies the work is epic #110: twelve eval sub-issues, each expected to be worth
single-digit Elo, i.e. twelve measurements that today's ±26 Elo instrument cannot make.

## Where this stands — read this first (2026-08-06)

**M0 through M5 are complete.** The sharded lab is calibrated at scale: a 20,000-game null test came
back **-2.17 ± 4.18**, agreeing with the single-job instrument and containing the true zero. **M6 is
next** — its section was rewritten against #217's measurements, which removed the concurrency blocker
it had been designed around and exposed a different one.

| | State |
|---|---|
| Repository | Public; CI is a gate, `build-and-test-result` required on `main` |
| Nightly | `nightly.yml` — deep perft, `[slow]` tier, sanitizers + `_GLIBCXX_DEBUG`, tactical ×100 |
| Opening book | Solved. `EngineTesting\openings-large.*` locally (optional), `UHO_4060_v3.epd` in CI — 242,201 openings |
| Strength lab | `strength.yml`, `workflow_dispatch` only, sharded **18 ways**, **calibrated at scale 2026-08-06** |
| Resolution today | **±4 Elo** at 20,000 games (CI, ~3 h); ±18 at 1000 single-job; ±25-26 at 500 (local) |
| Not yet possible | Epic #110's single-digit terms. That is exactly what M5 buys |

Where things live: method in `Docs/EloMeasurement.md`, results in `Docs/EloLog.md` (two ledgers,
never compared), workflow in `.github/workflows/strength.yml`.

**The default is 18 shards, not 20** (#217 Experiment A, 2026-08-06). 20 consumed the entire
concurrent-job allowance, so a run blocked every other PR's required check for three hours and was
therefore night-time-only. 18 leaves two slots free, costs **17 minutes** and no resolution —
**-1.51 ± 4.15** against the 20-shard **-2.17 ± 4.18**. Sections below that were written against 20
shards record what was measured at the time and are left as they stand.

Issues this work spawned, both open and neither blocking M5:

- **#204** — **fixed** (PR #212, 2026-08-06). `compute_budget()` could return a budget larger than
  the clock, so the engine forfeited at any increment below 100 ms and the standard 10+0.1 sat
  exactly on the floor with no margin. `hard` is now capped at half of `max(remaining - overhead, 0)`.
  This shaped M4's control run; the constraint on control runs is now weaker but has not gone away —
  see M5's time-control note.
- **#213** — move overhead is hard-coded at 50 ms with no UCI option to raise it. Opened by #212 as
  the deferred half of #204. Relevant here: a contended 20-way runner is where a too-small overhead
  would show up, though #212's cap means it can no longer compound into a forfeit.
- **#209** — `Docs/Workflow.md` needs a structure.

## Scope limits

- **`Run-Bench.ps1` never moves to CI.** nps on a shared, throttled runner is noise. Game-outcome
  Elo survives heterogeneous hardware because both engines share the runner; nps does not.
- **Local `Validate-PrePR.ps1` remains the pre-PR gate.** CI gains authority but does not replace it.
- **CI Elo is a separate ledger.** Linux/GCC numbers are not comparable to the clang-cl rows in
  `Docs/EloLog.md`. Ratios transfer; absolute values do not.
- No self-hosted runners, no paid larger runners, no third-party compute.
- This plan does not implement #184 (TSan) or #189 (MSan); it makes them cheap to schedule and says
  where they land.

## Key findings that shape the design

Established while writing this plan; each one changes a step.

1. **`Validate-PrePR.ps1` builds Release clang-cl only.** It calls `build.ps1 all` and
   `build.ps1 extended-tests` with no `-Config`, so Debug is compiled nowhere locally. The comment
   in `build-and-test.yml` claiming PrePR is "a strict superset" of the Windows job is true for the
   Release leg and **false for the Debug leg**. Fix the comment as part of M1.
2. **Neither Windows CI leg builds what ships.** `INTERPROCEDURAL_OPTIMIZATION_RELEASE` is set on
   `StratChessEvolved` (CMakeLists.txt:216); both legs build `StratChessTests`. A genuine
   shipping-configuration check has to build the `main` target.
3. **The opening book is the binding constraint on batch size.** `Tests/openings/openings-250.pgn`
   holds 250 positions. With color-swapped pairs that is 500 unique games — a local 500-game batch
   already exhausts it exactly, and a 20,000-game run would replay each opening 40 times. Fixing
   this is a prerequisite for large-N and improves local measurement immediately (M3).
4. **SPRT does not shard.** It is a sequential test; its stopping rule cannot be split across 20
   independent runners without a Fishtest-style aggregator. The resolution is to stop needing it:
   run large fixed-N batches in parallel and compute Elo and LLR once over the pooled PGNs. SPRT
   exists to save compute, and compute is what becomes free.
5. **`Run-EloMatch.ps1` already rebuilds the reference from a git tag on cache miss.** So "build
   both sides in the same job with the same toolchain" needs no new concept, and the
   never-measure-across-compilers rule holds by construction.
6. **History is 2.73 MiB packed, 94 objects, GPL-3.0 already in place.** One 4 MB blob exists in
   history: `StratEngine/Utils/MemLeakDetect.cpp.dump`, added at the initial commit and removed in
   e8045e1. A debugger dump of ~2010-era code — must be inspected before the repository is public.

## Design decisions

**Why public rather than paying for minutes.** Public standard runners are free and uncapped;
required status checks (currently unavailable — they need Pro on a private repo) become available,
so CI can stop being advisory; GHCR is free, which is what makes a prebuilt instrumented-libc++
image viable later for #189. Paying would buy only the first of those three.

**Why fixed-N pooled batches rather than SPRT in CI.** See finding 4. Keep `-Sprt` in
`Run-EloMatch.ps1` for local use, where compute is scarce and early stopping is worth its
complexity; CI uses fixed N.

**Why the strength lab is `workflow_dispatch` before it is automatic.** A measurement harness that
is wrong is worse than none, because its output looks exactly like a measurement. It gets calibrated
against a null test and a known-sign control (M4) before anything triggers it automatically (M6).

**Why nightly rather than per-PR for the heavy correctness work.** Deep perft, the extended tier and
the mutually-exclusive sanitizer families do not need to block a PR — they need to be *known*. Free
minutes remove the cost argument but not the wall-clock argument.

## Milestones

Each milestone is independently valuable and can be stopped after. M1 and M3 deliver before any lab
exists.

### M0 — Pre-flight audit (blocks M1; ~1 session, no code)

The only irreversible step in this plan is the visibility flip, so everything reviewable happens
first.

1. Secret-scan the full history: `gitleaks detect --no-git=false`, or `trufflehog git file://.`
   Both are free and run in minutes on a 2.73 MiB history.
2. Inspect `StratEngine/Utils/MemLeakDetect.cpp.dump` in history (finding 6) for absolute paths,
   machine names or anything else from the pre-import era. If it is unwanted, this is the moment to
   decide — rewriting history after the repository is public is pointless.
3. Read with fresh eyes, as a stranger would: `Docs/EloLog.md`, `Docs/Workflow.md`, `CLAUDE.md`,
   `.claude/plans/*` (all committed and all candid), and every open issue.
4. Confirm `logs/` and `game_settings.json` carry nothing local, and that `.gitignore` keeps it that
   way.
5. Set repository settings for a public repo *before* flipping: require approval for all outside
   collaborators' workflow runs (free-minute abuse is the standard attack), disable
   `pull_request_target` usage as policy, confirm no secrets exist that a fork PR could reach.

**Exit:** a go/no-go. Everything after this assumes go.

#### M0 outcome (2026-08-04)

Audited. **Go**, and the audit changed two assumptions.

`MemLeakDetect.cpp.dump` is a **cppcheck** XML dump (`<dumps><platform><rawtokens>`), not a memory
dump: symbol names plus three source paths under `C:/Development/Kode/StratChess/`. No credentials,
no personal identifiers. It is 338 KB packed of a 2.73 MiB pack. So it is harmless — but the
decision is still to remove it, together with an identity scrub, since a rewrite is only affordable
once and this is the moment.

The exposure that a rewrite is actually *for* is elsewhere: three author identities in history
(`ssigfred+git@gmail.com`, `oleerling@duck.com`, `thees@POMELO24` — the last carries a machine
name), plus `C:/Users/thees/...` paths in several plan files and three `Docs/` files.

M0 therefore splits in two, because only the second half is destructive:

- **M0a — content cleanup (normal PR).** Delete `Docs/Devlog.md` (superseded by `Changelog.md`),
  `Docs/Position_Class_Documentation.md` (602 lines documenting a `Position` class this engine does
  not have), `Docs/Improving Chess Engine ELO rating.md` (generic advice, superseded by the Roadmap
  and epic #110) and `Docs/superpowers/` (superseded by `.claude/plans/`). Fix the dangling
  `.superpowers/sdd/...` reference in `TestDesign.md`. Add the root `README.md` the repository has
  never had. `.claude/plans/` stays and ships public — it is the repository's own documented
  convention and the honest design record.
- **M0b — history rewrite (destructive, separate pass).** `git filter-repo --invert-paths` over
  `MemLeakDetect.cpp.dump` and `MemLeakDetect.cpp.dump-cert-results`, plus `--mailmap` collapsing
  the three author identities. Prerequisites and consequences:
  - All 503 commits are rewritten; every SHA changes. `elo-reference-v1` (fd8b665) and
    `elo-reference-v2` (df9245f) move with their tags, but the SHAs *quoted in prose* go dangling:
    **29 in `Docs/EloLog.md`**, 3 in `Docs/Changelog.md`, ~15 across `.claude/plans/`. filter-repo
    writes `.git/filter-repo/commit-map`; use it to remap those references mechanically in a
    follow-up commit rather than by hand.
  - Do it with no PR open (there were none at audit time) and with the three live worktrees removed
    first; every clone and worktree must be recreated afterwards.
  - Force-pushing does **not** purge the old objects from GitHub — they stay reachable through PR
    refs unless GitHub Support is asked to run gc. Harmless for this blob; worth knowing before
    anyone treats a rewrite as a way to unpublish something that matters.
  - Delete the retired `origin/master` and the stale `origin/worktree-*` branches in the same pass.

### M1 — Flip, and delete the rationing (immediate value, ~1 session)

Nothing new is engineered here. This milestone only removes constraints that exist because minutes
were scarce.

- Flip visibility to public.
- `build-and-test.yml`: drop the `windows-ci` label gate. Windows Release **and** Debug run on every
  full-tier PR again. Delete the label from the docs and from `CLAUDE.md`'s CI paragraph.
- Fix the inaccurate "strict superset" comment (finding 1) and the "only job that builds what ships"
  comment (finding 2) — the latter by either building the `main` target on the Windows Release leg
  or by correcting the claim. Prefer building `main`: it is the only way the ThinLTO link is ever
  exercised.
- Turn on branch protection with `build-and-test-result` as a required check. The single-check shape
  was kept deliberately for this moment; it is a settings change, not a rewrite.
- Add `concurrency:` groups keyed on ref so superseded runs cancel — with 20 concurrent jobs shared
  across the account, a stale run is now a real cost even though minutes are not.

**Value delivered:** the Windows coverage that was designed and then rationed away, on every PR; CI
becomes a gate.

### M2 — Nightly correctness (new signal, no new tooling, ~1 session)

New `.github/workflows/nightly.yml`: `schedule` (daily) + `workflow_dispatch`, on `main` only.

- **Deep perft matrix.** Perft is self-checking and splits perfectly by root move. Target `perft(6)`
  from the start position and Kiwipete first, then `perft(7)` startpos (3.19 G nodes) sharded across
  a matrix. This is the strongest correctness signal move generation can produce and the fast tier
  currently stops at depth 4.
- **Extended `[slow]` tier** on Linux, both configs.
- **`_GLIBCXX_DEBUG`** added to the `sanitize-linux` job — the libstdc++ analogue of MSVC's
  `_ITERATOR_DEBUG_LEVEL=2`, catching invalid iterators and container misuse that
  `_GLIBCXX_ASSERTIONS` (bounds only) does not. ABI-incompatible in general, but free here because
  all three dependencies are populate-only `FetchContent` sources compiled with our own flags.
- **Tactical stability ×100** instead of the local ×10.

**Value delivered:** perft depth this project has never tested, plus the iterator-debug class.

#### M1 and M2 outcome (2026-08-04)

Both done. M1 in PR #193, M2 in `nightly.yml` + `STRAT_STDLIB_DEBUG`.

Two corrections to what this milestone assumed:

- **Deep perft does not need sharding.** Perft runs at ~50 Mnps, making `perft(7)` about a minute and
  Kiwipete `perft(6)` about three. `perft run <depth> [fen]` already accepts a custom FEN, so no
  engine change was needed either.
- **`_GLIBCXX_DEBUG` runs on both jobs.** It was staged on the nightly first, since `sanitize-linux`
  feeds a required check, then promoted once a local GCC 13.3.0 build (the runner's compiler, via
  WSL) came back clean over the whole suite in 25 s.
- **The `[slow]` tier is three test cases**, 253 against the fast tier's 250. This milestone's
  "extended tier" jobs are therefore worth much less than the plan implied. The gap is test coverage
  (#156), not schedule.

Unbudgeted dividend from M1: public repositories get 4-core runners against the private tier's
2-core, so Linux jobs got 20-40% faster with no configuration change.

### M3 — A real opening book (helps local measurement immediately, ~1 session)

Finding 3. Replace or supplement `openings-250.pgn` with a large, balanced book — the UHO
(unbalanced-human-openings) families or `8moves_v3.pgn` are the conventional choices; anything with
≥ 5,000 positions removes the constraint for every batch size this plan contemplates.

- Keep `openings-250.pgn` for smoke runs; add the large book alongside and make the book a
  `Run-EloMatch.ps1` parameter with the large one as default.
- Record in `Docs/EloLog.md` that rows measured on the 250-position book are not directly comparable
  to later ones, and why.
- Verify the book is legal for redistribution before committing it to a public repository.

**Value delivered locally, before any CI lab exists:** every existing 500-game batch stops
exhausting the book at exactly its own size, and any batch above 500 games becomes meaningful.

#### M3 outcome (2026-08-05)

Done, with one deliberate departure: **the large book is not committed.**

The plan said to commit one and to check redistribution rights first. Those two pull against each
other now that the repository is public — a large book is third-party data of varying provenance, and
`Docs/EloLog.md` shows even the committed 250 openings are a cut of `8moves_v3.pgn`. Rather than
adjudicate that, the book follows the convention this repository already has for external test
assets: fastchess and the reference binaries live in `EngineTesting\` beside the checkout, never in
git. A book is the same kind of asset.

So `Run-EloMatch.ps1` gained `-Book <path>`, auto-resolving to
`EngineTesting\openings-large.pgn|.epd` when present and falling back to the committed smoke book,
with the fastchess `format=` flag following the extension.

The more valuable half turned out to be the **exhaustion warning**. The original problem was that
book exhaustion is invisible: a 5,000-game SPRT silently replays 250 openings twenty times and
reports a tighter error bar than it earned. Every run now prints the book and its opening count and
warns when `-Games` exceeds the 2N distinct games available. That makes the constraint self-reporting
rather than something a reader of this plan has to remember.

**Consequence to accept:** local measurement does not improve until someone drops a book into
`EngineTesting\`. The warning tells you when that matters, and until then behaviour is unchanged.

### M4 — Strength lab, single job, calibrated (~1-2 sessions)

New `.github/workflows/strength.yml`, `workflow_dispatch` only. One runner, one job.

Steps in the job: check out; build the candidate; `git checkout <reference-tag>` into a second build
directory and build that; build or fetch `fastchess` for Linux; run the match with
`-each proto=uci tc=10+0.1 option.Threads=1`, concurrency 3 (4 vCPU, leave headroom — a time loss
under contention invalidates the batch, and `Run-EloMatch.ps1` already treats time losses as fatal);
upload the PGN as an artifact.

Then calibrate before trusting it, and record both runs in the plan:

- **Null test.** Candidate and reference built from the same commit. Expected result: 0 Elo within
  the error bar. `Docs/EloLog.md` already documents this methodology (pooled 1000 games, -1.4 Elo)
  from the local instrument; the CI instrument owes the same evidence.
- **Known-sign control.** Same binary on both sides, one given half the time control. A large
  negative Elo must come out. This tests the harness independently of the engine — if it cannot
  detect a 2x time handicap, it cannot detect 5 Elo.

**Value delivered:** a trustworthy, if slow, second instrument that runs without touching your PC.

#### M4 outcome (2026-08-05)

`.github/workflows/strength.yml` exists and is dispatch-only. **The calibration runs have not
happened yet**: `workflow_dispatch` only offers a workflow that is already on the default branch, so
the two runs can only follow the merge. Until both come back right, nothing this workflow prints is a
measurement, and `Docs/EloLog.md`'s Linux ledger holds a placeholder row saying so.

Three things the plan did not anticipate:

- **The engine forfeits below a 0.1 s increment, so "half the time control" is the wrong control.**
  `compute_budget()` floors a move at 100 ms while 5+0.05 pays only 50 ms back, so once the clock
  drains the engine loses ground every move. Measured locally: **3 time losses in 4 games at 5+0.05,
  none at 5+0.1** — and at 2+0.02 the handicapped side flagged in all four. The control run must
  therefore halve the **base** and leave the increment alone (10+0.1 vs 5+0.1), which a 6-game local
  smoke confirmed is both clean and strongly signed. The workflow warns when either side is given an
  increment under 0.1 s.

  Two consequences beyond this milestone. The standard 10+0.1 sits *exactly* on the floor — its
  100 ms increment repays the minimum move cost and nothing more — so a slower or contended runner
  has no margin, which is a live risk for M5's 20-way concurrency and for the local instrument on any
  slower machine. And the engine cannot play bullet at all: this is a real strength limitation, worth
  its own issue rather than a workflow comment.

- **The book was already solved, far past what M3 assumed.** `UHO_4060_v3.epd` holds **242,201**
  openings, not the ~4,060 the filename suggests (that number is the evaluation band the positions
  were selected from). At 484,402 distinct games no batch in this plan can exhaust it, so M5 needs no
  larger book and the shard-disjointness work is purely about slicing, not supply.

- **`Run-EloMatch.ps1` was not reused, and should not be.** It is Windows-shaped throughout —
  `fastchess.exe`, the `EngineTesting\` sibling directory, tag-based binary caching, appending to
  `Docs/EloLog.md`. Making it cross-platform would put the local instrument's correctness at risk to
  save a workflow that shares only its adjudication settings, which are copied across verbatim
  instead. The two instruments deliberately differ only in toolchain and hardware.

Validated before commit rather than by dispatching and watching it fail: the YAML parses, every
`run:` block passes `bash -n`, and a local fastchess smoke confirmed the exact argument shape the
workflow uses (per-engine `tc=`, `option.Threads=1`, `-each proto=uci`, an EPD book with
`format=epd`) — that last one is what surfaced the increment finding.

**M4 is complete.** Both calibration runs passed on 2026-08-05, against `c2a9f78`, and are recorded
in `Docs/EloLog.md`'s Linux ledger:

| Run | Shape | Result |
|---|---|---|
| Known-sign control | 200 games, 10+0.1 vs 5+0.1, same binary | **+75.88 ± 42.56**, LOS 99.99% — detects a real difference |
| Null test | 1000 games, identical commit and clock | **-3.47 ± 18.21** — contains the guaranteed zero |

Both with **zero time losses**, which settles the risk this milestone raised about 10+0.1 having no
margin on a shared runner: at concurrency 3 on 4 vCPU it holds. The null result also matches the
local instrument's -1.4 at the same game count — two instruments on different toolchains and
hardware agreeing that neither is biased.

The control was run **before** the null test deliberately. A null result from a blind instrument is
indistinguishable from a null result from a working one, so proving the harness can detect anything
has to come first; otherwise "0 ± 18" is not evidence of anything at all.

The first dispatch found a packaging bug rather than a measurement: `fastchess` v1.8.0-alpha extracts
to a subdirectory, and the guard against `mv fastchess fastchess` compared basenames, so the binary
was never moved into place. Both builds had already succeeded. That is the argument for
dispatch-only stated as a fact rather than a principle — the first thing this workflow produced was
a defect in itself, and a `pull_request` trigger would have produced it on somebody's PR.

### M5 — Shard it (~1-2 sessions)

**Start here next session.** M4 replaced most of this milestone's guesses with measurements; the
numbers below are observed, not estimated.

#### What M4 measured that M5 needs

| Input | Value | Source |
|---|---|---|
| Throughput | **1000 games at concurrency 3 = 162 min** (~6.2 games/min/job) | The null-test run |
| Per-job overhead | ~7-10 min: checkout, two source builds, fastchess + book download | Same run |
| Book supply | 242,201 openings = 484,402 distinct games | `UHO_4060_v3.epd`, printed by every run |
| Time losses at 10+0.1, concurrency 3 | **Zero over 1200 games** | Both calibration runs |
| Single-job null to reproduce | **-3.47 ± 18.21** over 1000 games | `Docs/EloLog.md`, Linux ledger |

**Sizing follows directly.** 20 shards × 1000 games each ≈ 162 min per shard, so a 20,000-game batch
lands in roughly **3 hours wall-clock**, every shard well inside both the 340-minute job timeout and
the 6-hour cap. Ten shards × 2000 games would be ~324 min per shard — under the cap but past the
current timeout, so **do not shard fewer than ~16 ways** without raising it.

#### Design decisions M4 settled

- **Slice the book with fastchess's own `start=` index, not by splitting files.** `-openings` accepts
  `start=(1|N)`, so with `order=sequential` shard *i* (0-based) playing *R* rounds uses
  `start=<i*R + 1>`. Disjointness becomes arithmetic instead of file surgery. A 20,000-game batch
  consumes 10,000 openings — **4% of the book** — so supply is a non-issue and M3's worry is closed.
- **Build once, share the binaries as artifacts.** Currently each job builds both sides from source.
  Across 20 shards that is ~1 hour of duplicated runner time, but the real argument is correctness:
  every shard must play *the same two binaries*, and a build job feeding artifacts guarantees it
  rather than assuming 20 independent builds agree.
- **Keep per-shard concurrency at 3.** Each matrix job is its own runner, so more shards do not
  contend with each other; raising the per-job figure is what would. It is validated at zero time
  losses over 1200 games. The original reason to treat 3 as fixed was that #204 left 10+0.1 no
  margin; that is now fixed (PR #212), but nothing has been *measured* above 3, so 3 stays fixed on
  the plainer grounds that it is the only figure with evidence behind it.
- **Artifact names must be unique per shard.** `actions/upload-artifact@v4+` errors on a duplicate
  name within a run, so the current `strength-${{ github.run_id }}` needs the shard index appended.

#### The two things that can silently produce a wrong number

1. **The error bar must be computed pentanomially, not per-game.** fastchess reports `Ptnml(0-2)`
   because color-swapped pairs are correlated; pooling raw W/L/D and applying an independent-games
   formula yields an interval that is simply wrong, in the direction of looking more precise. The
   aggregator must pool at **pair** level, or reuse fastchess's own maths rather than reimplementing
   it. Decide which before writing the aggregator — this is the single highest-risk piece of M5.

   **Decided (2026-08-06): pool the `Ptnml(0-2)` counts fastchess already prints.** Reusing its maths
   directly is not actually available — fastchess computes from games it plays, and has no mode that
   re-derives statistics from a PGN — so the choice was between parsing its printed counts and
   re-deriving them from 20,000 games of PGN. Parsing the counts wins: the number being pooled is the
   number fastchess itself reported, which removes a whole class of parsing disagreement, and pooling
   is then elementwise addition of five integers per shard.

   The reimplementation risk this was flagged for is retired by `--self-test` in
   `.github/scripts/pool_pentanomial.py`, which checks the formula against six real
   (counts, Elo, interval) triples from this project's own matches, spanning 6 to 3500 games. It
   reproduces fastchess's output to both decimal places on every one, including the tight
   `+40.28 ± 9.81`. Run it in CI or locally: `python3 .github/scripts/pool_pentanomial.py --self-test`.
2. **A partially failed batch must not be reported.** Each shard keeps the existing gate (any time
   loss, illegal move, disconnect or stall fails that job). The aggregator must then **refuse to pool
   unless every shard succeeded** — otherwise a run where three shards died reports the surviving
   17 as though nothing happened, which is a biased subset wearing a full batch's error bar.

#### Steps

- Split `strength.yml` into `build` → `match` (matrix) → `aggregate`, with the build job uploading
  both binaries.
- Matrix of N shards, each with its own `start=` slice and its own artifact name.
- Aggregation job: download every PGN, pool at pair level, compute Elo and the interval once, write
  to the job summary, and fail if any shard did not succeed.
- **Prove the mechanics with a 2-shard, 20-game dispatch before any 3-hour run.** M4's two defects
  — the fastchess subdirectory and the untracked-dirt refusal — were both invisible on reading and
  obvious on the first execution. A cheap matrix run exercises slicing, artifact naming and
  aggregation without spending an afternoon to find a typo.

**Exit:** a sharded null test whose pooled result agrees with **-3.47 ± 18.21**, and whose shards can
be shown to share no opening (compare the first FEN of each shard's PGNs; any duplicate means the
slices overlap).

#### Status (2026-08-06) — **M5 COMPLETE**

**Exit criterion met.** Run `31054348465` — 20 shards x 1000 games, `reference_ref=HEAD`, 2 h 47 min
wall-clock, all 23 jobs green:

```
Shard opening positions: 20 total, 20 unique
Pooled: -2.17 +/- 4.18 Elo  (10000 pairs = 20000 games, score 49.69%)
Pooled Ptnml(0-2): [1495, 1665, 3754, 1642, 1444]
```

All three conditions hold:

| Condition | Result |
|---|---|
| Agrees with the single-job instrument (`-3.47 ± 18.21`) | **Yes** — `-2.17` sits well inside it, so sharding introduced no bias |
| Consistent with the true value, which is exactly 0 | **Yes** — `[-6.35, +2.01]` contains it |
| Reaches the predicted resolution | **Yes** — ±4.18 against a forecast of ≈±4 |

Recorded in `Docs/EloLog.md`'s Linux ledger. **The instrument is now calibrated at scale**: ±4 Elo,
against ±18 single-job and ±26 locally, which is the first interval this project has had that can
resolve one of epic #110's single-digit terms.

Mechanics were proven first by a cheap 2-shard, 20-game dispatch (run `31053457040`), whose
`+70.44 ± 119.43` was mechanism rather than measurement. The partial-batch refusal was exercised
separately by running the aggregator against a single shard log.

The cheap dispatch earned its keep exactly as predicted. Two defects, both invisible on reading:

1. **`.gitignore` blanket-ignores `/.github/*`** behind an allowlist, so `git add -A` silently
   skipped the new aggregator script. The existing workflow files only survive because tracked files
   are exempt from `.gitignore`. Fixed by extending the allowlist; the whole first dispatch died at
   the aggregate step on a file that was never committed.
2. **The formula check ran too late to be useful.** It now runs in `setup`, before any build — a
   missing or broken aggregator used to surface only after the entire batch, which on the real run
   would be three hours to learn the script is absent.

Nothing remains for M5. The next milestone is M6, whose trigger design turns on how many strength
runs can be in flight at once — see M6.

**Value delivered:** 20,000 games in ~3 hours unattended; ≈ ±4 Elo, against ±18 from the current
single-job instrument and ±26 locally. That is the first point at which epic #110's single-digit eval
terms are decidable at all.

### M6 — Wire to PRs and to the epic (~1-2 sessions)

**Read this before designing the triggers.** M5 and #217 replaced most of this milestone's original
assumptions with measurements. The concurrency blocker it was written around no longer exists; a
different one takes its place.

#### Measured inputs

| Input | Value | Source |
|---|---|---|
| Shards, default | **18** | #217 Experiment A |
| Wall-clock, ~20,000 games | **3 h 04 min** (18 × 1110) | #217 Experiment A |
| Resolution at that size | **±4.15 Elo** | Same |
| Runner-hours per full batch | ~55 (18 × ~3 h) plus ~10 min of build | Same |
| Concurrent jobs consumed | **18 of 20 — two slots stay free** | Same |
| Cost to a PR landing alongside | **10 min against a 4.5-5 min baseline** | Measured directly on PR #222 |
| Per-shard concurrency | **fixed at 3** | #217 Experiment B, declined |
| Aggregation cost | seconds | The `aggregate` job |

Actions minutes are free — the repository is public. Do not reason about M6's cost in spend.

#### The original blocker is gone

M6 was written around this: a 20-shard dispatch consumed the entire allowance, `build-and-test` is a
required check, so an automatic strength run would stall merges repo-wide. That was called a hard
blocker for any automatic trigger.

**It was fixed by capping the matrix, which is one of the three escapes that paragraph itself
offered.** At 18 shards a concurrent PR's first job starts within seconds and its run takes about ten
minutes instead of five. A delay, not a stall. Nothing in M6 is blocked on this any more.

#### The blocker that replaces it: two runs at once

`strength.yml` sets `concurrency: group: strength-${{ github.ref }}`. **Different PRs are different
refs**, so two labelled PRs do not queue against each other — both dispatch, and together they want
36 shards against a ceiling of 20. The second run's shards trickle in as the first's finish,
stretching both across many hours, and normal CI is starved for the duration.

The existing group only stops a PR colliding with itself. It does nothing about the case an
automatic trigger makes likely, because when it was written one run was assumed to be the maximum.

**Any trigger design needs a repo-wide serialisation group** — a constant such as `strength-lab`
rather than one keyed on `github.ref`. Decide this before the trigger, not after.

#### The highest-risk piece: making a decidable run affordable

20,000 games is the *resolution* figure, not the *decision* figure. `Run-EloMatch.ps1` solves this
locally with `-Sprt`, which stops as soon as the result is decisive. The CI lab has no SPRT, so it
always plays the full batch even when the answer was obvious at 15%.

SPRT is genuinely awkward under sharding: it is sequential, and 18 independent shards cannot stop one
another. Eighteen shards each running their own SPRT would be eighteen independent tests, which is
simply wrong. Two shapes were considered; **the measured resolution now decides between them.**

Intervals scale as `1/sqrt(n)`, so from the measured ±4.15 at 19,980 games:

| Games | Interval |
|---|---|
| 4,000 | ~±9.3 |
| 5,000 | ~±8.3 |
| 10,000 | ~±5.9 |
| 19,980 | **±4.15** |

- **Fixed smaller batch — rejected.** Epic #110's terms are single-digit; a ±8 interval cannot
  resolve a 5-Elo term. A smaller batch does not serve the payload this plan exists for.
- **Group-sequential — the recommendation.** Run a modest batch, pool, compute the LLR, re-dispatch
  only if undecided. Statistically sound provided the repeated looks are accounted for (alpha
  spending); the aggregator already emits exactly the pooled pentanomial counts an LLR needs.

Group-sequential also helps the concurrency story rather than straining it. A 4,000-game first look
is 18 shards × 222 pairs ≈ **37 minutes**, so it replaces one three-hour occupation with a series of
short ones with free gaps between — which serves "usable during working hours" better than the shard
count alone. Decide the alpha-spending scheme before writing anything, exactly as the pentanomial
question was decided before the aggregator. This is M6's equivalent of that risk.

#### Design decisions already settled

- **Reference must be the merge-base, not a fixed tag.** For a PR the reference has to be
  `git merge-base HEAD origin/main`, or the run measures everything that landed since the tag rather
  than the PR's own change. This is the one M6 item that prevents a *wrong answer* rather than adding
  convenience: it fails silently and produces a plausible number. The lab builds any ref from source,
  so this is an input-computation change, not a workflow-shape one.
- **Do not gate. Report.** The aggregator refuses to pool when any shard fails, by design — so a
  single flaky shard becomes a red X for a reason unrelated to the diff. Combined with the error bar
  and with PRs that legitimately lose Elo, gating would produce false failures more often than true
  ones. Post the verdict as a comment; leave `build-and-test-result` as the only required check.
- **Fork PRs get the job summary only.** Invariant 6 forbids `pull_request_target`, and a plain
  `pull_request` from a fork has a read-only `GITHUB_TOKEN`, so comment posting will fail. Restrict
  the trigger to non-fork PRs and accept that a fork contributor reads the summary.
- **Put cheap guards before expensive work.** M5's aggregator self-test moved into `setup` after a
  three-hour batch was nearly spent proving a script was missing. Any M6 script — comment posting,
  merge-base resolution, LLR computation — belongs in that same first job.
- **`.gitignore` blanket-ignores `/.github/*`** behind an allowlist. Any new file under
  `.github/scripts/` must be added to it or `git add -A` will silently skip it; `.py` is already
  allowlisted.
- **Documentation lands in `Docs/CI.md`**, which now carries what each workflow runs.
- **No Linux-specific reference tag.** The lab builds the reference from any ref and the ledger
  records the SHA, so an `elo-reference-v2-linux` tag would pin nothing a SHA does not.

#### Steps, in dependency order

1. **Merge-base `reference_ref`.** Load-bearing and independent of every other decision — worth doing
   whether or not the rest of M6 is ever built.
2. **Decide the alpha-spending scheme** for group-sequential. Policy, not code.
3. **Repo-wide serialisation group** on `strength.yml`.
4. **Trigger on a `measure` label**, restricted to non-fork PRs.
5. **Post the pooled verdict as a PR comment.**
6. Then start working #110 sub-issues against it. This is the point where the whole plan pays.

#### What this milestone deliberately does not do

**No automatic path-based trigger.** Firing ~55 runner-hours and three hours of wall-clock on every
PR touching `Eval.cpp` is heavy regardless of whether it blocks, and it is exactly what creates the
two-runs-at-once problem above. Runs stay deliberate — label-triggered — because they are expensive,
which is a different and more durable reason than the concurrency blocker that no longer applies.

**No gating on the result.** An Elo regression is not a build failure: a PR can legitimately lose Elo
(a refactor, a correctness fix) and the number carries an error bar that a pass/fail check discards.
Post the verdict, do not gate on it — unless someone deliberately decides otherwise, which is a
policy choice this plan does not make for them.

### Follow-ons, cheap once the above exists

- #189 (MSan) and #184 (TSan) as nightly jobs — mutually exclusive families, one slot each.
- #175 clang-tidy / clang-format on every PR.
- #156 coverage measurement; Codecov is free for public repositories.
- #180 epic-scale before/after, now affordable against both anchors.

## Files changed

| File | Milestone | Change |
|---|---|---|
| `.github/workflows/build-and-test.yml` | M1 | Drop label gate; fix two inaccurate comments; add `concurrency:`; build `main` on Windows Release |
| `.github/workflows/nightly.yml` | M2 | New — perft matrix, extended tier, tactical stability |
| `.github/workflows/strength.yml` | M4, M5, M6 | New — single job, then matrix + aggregation, then triggers |
| `Tests/openings/` | M3 | Large book added |
| `StratChessEvolved/Scripts/Run-EloMatch.ps1` | M3 | Book becomes a parameter |
| `Docs/EloLog.md` | M3, M4, M6 | Book-change note; CI calibration runs; separate Linux ledger |
| `Docs/Workflow.md` | M1, M2, M6 | CI section rewritten for the new shape |
| `CLAUDE.md` | M1, M6 | CI paragraph: label gate gone, gate not advisory, strength lab exists |

## Validation plan

- M1: a PR that touches `StratEngine/**` shows Windows Release and Debug running without a label;
  `build-and-test-result` appears as a required check and a red run blocks merge.
- M2: the nightly run's perft node counts match the known values exactly; a deliberately broken
  move-generation commit on a scratch branch makes it fail.
- M3: a local 501-game run no longer repeats openings.
- M4: **done.** Null test -3.47 ± 18.21 over 1000 games; control +75.88 ± 42.56 with the reference on
  half the base time. Both recorded in `Docs/EloLog.md` and above. Note the control came out
  *positive*, not "strongly negative" as this line originally predicted — the handicap is applied to
  the reference, so the candidate wins. Sign convention matters when reading it.
- M5: pooled result of a sharded null test agrees with **-3.47 ± 18.21**; verify shard slices are
  disjoint by checking that no two jobs' PGNs share an opening FEN.
- M6: one #110 sub-issue measured end-to-end.

## Invariants after this work

1. `Run-Bench.ps1` runs locally only. Nothing in CI reports nps.
2. Candidate and reference are always built in the same job, from the same toolchain, in the same
   configuration. No CI Elo number is ever compared with a locally measured one.
3. `Docs/EloLog.md` keeps the two ledgers visually separate, and every row states its book.
4. Any batch with a reported time loss is discarded, not reported.
5. Opening-book slices across shards are disjoint.
6. No workflow uses `pull_request_target`; no secret is reachable from a fork PR.
7. Local `Validate-PrePR.ps1` remains the pre-PR gate; CI is now also a gate, not a replacement.
