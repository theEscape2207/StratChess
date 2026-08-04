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
   d0da227. A debugger dump of ~2010-era code — must be inspected before the repository is public.

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

### M5 — Shard it (~1-2 sessions)

- Matrix of N jobs (start at 8, raise to 20 once the account concurrency behaviour is understood),
  each playing `Games/N` games with a **disjoint slice of the opening book** — overlapping slices
  replay identical games and inflate confidence.
- Aggregation job: download all PGN artifacts, pool them, compute Elo, error bar and LLR once, and
  write a summary to the job summary.
- Watch the 6-hour per-job cap and artifact retention settings.

**Value delivered:** 20,000 games in 2-3 hours unattended; ≈ ±4 Elo where the local instrument
gives ±26.

### M6 — Wire to PRs and to the epic (~1 session)

- Trigger `strength.yml` automatically on Engine-tier PRs touching `Eval.cpp` or the search files,
  or on a `measure` label — restricted to non-fork PRs.
- Post the pooled verdict as a PR comment.
- Add `elo-reference-v2-linux` as a tag, and a **separate table** in `Docs/EloLog.md` for the Linux
  ledger, with an explicit note that its rows must never be compared against the clang-cl rows.
- Then start working #110 sub-issues against it. This is the point where the whole plan pays.

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
- M4: null test within its error bar; time-handicap control strongly negative. Both recorded in
  `Docs/EloLog.md` **and** in this file.
- M5: pooled result of a sharded null test agrees with the single-job null test; verify shard slices
  are disjoint by checking that no two jobs' PGNs share an opening FEN.
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
