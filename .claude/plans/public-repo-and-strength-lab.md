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

## Where this stands — read this first (2026-08-05)

**M0 through M4 are complete. M5 is next**, and its section carries measured inputs rather than
estimates.

| | State |
|---|---|
| Repository | Public; CI is a gate, `build-and-test-result` required on `main` |
| Nightly | `nightly.yml` — deep perft, `[slow]` tier, sanitizers + `_GLIBCXX_DEBUG`, tactical ×100 |
| Opening book | Solved. `EngineTesting\openings-large.*` locally (optional), `UHO_4060_v3.epd` in CI — 242,201 openings |
| Strength lab | `strength.yml`, `workflow_dispatch` only, **calibrated 2026-08-05** |
| Resolution today | **±18 Elo** at 1000 games (CI), ±25-26 at 500 (local) |
| Not yet possible | Epic #110's single-digit terms. That is exactly what M5 buys |

Where things live: method in `Docs/EloMeasurement.md`, results in `Docs/EloLog.md` (two ledgers,
never compared), workflow in `.github/workflows/strength.yml`.

Issues this work spawned, both open and neither blocking M5:

- **#204** — `compute_budget()`'s 100 ms per-move floor is absolute, so the engine forfeits at any
  increment below it, and the standard 10+0.1 sits exactly on that floor. Shaped M4's control run and
  constrains any future time-control choice.
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
  losses, and #204 leaves 10+0.1 no margin — treat 3 as fixed.
- **Artifact names must be unique per shard.** `actions/upload-artifact@v4+` errors on a duplicate
  name within a run, so the current `strength-${{ github.run_id }}` needs the shard index appended.

#### The two things that can silently produce a wrong number

1. **The error bar must be computed pentanomially, not per-game.** fastchess reports `Ptnml(0-2)`
   because color-swapped pairs are correlated; pooling raw W/L/D and applying an independent-games
   formula yields an interval that is simply wrong, in the direction of looking more precise. The
   aggregator must pool at **pair** level, or reuse fastchess's own maths rather than reimplementing
   it. Decide which before writing the aggregator — this is the single highest-risk piece of M5.
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

**Value delivered:** 20,000 games in ~3 hours unattended; ≈ ±4 Elo, against ±18 from the current
single-job instrument and ±26 locally. That is the first point at which epic #110's single-digit eval
terms are decidable at all.

### M6 — Wire to PRs and to the epic (~1 session)

- Trigger `strength.yml` automatically on Engine-tier PRs touching `Eval.cpp` or the search files,
  or on a `measure` label — restricted to non-fork PRs.
- Post the pooled verdict as a PR comment.
- ~~Add a separate table in `Docs/EloLog.md` for the Linux ledger~~ — **done** in M4. The ledger
  exists, carries the two calibration rows, and states the never-compare rule. The
  `elo-reference-v2-linux` tag it also proposed is **not needed**: the lab builds the reference from
  any ref and the ledger records the SHA, so a Linux-specific tag would pin nothing a SHA does not.
- Then start working #110 sub-issues against it. This is the point where the whole plan pays.

**Before triggering automatically, decide what a red strength run means.** An Elo regression is not
a build failure: a PR can legitimately lose Elo (a refactor, a correctness fix) and the number
carries an error bar that a pass/fail check discards. Post the verdict, do not gate on it — unless
someone deliberately decides otherwise, which is a policy choice this plan does not make for them.

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
