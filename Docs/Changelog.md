# Changelog

All notable changes to StratChessEvolved are documented here. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), adapted for a project without
external releases: sections are dated rather than versioned, and entries keep the level
of implementation detail (files touched, validation method, "why") that's useful for a
solo/AI-assisted dev workflow, rather than the terse end-user-facing style the original
convention assumes. `Deprecated`/`Security` are dropped — they essentially never apply to
engine internals.

This is the permanent historical record migrated from `Docs/Roadmap.md`'s old
`## ✅ Completed Work` section (see PR #105); `Roadmap.md` itself now holds only active
principles, pointing to GitHub Issues for the live backlog.

Dates are PR merge dates, verified against `gh pr list`/`gh pr view` — the original
Roadmap.md's dates and PR citations had several errors (wrong PR numbers, entries filed
under the wrong month) that surfaced during this migration; see the entries themselves
for what was corrected. A handful of entries have no PR reference in the original text
and couldn't be matched with confidence — those remain in the undated pocket below.

Newest first.

---

## 2026-08-11 — Configurable UCI transposition-table memory (#254)

### Added

- **`Hash` UCI spin option** — advertised as `default 192 min 1 max 1536` MiB. Applying it while
  idle constructs a replacement transposition table, then swaps it into the persistent AI only
  after allocation succeeds; allocation failure keeps the prior table live and reports the failure
  through `info string`.
- Successful changes report the actual rounded-down entry allocation and bucket count. Hash budgets
  cover entry bytes; the platform-dependent lock array remains additional memory.

### Notes

The advertised 192 MiB default is behaviorally unchanged: the former 256 MiB constructor request
already rounded down to 2,097,152 buckets / 192 MiB. Against `origin/main`, `Run-Bench.ps1 -Depth 12
-Threads 1` produced identical best moves and node counts on all eight positions (34,478,850 total
nodes in each build).

`ucinewgame` clears but does not replace the configured table. With a depth-8 search before every
timed clear, its median setup latency was **22.520 ms at 192 MiB** and **175.707 ms at 1536 MiB**
(five samples after one warm-up). This occurs before `go`, so it is client-visible setup latency,
not search-clock time.

---

## 2026-08-11 — Tooling: reusable UCI latency probe, and a log of received commands (#269)

### Added

- **`Scripts/Measure-UciLatency.ps1`** — round-trip cost of one UCI command against a control case,
  fresh process per case. Three sessions had written a throwaway version of this (#213, #263, #266);
  the two traps that made #213's numbers wrong are now built in and documented in the script's help:
  drive past move 1, and use `-CompletionMarker bestmove` for `go`, which is asynchronous and whose
  `readyok` therefore comes back mid-search.
- **`uci --log-commands[=path]`** — one line per received command, flushed per line, written to a
  file-only logger. Off unless asked for. It answers "did the GUI actually send X?" without the
  temporary source instrumentation #263 needed.

### Notes

The command log must never use spdlog's default logger: in UCI mode nothing calls `InitDefault()`,
so the default is still spdlog's built-in **stdout** console sink, silent only because `main()` sets
the global level to `off`. Stdout is the protocol channel. The logger is also unregistered and owned
by the `UciHandler`, unlike `EnsurePerfLogger`'s registry-plus-`once_flag` shape, which would pin the
first filename for the process lifetime and hold its handle open.

Measured with the new script on the shipping build: control (`isready` round trip) **0.09 ms**
median, `ucinewgame` **0.07 ms** — indistinguishable, which is PR #274's rebuild removal showing up
where #266 had measured 36.83 ms. Instrument calibration against a known duration: `go movetime 200`
read **201.37 ms** median (min 201.12, max 201.66).

Equivalence against `origin/main` @ `2253483`, `Run-Bench.ps1 -Depth 12 -Threads 1`: identical node
counts and best moves on all eight positions (34,478,850 nodes both sides). The logging call sits in
the command loop, off the search path.

---

## 2026-08-09 — Fix: the engine could play an illegal move after `bestmove` (#245)

### Fixed

- **`searching_` is now cleared BEFORE `bestmove` is sent, not after** (`UCIHandler.cpp`). The old
  order left a window in which the engine had already published `bestmove` but still reported itself
  as searching. `refuse_while_searching()` silently refuses `position` in that state — while `go` is
  not refused — so a client replying at full speed had its `position` dropped, and the next search
  ran on the **previous** position. The engine then returned a move that was legal there and illegal
  on the real board: typically its own previous move, from a square it had already vacated.

### Notes

Found by the strength lab, not by the test suite: two games out of ~19,980 in run `31281221815` were
forfeited this way (`0-1 {White makes an illegal move}`), which failed two shards and voided that
measurement. Every earlier production run had passed clean, because the window is nanoseconds wide —
about 1 command cycle in 10^6.

**Not caused by the evaluation change that surfaced it.** Evaluation selects among legal moves; it
cannot make the engine lose track of its own pieces. One of the two failures is in a pawnless
K+B+N vs K ending, where the new pawn terms return zero for both sides.

Confirmed experimentally rather than by inspection. Widening the window to 50 ms in a scratch build
turned the race from unobservable into deterministic — **39 of 40** iterations refused, 38 of them
replaying the previous move. With the corrected order and the same 50 ms probe still in place:
**0 of 40**. `Scripts/uci_race_probe.py` is the driver, and its docstring carries the recipe.

A unit test cannot capture this: on a correct build the window does not exist, so nothing can observe
it without reintroducing the defect. The probe plus the recorded before/after is the regression
artifact.

---

## 2026-08-09 — Retune the passer bonus after a measured regression (#116)

### Changed

- **`PASSED_PAWN_RANK_SCALE` halved** — roughly 10/22 cp on the pawn's starting rank up to 40/90 at
  the 7th, where the first version gave 20/45 up to 80/180. Shape untouched, so the two measurements
  differ in magnitude alone.
- **Blockade discount**: a passer whose stop square is occupied by any enemy piece scores half
  (`PASSED_PAWN_BLOCKADED_SCALE`). The first version paid a 7th-rank passer its full value with the
  enemy king parked in front of it.

### Notes

The first version measured **-11.52 +/- 4.36 Elo** over 19,980 games (run `31300861562`) — decisively
negative, with the 95% interval entirely below zero. The detection was not at fault: the masks were
verified exhaustively and the unit tests pin every property. The valuation was, in the two places the
eval review had flagged in advance.

New test: the passer bonus is asserted **monotonic across every rank**, walking a lone pawn from its
starting square to the 7th. The bonus is now scaled twice and each scaling truncates, so monotonicity
has to be checked rank by rank rather than argued in general.

Bench against the merge base is **not usable as a like-for-like comparison here**, and the aggregate
figure is actively misleading: the two builds search different trees, and the candidate spends far
more of its nodes on `open-mid`, the slowest-nps position in the suite (8.6M nodes -> 13.8M), which
drags the weighted aggregate down well past any per-node cost. Per position the cost is roughly
1.5-7.6%, with one position slightly faster. Strength, which subsumes both the evaluation change and
its speed cost, is the only instrument that settles this.

---

## 2026-08-09 — Passed-pawn bonus and backwards-pawn penalty (#116)

### Added

- **Passed-pawn bonus** in `eval_pawns`: no enemy pawn on the pawn's own or either adjacent file
  ahead of it, and no friendly pawn of its own directly ahead — the rear pawn of a doubled pair can
  never advance past its partner, so only the front pawn is passed. Scaled by how far the pawn has
  advanced (`PASSED_PAWN_RANK_SCALE`, 1/16ths) and tapered, giving roughly 20/45 cp on the starting
  rank up to 80/180 cp on the 7th.
- **Backwards-pawn penalty**, requiring **both** clauses: every friendly pawn on an adjacent file is
  strictly ahead, *and* the stop square is attacked by an enemy pawn and defended by none. The
  definition is written on the constant, because "backwards pawn" has several incompatible ones in
  the literature and an unstated one cannot be tuned.
- **`g_bbPassedMaskWhite` / `g_bbPassedMaskBlack`** in `defines.h` — compile-time three-file forward
  spans, built with explicit file bounds rather than a shifted file mask, which is what keeps an
  a-file pawn's span off the h-file.

### Notes

This retires the two constants issue #116 exists for: `PASSED_PAWN_BONUS` and
`BACKWARDS_PAWN_PENALTY` were defined and referenced nowhere, under a standing
`// TODO: Add bonus for passed pawn - bonus should be dependant on game stage`. The phase dependence
that TODO asked for is what `PASSED_PAWN_BONUS_EG` provides.

The masks also give the backwards term its clause (a) for free: intersecting the adjacent files with
the complement of the forward span leaves exactly the adjacent-file squares level with or behind the
pawn, so no second mask table was needed.

Values are untuned and literature-shaped, matching how mobility landed — #117 owns tuning. Rank and
phase shape are in a separate table from the magnitude so #117 can move them independently.

`eval-reviewer` verified the masks exhaustively rather than by inspection — all 64 squares of both
arrays against an independent reimplementation, plus the clause-(a) set identity and the White/Black
mirror relation — and found no defect in them. Three of its findings were fixed here: the
doubled-pair exclusion above, a stale blend comment that still claimed `eval_pawns` was phase-neutral
(it tapers now), and a backwards-pawn test that did not test what it claimed. That test moved the
black pawn to the h-file, which also took it out of the white pawn's span and handed it a passer
bonus, so the assertion passed on the passer swing and would have held with the penalty at zero. The
control now moves the pawn one rank instead, and both backwards tests were confirmed to fail with
`BACKWARDS_PAWN_PENALTY` temporarily set to 0.

Validation: `[eval]` 72 cases, full fast tier in Release **and** Debug (5,993 assertions, 292 cases),
including a new colour-symmetry FEN with passers on opposite edge files at different advancement.
Bench against the merge base shows no slowdown — 3.385M nps against 3.315M, spreads of 0.1% and 0.4%
within each build.

---

## 2026-08-08 — ThreadSanitizer for the Lazy SMP helpers (#184)

### Added

- **`tsan-linux`** in `build-and-test.yml`: builds `StratChessEvolved` with `-fsanitize=thread` and
  drives six multi-threaded scenarios over UCI at `Threads=4`, `8` and `16`, including a time-managed
  `movetime` abort. Wired into `build-and-test-result`, so a race blocks the merge and a skipped tier
  still reports success.
- **`.github/scripts/tsan_smp_drive.py`** — the driver, committed rather than inlined in YAML so a CI
  failure reproduces locally. It waits for each command's completion token and treats an early exit
  as failure, which is what a race looks like under `-fno-sanitize-recover`.
- `.claude/plans/tsan-lazy-smp.md` — survey, positive control, cost measurements and the CI
  contention analysis.

### Notes

**No suppression file, and that is the result.** #184 expected the shared TT to be racy-by-tolerance
and budgeted for suppressions. `TranspositionTable` is not lock-free — a `std::shared_mutex` per
bucket, atomics for the counters — so there is no torn read to tolerate. Six configurations plus the
fast tier reported zero races, with helpers demonstrably running (144,260 nodes at `Threads=1` against
421,608 at `Threads=4` on the same depth-8 search).

The zero is backed by a positive control: a deliberate unsynchronised increment injected into
`helper_loop` produced two reports with correct stacks. Two mechanics silently fake a clean run and
are handled — ASLR (TSan dies before `main` on Ubuntu 24.04 without `setarch -R`) and piping UCI
commands ahead of the searches they configure.

The job builds only the engine and skips the Catch2 tier: the tier is single-threaded, so under TSan
it adds nothing, and including it would cost a second instrumented target plus 65 s and push the job
past the current critical path.

A `stop`-mid-search scenario was written, measured and removed: a TSan-instrumented engine never
answers `stop` with a `bestmove` — at 1, 4 and 8 threads, on 4 and 24 cores — while a clean build of
the same commit answers in 0.00 s. Filed as #243; it puts the abort-on-request path out of the job's
reach, which is why `movetime` is covered instead.

Validation: `Build` tier. The driver was exercised against the instrumented build pinned to four
cores, the runner's count — 6/6 scenarios clean in 48.2 s — and separately observed failing correctly
when the binary still carried the injected race. The job's first green run took **197 s** against the
260 s critical path, so PR wall clock is unchanged.

---

## 2026-08-08 — `Run-PerftCheck.ps1`, and the corpus sweep recorded (#196)

### Added

- **`Scripts/Run-PerftCheck.ps1`** wraps perftcheck: resolves `perftcheck.exe` from `EngineTesting\`
  and the engine through `Get-BuildArtifact.ps1`, runs the corpus, then **classifies the failures**.
  A clean sweep is not zero failures — the corpus holds positions the FEN parser rejects, and the
  engine answers for the start position instead. Those carry a fingerprint (an actual node count
  equal to startpos perft at that depth); the script buckets them and fails only on what is left.
  `-Limit` bounds a sanity run, `-ClassifyReport` re-reads a past report without running anything.
- `Docs/TestDesign.md` gains the sweep as a tier, a coverage-map row and a section carrying the
  2026-08-05 result. `Docs/Workflow.md` records the standing decision that it stays local.
- `Get-ChangeTier.ps1` classifies the new script as `Tooling`, with a self-test case. Its enumeration
  is deliberately explicit, so an unregistered script falls to `Engine` — which is what this one did
  on its own branch until it was registered.

### Notes

Closes #196. Steps 1, 2 and 4 of it were already discharged (the UCI `go perft` command in #197, the
exhaustive run in #198, the Apache-2.0 licence); what remained was the CI question, and the sweep's
own result answers it — zero disagreements on legally reachable input means a CI leg would be a
regression tripwire, a role `perft test` and the nightly perft legs already fill.

The result was reachable only through issue comments before this. It is now a documented statement:
on every legally reachable position in the corpus at depths 1–4, `MoveGenerator` matches the
Stockfish/TGCT oracle exactly.

Validation ran at `Engine` tier — full build, extended tests, tactical suite and self-play, all
passing — because the new script was still unregistered when the branch was validated. The classifier
was checked against the archived 73-failure report from #198 (73 rejected, 0 unexplained, reproducing
that run's reading) and against a mutated copy of it with one fingerprint broken (1 unexplained,
exit 1). A live 3,000-case run exercised the run path.

---

## 2026-08-08 — `Run-EloMatch.ps1` refuses an SPRT against the fixed anchor (#159)

### Changed

- **`-Sprt` now exits 1 when the reference comes from the tag lookup.** A tag-resolved reference is a
  fixed anchor, so the hypothesis under test is about `main` plus the change versus that anchor — a
  statement about the sum, satisfiable by the margin `main` had already accumulated. The combination
  is refused rather than warned about, because no per-change reading of such a verdict exists and
  none can be recovered by differencing anchor rows.
- **`-AnchorSprt`** overrides the refusal for a cumulative reading wanted on purpose, and labels the
  `EloLog.md` row as one. The 2026-07-29 `candidate-08d4ef8` row needed that caveat written by hand
  after the fact; it is now emitted with the row.
- `Docs/EloMeasurement.md`'s command examples pair every `-Sprt` form with `-ReferenceExe`.

### Notes

Closes the last open item of #159 — the methodology decision it asked for was settled in
`EloMeasurement.md`, and `strength.yml` already defaults `reference_ref` to the merge base. The local
script was the one path that still produced a verdict about the wrong quantity by default.

Validation: `Tooling` tier, plus the four argument paths exercised directly — default anchor refused,
`-AnchorSprt` and `-ReferenceExe` admitted, and the non-SPRT default path unchanged.

---

## 2026-08-07 — Mobility evaluation for knight, bishop, rook and queen (#98, #113)

### Added

- **`eval_mobility`** — counts the squares each piece can move to, weighted per piece type and
  phase-split, and adds it to the tapered sum. Closes #113 as well: the queen is scored from the
  start, which is the risk that issue existed to prevent.
- **`EvalContext::pawn_attacks`**, built in `BuildContext` with the same file-masked shifts
  `MoveGenerator::GeneratePawnCaptures` uses, so the two cannot disagree about what an edge-file
  pawn covers. #116 will want them too.
- A `mobility` row in `EvalBreakdown`, the UCI `eval` table, and the printed net sum.

### Notes

**Design, all recorded at the term**: pseudo-legal rather than legal (legality filtering would need
move generation per piece per node); safe mobility, so squares an enemy pawn covers do not count;
enemy-occupied squares *do* count, since a piece that can capture is active — one mask rather than
two, applied identically to every piece type. The king is excluded deliberately: king mobility is a
king-safety signal and belongs to #97.

**Weights are conservative and not hand-tuned.** #117 owns them. Mobility overlaps the PSTs, which
already reward central placement, so a smaller gain than the literature suggests is the expected
outcome rather than a defect.

**Measured at +38.34 ± 4.26 Elo** over 19,980 games against the merge base (run `31191858114`,
95% interval [+34.08, +42.60], score 55.50%). Decisive, not suggestive — the interval clears zero by
eight standard errors. Recorded in `Docs/EloLog.md`'s per-change table, which this is the first row
of.

**That figure is the NET of two opposing effects.** The term costs **-6.2% nps** (`Run-Bench.ps1`,
depth 12, clang-cl, two runs per binary; per-position -8.1% to +3.2%) — roughly 10 Elo of search
speed at the project's ~1.7 Elo per 1% conversion. So the evaluation improvement is worth about +48
gross, and the term repays its own cost several times over. It is also the pre-tuning figure: #117
owns the weights.

**Node counts and time-to-depth are not evidence here** and are deliberately not quoted. The best
move changes at the root on several bench positions, so the two builds search different trees; nps is
the only clean per-node measure. The same trap is on record from #111/#114/#115's node probe.

**`WAC-287` removed from the tactical suite.** It began failing at its committed depth 6, but the
cause is not this term: with the mobility weights zeroed — behaviourally `main` — it already fails at
depth 8. A depth sweep showed it oscillating PASS/FAIL/PASS/FAIL across depths 5-8 both with and
without mobility, so it never met the depth-stability criterion #235 introduced. Suite is 36/36.

The sweep also separated two things that look alike. Ten positions simply **need more depth** — they
fail shallow and pass at every greater depth, which is normal and fine. Three get **worse with more
search**: `WAC-043` is correct at depths 5, 6 and 7 and wrong at 8; `WAC-287` alternates strictly with
parity; `WAC-065` dips at 6 and recovers. A deeper search returning a worse move is a property of the
search, not of the position, and nothing in the repository currently tests for it — `tactical
stability` checks repeat runs at ONE depth. Filed as #237.

**Two tests were re-pointed from `Evaluate()` to the term they are about.** The #126 open-file tests
asserted whole-position equality to prove a claim about `eval_rooks`; moving a knight or a pawn
legitimately changes mobility, so those totals now differ while the rook term does not. Comparing
totals to prove a single term's behaviour was over-coupling that any new term would have broken.

**`UCITests`' breakdown row list was incomplete**, holding 5 of the 7 rows. Its "nets sum to the
printed total" invariant was passing only because `bishops` and `castling` were zero in the tested
positions. Now complete, and it caught a real omission immediately: the `eval` command's printed sum
had not been updated for the new row.

---

## 2026-08-07 — Tactical suite is green again: QFORK-001 out, seven depth-verified positions in (#235)

### Removed

- **`QFORK-001`** from `Tests/tactical_test_cases.json`, and its `[.]`-hidden Catch2 twin from
  `StratChessTests/TacticalTests.cpp`. Its best move was depth-dependent — `d1-b3` at depth 4-5,
  `d1-g4` at 6+, and `d1e2` in the suite as it stood — so it tested nothing stable. Issue #66's
  null-move-pruning zugzwang guard, which it was written for, is code-level in
  `should_try_null_move()` and unaffected.

### Added

- **Seven WAC positions**, each verified to give the same best move at every depth in a band, not at
  a single depth: WAC-010 and WAC-011 at depth 5 (stable 4-8), and WAC-003, 006, 007, 008 and 014 at
  depth 6 (stable 6-9). All are `tactical_win`, which also thins the suite's mate-heavy skew.
- **A depth-stability requirement** in `Docs/TestDesign.md`'s "Growing the suite" procedure — the
  criterion whose absence let QFORK-001 in on a single depth-4 check.

### Notes

**The suite now reports 37/37 (100%).** It had been reporting 30/31 for over a month while printing
`PASS`, because `TacticalTestRunner` has only an aggregate pass-rate threshold and 96.8% cleared it.
A permanently-red case in a green suite trains everyone to skip the `FAIL` lines, so a *second*
regression would have looked identical to the noise. Restoring "any FAIL is news" was the point of
the change; the seven new positions are the bonus.

**Three candidates were rejected by the same check that validated the rest** — one the engine never
solves within depth 8, and two whose answers flapped across depths (`d4b5`/`b2b4`/`d4f5` and
`g2g3`/`e1c1`/`c4e6`). They are exactly the class of position this issue existed to keep out, and
they were caught before being committed rather than a month afterwards.

---

## 2026-08-06 — Strength lab defaults to 18 shards, so a run stops blocking CI (#217 Experiment A)

### Changed

- **`shards` defaults to 18, not 20.** At 20 a dispatch consumed the entire 20-job concurrent
  allowance, so `build-and-test-result` — a required check — could not start for any other PR until
  the batch finished three hours later. That made a full run night-time-only. 18 leaves two slots
  free.
- `strength.yml`, `Docs/CI.md` and the plan's status table describe the new default; the runner
  topology is recorded by the `lscpu` step added the same day.

### Notes

**Measured, not assumed.** A second null test at 18 × 1110 returned **-1.51 ± 4.15** against the
20-shard **-2.17 ± 4.18**. The interval is the result: at ~10,000 pairs a spread estimate is itself
known to about ±0.03, so the 0.03 difference is one standard error and the split costs no
resolution. Wall-clock **3 h 04 min against 2 h 47 min — a 17-minute cost.** Zero time losses,
slices disjoint, point estimate containing the guaranteed zero. Recorded in the Linux ledger.

**The blocking claim was verified directly**, not inferred. An unrelated Build-tier PR opened while
the batch ran had its first job start 5 s later and completed in 10 min, against a 4.5-5 min
uncontended baseline — two of its five build legs running at a time while three queued. A delay,
not a block.

**This says nothing about per-shard concurrency.** Shard count changes how many runners are used,
not the CPU each engine gets, so the effective time control is unchanged and the row stays
comparable with everything already in the ledger. Raising concurrency would not be, which is why
#217's Experiment B is a separate question.

**`strategy.max-parallel` is the wrong lever** and was rejected: with 20 shards capped at 18, the
last two only start when the first two finish, roughly doubling wall-clock to save the same two
slots.

---

## 2026-08-06 — CI split out; standing decisions written down (#209 follow-up)

### Changed

- **`Docs/CI.md` is new**, carrying what each workflow runs. It was a third of `Workflow.md` and
  three workflows deep; #209 grouped it and noted the split as available once the seams were visible.
  Moved verbatim — no CI content was rewritten.
- **`Workflow.md` gains Part 2, "Standing decisions"**: direction that was previously implicit and
  could drift back by accident. `CLAUDE.md` now points at these rather than restating them.

### Added

- **What validates what.** Linux Debug + sanitizers is the primary correctness gate; Windows CI
  covers the *shipping toolchain* and does not become redundant when TSan and MSan land, because
  clang-cl silently drops flags Linux cannot observe. `/RTC1` is the live example: clang-cl accepts
  it, warns about nothing even under `-Wunused-command-line-argument`, and emits byte-identical
  objects, where MSVC produces 53% more object. No Windows Debug configuration belongs in a gate —
  ASan and MSan subsume what `/RTC1` would find.
- **Speed and nps.** The goal is measured positive Elo; speed serves it. Below ~5%, `Run-Bench.ps1`
  is the instrument — an Elo match cannot resolve an effect that small, since 1% nps ≈ 1.7 Elo
  against the strength lab's ±4. Anything adding per-node work gets a bench pass, evaluation terms
  included, and a measured slowdown needs a stated benefit that outweighs it.
- **Threat model.** Not network-facing, no privilege boundary, no attacker. External-input work aims
  at robustness rather than security, and exploit mitigations need a reason beyond sounding prudent.

### Notes

Recorded first use: **#218 (Control Flow Guard) was declined and closed** on the threat model,
deliberately without benchmarking — a 0% cost result would still not have answered the adoption
question, because cost was never the objection. `/GS`, ASLR, high-entropy VA and DEP are all on by
default, verified present, cost nothing to keep, and stay.

---

## 2026-08-06 — External input reports and exits cleanly (#178)

### Fixed

- **Malformed input no longer fail-fasts.** Every config failure printed a precise diagnostic and
  then died with `0xC0000409` (`STATUS_STACK_BUFFER_OVERRUN` — `std::terminate`), because
  `Game::Init` catches, reports and rethrows with nothing above it. `main` now catches, so the same
  message is followed by exit 1. `Init` keeps rethrowing deliberately: swallowing there would let a
  half-initialised `Game` continue into `Run()`.
- **`perft run <depth>` parsed its argument unguarded** and died with no output whatsoever — the only
  input path in the engine that failed completely silently.
- **`position` and `setoption` no longer mutate state a running search reads.** Both are refused with
  an `info string` while a search is in flight. Previously a `position` arriving mid-search could
  make the engine answer for a position the client never asked about: the documented reproduction
  returned `e2a6`, which is not legal from the start position it had actually searched. It now
  returns `g1f3`.
- Perft test-case loading rejected a non-numeric depth key by throwing, and a *negative* key would
  have indexed `expected_nodes` out of bounds.

### Added

- **`Engine::parse_int`** (`StratEngine/Utils/ArgParse.h`) — the single path for every `argv` and
  JSON-key integer. Stricter than `std::stoi`, which accepts trailing garbage (`"12abc"` → 12) and
  reports everything else by throwing. `[argparse]` and `[config]` test tags.

### Notes

`Game::Init` is called from `Game`'s constructor, so a throwing `Init` means the object is never
fully constructed and `~Game()` never runs — which is why catching in `main` needed no cleanup work
alongside it. `unsubscribePlayerEvents`'s "assumes both players exist" TODO is unreachable from this
path and was left alone.

The mid-search guard needs an explicit flag: `search_thread_.joinable()` looks equivalent but stays
true after the thread function returns, so it would have rejected the `position` of every normal
`go` → `bestmove` → `position` cycle. A test pins that reasoning.

CFG is split to its own issue — the shipped binary has ASLR, high-entropy VA, DEP and `/GS` (all
verified present, so the CMake migration lost nothing), but its CFG function table is empty, making
the guard inert. Enabling it needs a measurement, not a bugfix. Design:
`.claude/plans/harden-external-input-sites.md`.

---

## 2026-08-06 — Strength lab runs sharded, pooled pentanomially (M5)

### Changed

- **`strength.yml` split into `setup` → `build` → `match` (matrix) → `aggregate`.** Both engines,
  fastchess and the book are built and staged once and shipped to every shard as one artifact, so
  the shards provably play the same two binaries against the same book rather than 20 independent
  builds being assumed to agree.
- **Shards slice the book by arithmetic, not file surgery.** With `order=sequential`, shard *i*
  playing *R* pairs starts at `i*R + 1`, so slices cannot overlap. Verified every run by comparing
  the first FEN of each shard's PGN, which also hard-fails if the PGNs carry no `[FEN]` tag — without
  that, the check would silently pass forever.

### Added

- **`.github/scripts/pool_pentanomial.py`** pools shard results at colour-swapped *pair* level.
  Pooling raw W/L/D and applying an independent-games formula understates the variance, giving an
  interval that is wrong in the direction of looking more precise. `--self-test` checks the formula
  against six real (counts, Elo, interval) triples from this project's own matches spanning 6 to
  3500 games; it reproduces fastchess's own output to both decimals on all six. That check runs in
  `setup`, before anything expensive.
- **Refusal to pool a partial batch.** A run where three shards of twenty died is not a smaller
  batch, it is a biased one — the survivors are those that avoided whatever went wrong. The
  aggregator requires exactly the expected shard count and the workflow reports the discard.

### Notes

**Calibrated at scale.** A 20,000-game null test (20 shards × 1000, run `31054348465`, 2 h 47 min)
returned **-2.17 ± 4.18** — agreeing with the single-job instrument's `-3.47 ± 18.21`, containing the
true zero the test guarantees, and hitting the forecast ±4 resolution. Shard slices verified disjoint
(20 opening positions, 20 unique), zero time losses across all 20 shards. Recorded in the Linux
ledger. Mechanics were proven first by a cheap 2-shard, 20-game dispatch whose `+70.44 ± 119.43` was
mechanism rather than measurement.

Resolution is now **±4 Elo**, against ±18 single-job and ±25-26 locally — the first interval that can
resolve one of epic #110's single-digit eval terms. Still `workflow_dispatch` only; wiring it to PRs
is M6, and a full dispatch holds the entire 20-job concurrent allowance for three hours, which
constrains how that trigger can be designed.

`.gitignore` blanket-ignores `/.github/*` behind an allowlist, so the new script was silently skipped
by `git add -A` and the first dispatch died at the aggregate step on a file that was never committed.
The allowlist now covers `.github/scripts/`. Design:
`.claude/plans/public-repo-and-strength-lab.md`.

---

## 2026-08-05 — Time budget is bounded by the clock (#204)

### Fixed

- **`compute_budget()` could return a budget larger than the clock it was given**, so at any
  increment below 100 ms every move cost more than it repaid and the forfeit was only a question of
  how many moves remained. Three compounding faults: `usable` was floored at 100 ms (inventing time
  that was not there), `soft` was floored at an absolute 100 ms, and a trailing
  `max(min(candidate, cap), soft)` let `soft` override the `usable/2` cap entirely. The last is what
  actually forfeited — at 200 ms remaining with a 5 s increment the cap computed 75 ms and the
  function returned 4005 ms.
- The floor is now clock-relative (`min(100 ms, usable/2)`) and the cap bounds **both** limits via
  `clamp`, so `hard <= max(remaining - overhead, 0) / 2` holds structurally rather than by case
  analysis. A clock at or below the 50 ms overhead yields a zero budget; `handle_empty_move_emergency()`
  supplies a legal move, which is the only non-forfeiting option at that point.

### Notes

Budgets are unchanged wherever `remaining > 249 ms` — verified by sweeping every millisecond at
10+0.1 — so the existing Elo baseline carries over and no new match was needed. Below that the two
formulas diverge exactly where the old one forfeited. Simulating a 2+0.02 clock while spending the
*hard* limit every move now settles at 91 ms remaining instead of draining to zero.

`TimeUtils.h`'s documented invariant said `hard >= soft >= 100 ms`, which was the defect written
down; it now reads `usable/2 >= hard >= soft >= 0`. Three `[time_mgr]` cases asserted the old floor
and were updated; the blitz, classical and zero-increment cases were left untouched deliberately, as
the evidence that normal play did not move. Design: `.claude/plans/time-budget-clock-relative-floor.md`.

---

## 2026-08-05 — `Get-BuildArtifact.ps1` fails on a missing binary by default

### Changed

- **`-RequireExists` became `-AllowMissing`**, inverting the default. A missing binary is now an
  error at resolution instead of a path handed back to a caller that will fail later, or run
  something stale from another checkout.
- `Validate-PrePR.ps1` and `Run-EloMatch.ps1` pass `-AllowMissing`: both legitimately resolve the
  path *before* it exists — Validate-PrePR builds in Step 1, and `Run-EloMatch -Resume` needs no
  candidate build at all. Nothing else passed `-RequireExists`, so every other caller gains the check.

### Notes

Prompted by a session where a script pointed at the main checkout's stale binary and reported a fix
as not working, and another where a returned-but-absent path failed several steps later with an
unrelated-looking message. The script already resolved paths relative to its own location, which is
the guard against the cross-worktree case; what it lacked was refusing to hand back a path to
something that is not there.

---

## 2026-08-05 — A rejected FEN no longer leaves the previous position on the board

Closes #200.

### Fixed

- **`cmd_position` reset the board to the start position when `SetupFromFEN` rejects a FEN**, and
  reports it with `info string`. It previously kept whatever position was already loaded and said
  nothing, so the engine answered for a position the caller never sent — and the answer depended on
  what had been loaded earlier in the session. The same command genuinely returned 20 moves in one
  session and 48 in another.
- **`MoveFormatter::FromUCI` validated its square characters.** It checked only that the token was at
  least four characters, so `zzzz` computed file 25 and rank −66 and indexed the mailbox at −503 — an
  out-of-bounds read reachable from any GUI sending `position startpos moves zzzz`. Found by a test
  written for the change above, which crashed the test binary with `0xC0000409`.
- **An unparseable move stops the replay and is reported**, rather than being skipped silently. Later
  moves are relative to the position the skipped one would have produced, so replaying them built a
  position nobody described.

### Notes

The previous behaviour was deliberate — it carried a comment arguing that resetting "would answer
`bestmove` for a position the GUI never sent", and a test asserting it. Both rested on premises that
do not hold: keeping the previous position answers for an unsent position too, just less visibly and
non-reproducibly, and `info string` is the error channel UCI has. The test now asserts the new
contract.

`Board::SetupFromFEN` itself is unchanged and still leaves the board untouched on failure; the reset
belongs to the UCI layer, which is what owns the session.

---

## 2026-08-05 — Opening book is selectable, and book exhaustion is now visible

M3 of `.claude/plans/public-repo-and-strength-lab.md`.

### Added

- **`-Book <path>`** on `Run-EloMatch.ps1`. Empty auto-resolves: `EngineTesting\openings-large.pgn`
  or `.epd` if present, else the committed 250-opening smoke book. The fastchess `format=` flag
  follows the file extension.
- **An exhaustion warning.** Every run now prints the book and its opening count, and warns when
  `-Games` exceeds the 2N distinct games N openings can produce. 500 games consumes the committed
  book exactly; 501 warns.

### Notes

Large books are deliberately **not committed**: third-party data of varying provenance in a public
repository. They live in `EngineTesting\` with fastchess and the reference binaries, which is where
every other external test asset already lives. `Docs/EloLog.md` records how to add one and why rows
measured on different books are not directly comparable.

---

## 2026-08-04 — UCI `go perft` command

### Added

- **`perft <depth>` and `go perft <depth>` in `UciHandler`**, delegating to `Testing::Perft::divide()`
  on the current position. Output is the conventional divide format (`a2a4: 420`), which matches the
  regex external validators parse. Depth is bounded by the same limit the `perft` CLI subcommand
  uses, and malformed input is ignored per UCI convention.

  `stop_and_join()` first: `Perft::divide` walks the tree with `DoMove`/`UndoMove` on `board_`, which
  a running search is reading.

  Unblocks #196 — perftcheck and similar harnesses drive engines over UCI, and perft was previously
  reachable only through the CLI subcommands.

### Notes

`go perft` is dispatched before the bare `go` branch, or it would be parsed as a search whose unknown
tokens are silently skipped. Two `[uci][perft]` tests drive `run()` over redirected stdin to cover
that ordering specifically — the other tests call `cmd_perft` directly and would not catch it.

---

## 2026-08-04 — Perft suite enters the PR gate; CPW positions 4, 5 and 6 added

### Added

- **`perft test` runs on `build-linux`, Release leg only** — the 131-position, 655-check suite behind
  `Tests/perft_test_cases.json`, which previously ran in no automated gate. The Catch2 `[perft]` tests
  cover seven hardcoded cases (startpos d1-4, Kiwipete d1-3); this is the rest. Release-only because
  perft is compute-bound: 30 s optimised against 4 of 131 positions in six minutes under Debug.
- **CPW positions 4, 5 and 6** added to that suite at depths 1-5 (position 3 was already present).
  They are the standard promotion, pin and castling torture positions. All 15 published node counts
  were reproduced by the engine before being committed — they lock in correct behaviour rather than
  reporting a fault.

### Changed

- **`build-linux` builds `all`**, matching the Windows job. `StratChessEvolved.cpp` — `main()` and the
  perft, tactical and eval runners — belongs to no other target, so GCC never compiled it on a PR. It
  also exercises the GCC LTO link, which is not MSVC-specific.

### Notes

Runners perft at **~22.5 Mnps**, 2.2× slower than a local build (startpos depth 7 in 140 s, Kiwipete
depth 6 in 364 s). Deeper perft was costed from that and declined: startpos(8) ~62 min, Kiwipete(7)
~4.7 h against a 6-hour job cap, and neither adds coverage — startpos(7) already exceeds 2^31 and
perft allocates nothing per node. Reasoning recorded in `Docs/Workflow.md`.

---

## 2026-08-04 — Nightly correctness workflow

M2 of `.claude/plans/public-repo-and-strength-lab.md`. `nightly.yml` runs at 03:00 UTC and on
`workflow_dispatch`; it gates nothing.

### Added

- `deep-perft` — `perft(7)` from the start position and `perft(6)` from Kiwipete, compared against
  their known node counts. The fast tier stops at depth 4. Measured at ~50 Mnps locally these are ~1
  and ~3 minutes, so neither is sharded.
- `extended-tests` — the `[slow]` tier in Release and Debug.
- `sanitize-extended` — that tier under ASan+UBSan plus `_GLIBCXX_DEBUG`.
- `tactical-stability` — `tactical stability 100`.
- **`STRAT_STDLIB_DEBUG`** (CMake BOOL, `OFF`) — libstdc++ debug mode: checked iterators and
  container preconditions, which `_GLIBCXX_ASSERTIONS` does not cover. A configure error on MSVC and
  clang-cl, whose Debug builds get the equivalent from `_ITERATOR_DEBUG_LEVEL`. Enabled on the per-PR
  `sanitize-linux` job as well, after a local GCC 13.3.0 run (the runner's compiler) came back clean
  over the whole suite in 25 s.

### Notes

The `[slow]` tier is three test cases — 253 against the fast tier's 250. `extended-tests` and
`sanitize-extended` cost nothing but are weak evidence; "extended tier" oversells what exists.
Growing it belongs to #156.

---

## 2026-08-04 — Repository made public; CI un-gated and promoted to a merge gate

Milestone M1 of `.claude/plans/public-repo-and-strength-lab.md`. Public standard runners are free and
required status checks are available, so the rationing the private repository needed is reversed.

### Changed

- **`windows-ci` label gate removed** — all three build jobs now carry an identical tier condition.
- **`build-and-test-result` is a required check on `main`**; a red run blocks the merge. A SKIPPED
  leg reports success, so Docs and Tooling PRs are not blocked by jobs that correctly never ran.
- **Windows legs build `all`, not `tests`** — `INTERPROCEDURAL_OPTIMIZATION_RELEASE` is set on the
  `StratChessEvolved` target alone, so a tests-only build never performed the ThinLTO link that ships.
- **`concurrency:` group added**, cancelling superseded PR runs; the 20-job ceiling is account-wide.
- Dropped `labeled` from the `pull_request` trigger types.

### Fixed

- The claim that `Validate-PrePR.ps1` is *a strict superset* of the Windows job, in both
  `build-and-test.yml` and `Docs/Workflow.md`. It never passes `-Config`, so it builds Release only
  and never compiles Debug.

### Notes

Public runners are 4-core against the private tier's 2-core: Linux jobs got 20-40% faster with no
configuration change (`sanitize-linux` 5m34 → 3m17).

Preceded by M0 — four superseded documents deleted and a root `README.md` added (PR #191); history
rewritten to drop a stale cppcheck dump and collapse three author identities, with the 44 SHAs quoted
in prose remapped (PR #192). All 505 SHAs changed; the HEAD tree and both Elo anchor trees are
byte-identical, so `Docs/EloLog.md` remains valid as measured.

---

## 2026-08-04 — Tier-gate the push trigger; narrow the `windows-ci` rule (issues #185, #187)

### Fixed
- **`classify` can now read a push.** It diffed `origin/main...HEAD`, which on a push to `main` is
  empty — so every merge classified as Docs regardless of content, and each job carried
  `github.event_name == 'push' ||` to compensate, running full Linux validation on docs-only merges.
  Pushes now diff against `github.event.before`. Chosen over `HEAD^` because it stays correct when a
  push carries several commits; identical for this repo's one-merge-commit-per-push shape.
  An unreachable ref still fails closed to Engine tier.
- `build-linux` and `sanitize-linux` are tier-gated on both events. The Windows job's condition reads
  `github.event.pull_request.labels`, already null on a push, and is unchanged.

### Changed
- **The `windows-ci` rule narrowed** from "any PR touching `StratEngine/**`, the CMake files or the
  workflow" to diffs that can change *what the Windows build produces* — `CMakePresets.json`,
  `build.ps1`, `Compat.h`, the clang-cl branch of `strat_configure_target`, or any `_MSC_VER`/`_WIN32`
  conditional. The old rule covered essentially all engine work; `Validate-PrePR.ps1` is a documented
  strict superset of the job, `build-linux` supplies the clean checkout, and `sanitize-linux` (#186)
  narrowed the remainder again. The narrowing assumes the local script ran — a PR pushed around
  `New-PullRequest.ps1` should still be labelled.

### Notes
- Residual unique Windows coverage is **Windows-specific Debug**: `Validate-PrePR.ps1` never passes
  `-Config`, so it builds Release only, while the Windows job runs Release+Debug. Closing that
  locally instead remains open in #187.
- The `windows-ci` label did not exist in the repository until 2026-08-04. Nothing was missed — the
  only merge between #182 and then would not have qualified — but a label gate whose label is absent
  fails open silently, so it is recorded here.

---

## 2026-08-04 — ASan/UBSan in CI (issue #179)

### Added
- **`sanitize-linux` CI job** — builds `StratChessTests` with `-fsanitize=address,undefined` in Debug
  and runs the fast tier, on the same trigger as `build-linux`. Debug rather than Release so
  `assert()` and the `#ifndef NDEBUG` tripwires stay live alongside the instrumentation; they catch a
  different class of fault than the sanitizers do.
- **`STRAT_SANITIZE`** (CMake STRING, empty by default) — passed through to `-fsanitize=`. Applied to
  both the compile and the link line, since `-fsanitize=` also selects the runtime libraries. Adds
  `-D_GLIBCXX_ASSERTIONS`, because ASan sees a container's heap block but not the elements inside it,
  so an out-of-range `std::vector::operator[]` is otherwise never reported.
- **`STRAT_SANITIZE_RECOVER`** (CMake BOOL, `OFF`) — `OFF` adds `-fno-sanitize-recover=all`, which is
  what makes a finding fail the job: sanitizers report and continue by default, exiting 0 with the
  findings only in the log. `ON` is survey mode, for a first pass over an uninstrumented
  configuration where aborting on the first report means one rebuild per finding.
- **Configure error on MSVC and clang-cl.** The GNU `-fsanitize=` spelling does not survive the MSVC
  driver, and clang-cl is the compiler that accepts flags and drops them before the frontend (#84), so
  requesting sanitizers on Windows fails loudly rather than producing an uninstrumented binary that
  looks instrumented.

### Notes
- **The engine was clean on first contact** — 250 test cases / 3520 assertions, zero ASan findings,
  zero UBSan findings, zero leaks. The "expect a first-run backlog" caution in #179 did not apply to
  the fast tier, so the job landed already enforcing rather than advisory.
- Scope limit: the fast tier only. Code paths reached solely by the `[slow]` tactical suite or a
  deeper search are not covered.
- No new test was needed for move generation: `PerftTests.cpp` already runs the start position to
  depth 4 and Kiwipete to depth 3 in the fast tier.
- TSan was deliberately left out and filed as #184 — the shared TT is racy by design, so the work
  there is deciding which reports are signal, not adding a flag. The push-trigger waste noticed while
  reviewing the job's condition is #185.

---

## 2026-07-30 — Reject illegal FENs: waiting side in check (issue #45)

### Added
- **`Board::WaitingSideInCheck()`** — the mirror of `InCheck()`: true when the king of the side *not*
  to move is attacked. Unlike `InCheck()` that is not a legal state, since the waiting side would
  have had to leave its king en prise. Kings on adjacent squares are covered by the same test,
  because `GetAttackBoard` includes king attacks.
- **`SetupFromFEN` rejects such a position**, reporting through the `bool` channel added for #155, so
  `position fen <illegal>` is declined and the board keeps what it held.

  The symptom #45 reported — `bestmove e1e8`, capturing the king — no longer reproduced by the time
  this was fixed, and what was happening instead is worse. Measured on the pre-change engine with
  `4k3/8/8/8/8/5b2/8/4RK2 w - - 0 1`:

  - **Release**: it plays ordinary moves (`e1e7`/`e1e6`/`f1f2` at depths 1/2/6) and reports a routine
    material verdict for an impossible position. The king capture *is* generated — nothing in
    `MoveGenerator` filters it — but `DoMove` discards it by accident, not by design: `MoveHelper::IsValid`
    has a "cannot take a King" rule that `DoMove` only consults inside an `assert()`, so in Release the
    capture proceeds, the king is removed, and `InCheck()` then calls `GetFirstPiece()` on the now-empty
    king bitboard. That violates the function's own documented `mask != 0` precondition:
    `countr_zero(0)` is 64, so `g_bbKingMoves[64]` reads one entry past a 64-entry table. The garbage
    that comes back makes `DoMove` conclude the mover left its own king in check, so it rolls back and
    returns false.
  - **Debug**: the same position aborts the process on `assert(MoveHelper::IsValid(...))`.

  So the real pre-change exposure was an out-of-bounds read (Release) or an abort (Debug) reachable from
  any hand-written FEN. Rejecting the position at load closes the only externally reachable route to it;
  hardening the paths themselves — the assert-only invariants and `GetFirstPiece`'s precondition — is
  tracked as #163, since a guard in `DoMove` or `GetAttackBoard` costs nps and needs measuring.

### Fixed
Two pre-existing illegal positions, found by sweeping all 292 FENs in the repository (source literals
plus the `Tests/*.json` suites) through the loader:

- **`FEN_ROOK_ON_7TH` (`EvalTests.cpp`)** — White's rook on e7 gave check to the black king on e8
  while it was White to move. The black king moves to g8; the rook stays on e7 and the e-file stays
  pawnless, so the open-file and 7th-rank assertions are unchanged. It is the only one of that file's
  FEN constants without a hand-verification note in its comment.
- **`M2-001` (`Tests/tactical_test_cases.json`)** — the second white rook stood on h1, checking the
  black king down the open h-file with White to move. It moves to g1, which leaves `Rb8#` (the mate
  the case actually tests, and now its only listed answer) intact; verified against the engine, which
  finds it at depth 2. The unreachable `h1h7` alternative is dropped.

### Notes
- The check runs on a scratch `Board` before the real one is touched, preserving #155's guarantee
  that a rejected FEN mutates nothing. `setup_board()` therefore runs twice on a successful load —
  deliberate, and trivial next to a search.
- Legality cannot live in `FENParser` (no board, so no attack generation). `FenBatch::ClassifyLine`
  consequently validates *syntax* only, and the batch eval runner's `!SetupFromFEN` branch — dead
  when written in #162 — is now what keeps an illegal position out of a tuning corpus.
- The issue's alternative (load it, answer `bestmove 0000`) was not taken: declining makes the
  illegal position unrepresentable instead of something every board consumer must special-case.
- No Elo impact: no legal position's evaluation or search changes.
- Plan: `.claude/plans/fen-legality-validation.md`.

---

## 2026-07-30 — `SetupFromFEN` error channel (issues #155, #46)

### Changed
- **`Board::SetupFromFEN` returns `[[nodiscard]] bool`** instead of `void`. On failure it logs the
  parse error and leaves the board exactly as it was. `[[nodiscard]]` under `/WX` is what enforces
  this: a discarded return is `warning C4834` → `error C2220`, so no call site can keep the old
  silent behaviour. All 8 engine/app call sites and 31 test call sites handle it.
- **`UciHandler::cmd_position` declines a malformed FEN**: the board keeps the position it held and
  the function returns before the `moves` list is parsed, so a move list is never replayed onto a
  position the requested FEN did not load. Logged at debug level — UCI has no error channel, and
  silently ignoring the command is conventional. Deliberately *not* a reset to the starting
  position, which would answer `bestmove` for a position the GUI never sent.
- **`Config::ReadFEN` falls back to the standard opening position** when the FEN in
  `game_settings.json` does not parse, matching what the empty-FEN path above it already did.
  `SetCustomGame()` is skipped on that path, since a default board is not a custom game.
- **`Board(const std::string& fen)` asserts** on a malformed FEN. A constructor has no way to
  report failure and every caller passes a literal, so Debug is where that should surface; Release
  behaviour is unchanged (empty board, error logged).
- `Perft::run_test_suite` now fails the suite on an unparseable suite FEN rather than running the
  position against an empty board.

### Fixed
- **#46** — a FEN missing its side-to-move field no longer leads to output for a position that was
  never loaded. The parse half was already fixed by #143's four-field floor (a bare piece-placement
  string is rejected, so the side-to-move default is unreachable); what remained was `cmd_position`
  ignoring the failure. Covered by `[uci]` regression tests.

### Notes
- Error handling only — no behaviour change on any well-formed FEN, and no Elo impact.
- Out of scope: FEN *legality* validation (a position whose non-mover is in check, #45). This change
  builds the channel; #45 adds a rule that reports through it. `FenBatch::ClassifyLine`'s double
  parse is likewise left for the follow-on #155 mentions.
- Plan: `.claude/plans/setupfromfen-error-channel.md`.

---

## 2026-07-29 — Bishop pair, connected rooks, castling (issues #111, #114, #115)

### Added
- **Bishop pair** (`eval_bishops`) — requires bishops on **opposite square colours**, not a count of
  two. The term exists because the pair covers both colours; two same-coloured bishops (reachable by
  underpromotion) do not, and a `popcount >= 2` shortcut would pay for them anyway. `ScorePair{30, 45}`,
  rising to the endgame as the board opens.
- **Connected rooks** (inside `eval_rooks`) — same rank or file with nothing between, scored per
  connected **pair**. Reuses `RookAttacks` so blockers come from the existing PEXT tables (#108); each
  pair is counted once by testing only against not-yet-visited rooks. `ScorePair{15, 8}`, falling to the
  endgame where the 7th-rank bonus already pays.
- **Castling** (`eval_castling`) — graded by king file on the home rank once both castling rights are
  gone: a/b/c and g/h sheltered, f neutral, d/e central. Middlegame-only (`{25, 0}` / `{-20, 0}`).
- `EvalBreakdown` gains `bishops` and `castling` rows, so the UCI `eval` table still sums to the score.

### Notes
- **Castling is derived from the position, never from history.** Whether a side actually castled is not
  recoverable from a FEN, so a `hasCastled` flag would have made `Evaluate()` a function of *how* a
  position was reached: two paths to one position would disagree while sharing a transposition-table
  entry, and #117's FEN corpus would score every position as never-castled. The term reads castling
  *rights* — a FEN field — plus king placement, which is the issue's own "irrevocably lost the right"
  formulation and is position-pure. See `.claude/plans/eval-bishop-pair-connected-rooks-castling.md` D2.
- Constants are untuned starting points; #117 owns fitting them.

### Measured
**SPRT `Custom [-5, 15]` vs `main` @ 56b3f45: H1 accepted** at 378 games (LLR 2.97, LOS 99.49%),
+38.76 ± 29.86, 30m52s. Read as "the three terms are worth having" — the 95% interval [+8.90, +68.62]
excludes zero, but the point estimate should not be quoted as a figure. Attribution among the three was
traded away deliberately for budget: one SPRT instead of three.

A companion run against the fixed `elo-reference-v1` anchor accepted H1 in 23 minutes, but measures
cumulative standing rather than this change — an SPRT against a fixed anchor tests the *sum*. Both rows
are in `Docs/EloLog.md`, labelled; the conflict between the anchor convention and per-change SPRT
verdicts is tracked in #159.

The gain is evaluation quality, not search efficiency. A first 3-position probe appeared to show a
21.75% node reduction to depth 11, but that was a sampling artifact — across 5 term-isolating
positions the spread is -44% to +229% with a -1.6% aggregate. Node count at fixed depth is chaotic
under small eval perturbations (a 20-30 cp shift can flip the PV and change the entire tree), and
move ordering never consults static eval anyway: `Sort.cpp` uses TT move, MVV-LVA, killers and
history. Static eval reaches only the leaf return, the qsearch stand-pat cutoff and alpha raise, and
delta pruning. The terms therefore pay a real 2.03% nps cost, buy no node reduction, and still won —
which is what an evaluation improvement is supposed to look like.

---

## 2026-07-29 — FEN halfmove/fullmove fields made genuinely optional (issue #143)

### Fixed
- `FENParser::ParseFEN`'s regex ended in `\d+\s+\d+\s*$`, mandating all six FEN fields, while the
  parser body below it treated fields 5 and 6 as optional and was commented as such. The
  optionality had been silently disabled since it was written; the regex now accepts 4-6 fields.
  Strictly more permissive — no previously-valid input changes behaviour.
- The field-count check (`parts.size() < 4`, "too few fields in FEN") was unreachable dead code,
  because the regex ran first and rejected such input with the generic "overall format invalid".
  Tokenizing now happens before the regex, so the specific message actually reaches callers.
- `FenBatch::ClassifyLine`'s own field-count pre-filter is removed. It reported *"malformed FEN
  (N field(s), need at least 4)"* when 4 fields were in fact rejected — advice that would not have
  helped anyone cleaning a corpus. One tier now does what two did, without the wrong message.

### Notes
- Halfmove defaults to 0 and fullmove to 1 when omitted, now documented at the point of use.
  The halfmove default is not inert: `Board::SetupFromFEN` feeds it to `gameInfo_.fiftyCount`,
  which drives 50-move draw detection. Bookkeeping only — unlike a missing side-to-move field
  (issue #46), it cannot change whose move it is.
- **EPD is deliberately still rejected.** Trailing operations (`c9 "1-0";`) after the four core
  fields are out of scope for the FEN grammar; that belongs to issue #117's corpus loader.
  Accepting 4-6 fields is a prerequisite for EPD ingestion, not a delivery of it.
- Expected Elo impact: none. Input-format leniency only; no score changes.

---

## 2026-07-29 — Tapered Evaluation (issue #99, carries #118 item 4)

### Changed
- Evaluation is now interpolated between a midgame and an endgame score instead of hard-switching
  on a material threshold. Game phase is an integer in `[0, 24]` computed from non-king, non-pawn
  piece counts (N=1, B=1, R=2, Q=4 over both sides), clamped against promotion overshoot. Terms
  return a `ScorePair{mg, eg}`; `BlendPhase` interpolates.
- **The king PST is the first tapered term.** `g_Eval_Bitboards[5]` and `[6]` were always an
  (mg, eg) pair — they were just selected discontinuously. Blending them removes a cliff of up to
  100 cp that a single capture could cross mid-search, which scored positions by *when* a trade
  happened rather than whether it was good.
- Rook-on-7th became endgame-weighted (0 at mg, full bonus at eg) rather than hard-gated. Whether
  that bonus should be endgame-only at all is dubious chess, but re-weighting it is #117's job.
- `PlayState` (and the never-assigned `FINALGAME`) deleted. `EvalContext::stage` is replaced by
  `phase`; `EvalBreakdown` reports `phase`, and the UCI `eval` command prints `phase: N/24` in
  place of `stage: <name>`.

### Fixed
- **#118 item 4** — the winning king's PST is now suppressed while mop-up is active. The endgame
  king table charged that king 10 cp per step of centralization given up to approach the cornered
  loser, against the 4 cp per step mop-up paid for closing in, so approaching scored **negative**:
  mop-up only ever softened a disincentive it was written to remove. This is the most likely
  reason #70 measured ≈0 Elo. It also decouples `MOPUP_CMD_WEIGHT` from the endgame king PST
  slope — two numbers expressing one concept, which #118 flags as a prerequisite for #117.

### Design notes
- The old `min(material) <= 11500` threshold was wrong in three ways, all removed: it was a
  discontinuity, it keyed on a king-inclusive material sum (the entire reason for the otherwise
  inexplicable `11500 = 10000 + 1500`), and it took `min()` over both sides — so a player still
  holding a queen switched to endgame king scoring as soon as its *opponent* was stripped down.
- Phase is piece-count based, not material-sum based: a material sum conflates "few pieces left"
  with "one side is winning", which is the same confusion in another form.
- `Evaluate()` blends **per term** rather than once over the accumulated pair — a deliberate
  departure from the plan's D2. Integer truncation makes `BlendPhase(a) + BlendPhase(b)` and
  `BlendPhase(a + b)` differ by up to 1 cp per term, which would break #129's asserted
  rows-sum-to-total invariant by ~3 cp. Bounded, deterministic accuracy was traded for a
  breakdown that still reconciles with the score it reports.
- Mop-up stays a discrete gate (now keyed on phase), not a blended term: it is a special case for
  pawnless decisive endings, not a smoothly-scaling idea. The gate moved into `BuildContext` so
  `eval_pst` and `eval_mopup` read one definition of "mopping up" rather than two copies.
- Scope was held to terms that already had phase-dependent behaviour. Inventing `(mg, eg)` pairs
  for every PST is a tuning exercise (#117), and doing it here would have made this unmeasurable.

### Validation
- Step-1 identity: with every term at `mg == eg`, a 1731-position corpus scored byte-identical
  (SHA256 `F6D51B9B…`) via #129's batch mode — isolating the plumbing from the behaviour change.
  After tapering, 344 of those 1731 positions move.
- Full fast tier passes in Release **and** Debug (2842 assertions, 216 cases), including new
  endpoint, monotonicity, no-cliff and #118-item-4 cases. The #118 regression test was written
  first and confirmed failing.

See `.claude/plans/tapered-evaluation.md` for the full reasoning, including the drift review done
before implementation.

---

## 2026-07-27 — Static-Eval Per-Term Breakdown (issue #129 phase 2)

### Added
- `EvalBreakdown` (`StratEngine/Eval.h`) and `EvalComplex::Breakdown()` (`StratEngine/Eval.cpp`):
  the public, production path to what each evaluation term contributes for a position, per color,
  plus the game stage and the total. Rows come from the same `BuildContext` and the same four
  private term functions `Evaluate()` calls — never a parallel computation.
- The UCI `eval` command now prints a `white | black | net` table above phase 1's two total lines,
  followed by the game stage. The per-color split, not just the net, is the point: a net of -30
  does not say whether a term is penalising White or rewarding Black, which is usually what is
  being debugged. Completes issue #129; unblocked by the #127 restructure (PR #141).

### Changed
- `UciHandler::cmd_eval()` downcasts `eval_` to `EvalComplex` to reach the breakdown, mirroring
  the `dynamic_cast` `init_ai()` already performs. The two total lines print unconditionally, so
  an evaluator with no terms to report degrades to exactly phase 1's output.

### Design notes
- Visibility widened by exactly one method: `BuildContext` and the four `eval_*` functions stay
  private, and `EvalManager`'s abstract interface was deliberately left alone rather than given a
  `virtual Breakdown()` that `EvalSimple` — and every future evaluator — would inherit and never
  implement.
- `EvalBreakdown::total` is `Evaluate()`'s own return value rather than a restatement of its
  side-to-move sign flip, so the flip lives in one place and `eval` cannot print a total the
  search would disagree with. `Evaluate()` itself is byte-for-byte unchanged — rewriting it as
  `return Breakdown(board).total;` would make non-drift structural but puts a struct fill on the
  path `AIPerplex::quiescence` calls millions of times a second.
- `material` is printed verbatim from `EvalContext`, so king-inclusive (10000 cp per side) — a
  king-stripped display figure would be a number no part of the evaluator computes. That it
  cancels in `net` is documented at `EvalContext::material` and at the print site rather than
  emitted on every call — it is a fixed property of the evaluator, not information about the
  position being examined.

### Validation
- `[uci]` and `[eval]` tags plus the full fast tier, Release and Debug. The honesty invariant is
  asserted on the *printed* table (`UCITests.cpp`): every row's net equals its own white-minus-
  black, the net column sums to the printed total, and that total equals a white-relative
  `Evaluate()` computed independently — across five positions covering middlegame, endgame,
  Black-to-move and an active mop-up term.
- No ELO match: no evaluation value changes, and search never calls the new path.

See `.claude/plans/uci-eval-command-term-breakdown.md` (D7–D10) for the full reasoning.

---

## 2026-07-27 — EvalContext Restructure (issue #127)

### Changed
- `EvalComplex::Evaluate()` (`StratEngine/Eval.cpp`) reshaped from one ~130-line function built
  around a 12-way `switch` into an `EvalContext` struct (`StratEngine/Eval.h`) holding the shared
  per-call intermediates (bitboards, pawn masks, king squares, material, game stage) plus four
  private per-term functions — `eval_pawns`, `eval_rooks`, `eval_pst`, `eval_mopup` — each
  `(const EvalContext&, eColor) -> int`. `Evaluate()` itself is now ~25 lines: build the context,
  sum the four terms per color, apply the side-to-move-relative material difference. Pure
  restructure — no new term, no changed weight, no changed behaviour.
- `eval_pst` uses per-piece-type bitboard loops (one per non-king piece type, plus a dedicated
  king branch) instead of a single mailbox-lookup pass over every occupied square, dropping a
  `Board::GetPiece(square)` call per piece. Landed as its own step after the other three terms
  were already extracted, since it was judged the change most likely to disturb score identity.
- Two structural quirks are preserved exactly, now with comments explaining why: the king is
  excluded from the generic PST add and instead uses a stage-selected table
  (`g_Eval_Bitboards[5]`/`[6]`) from its own branch in `eval_pst`; `Board::GetMaterialScore`
  still includes the king at 10000 cp and that inclusion still cancels in the final
  white-minus-black difference. Phase detection (`iMinScore <= 11500`, the king-inclusive
  material, and the `min()` over both sides) is untouched — it is wrong in known ways that belong
  to #99, not this restructure — and `EvalContext::stage`'s doc comment now carries the
  explanation for the `11500` constant so it doesn't need re-deriving.
- Issue #126's rook open-file fix (PR #137: pawns-only classification, not
  `all_black`/`all_white`) and the deliberately-asymmetric own-pawn-forward-only /
  enemy-pawn-whole-file scope (open question tracked on #116) both carry over unchanged into
  `eval_rooks`.
- `EvalManager` gained no data members; `Evaluate()` is still `const`; `EvalContext` is always a
  per-call stack local, never a member — the Lazy SMP sharing contract documented in `Eval.h`'s
  class comment still holds verbatim.
- **Kingless-board guard (post-review fix, real behavioural guarantee):** the initial restructure
  called `Board::GetFirstPiece` on both king bitboards unconditionally while building `EvalContext`,
  and `eval_pst` applied a king PST unconditionally. `GetFirstPiece`'s `assert(mask != 0)`
  precondition is compiled out in Release, so a kingless `Board` — reachable in production via
  `UciHandler::board_`, which is never seeded with the start position, so a UCI `eval` issued before
  any `position` command hits it — silently indexed `g_Eval_Bitboards` out of bounds instead of
  trapping; Debug caught it as an assertion failure in `[uci]`. Fixed by giving `EvalContext::king_sq`
  a `NO_SQUARE` sentinel for a colorless king and gating both `eval_pst`'s king branch and
  `eval_mopup` on it, restoring the pre-#127 behaviour exactly: a kingless board evaluates to `0`,
  as it always did when the king PST lived inside a loop over `ALL_PIECES` that never iterates on an
  empty board.
- Context construction moved into `EvalComplex::BuildContext()`, the single site both `Evaluate()`
  and the term-level test fixture call — closes the drift hazard where the fixture previously
  re-implemented the `11500` phase threshold by hand. The three `TODO` breadcrumbs the old `switch`
  carried (passed pawns, connected rooks, castling-done) are re-attached to the term functions that
  now own that responsibility.

### Added
- `StratChessTests/EvalTests.cpp`: term-level `[eval]` cases, the restructure's main secondary
  benefit — terms are individually callable now, so they can be asserted on directly instead of
  only inferred from whole-position deltas. `EvalComplexTestFixture` (a `STRAT_ENABLE_TEST_ACCESS`
  friend of `EvalComplex`, same mechanism as the AIPerplex/UciHandler fixtures) builds an
  `EvalContext` from a `Board` and forwards to each term. New cases cover exact pawn-penalty and
  rook-bonus values, the king's exactly-once stage-selected PST contribution (verified against an
  independently-computed expected value), mop-up's winner-only contribution, and a structural
  check that the four terms plus raw material reproduce `Evaluate()`'s result exactly. All
  existing `[eval]` cases, including the #125 colour-symmetry and #126 rook cases, pass unchanged.

### Validation
- **Score identity (primary evidence)**: an 8574-position corpus — FENs harvested from
  `Tests/perft_test_cases.json`, `Tests/tactical_test_cases.json`, `StratChessTests/*.cpp`
  literals, `Tests/openings/openings-250.pgn`, and self-play PGNs — scored with the pre-restructure
  binary and the post-restructure binary via the batch `eval` mode (#129). The two outputs are
  byte-identical (matching SHA-256 hash), checked after every incremental step of the restructure,
  not just at the end. The one-off harvester that built this corpus was later preserved as
  `StratChessEvolved/Scripts/build_corpus.py` (issue #140) — see its module docstring and
  `Docs/TestDesign.md`'s `[uci]` section for reuse by future behaviour-preservation refactors.
- **Node-count identity (secondary evidence)**: five positions spanning game stages, `go depth 6`
  via UCI with `threads=1` (Lazy SMP is nondeterministic and would mask the signal), searched with
  the pre- and post-restructure binaries. Node counts and best moves match exactly at every
  position.
- **No ELO match**: score and node-count identity are strictly stronger evidence than an ELO batch
  for a pure refactor — there is nothing an ELO match could show that identity at every node
  doesn't already prove, and a fixed-size batch can't resolve a true zero-Elo delta anyway.

Plan: `.claude/plans/eval-context-restructure.md`.

## 2026-07-27 — UCI `eval` Command + Batch FEN Scoring, Phase 1 (issue #129)

### Added
- UCI `eval` command (`UciHandler::cmd_eval()`, `StratEngine/UCIHandler.cpp/h`): prints the
  static evaluation of the current position (whatever `position` last set up, or the default
  empty board if none has run yet) as two lines — the raw side-to-move-relative score, and the
  same score restated from White's point of view — so the sign convention doesn't have to be
  read out of source. Not a search response: emits neither `info` nor `bestmove`, so a GUI
  cannot mistake it for one. `UciHandler` now owns its own `EvalManager` instance
  (`EvalTypes::COMPLEX`, matching what `init_ai()` configures for search), constructed once in
  the constructor rather than re-created by `init_ai()`/`ucinewgame()` — the evaluator holds no
  per-game state to reset.
- CLI batch mode: `StratChessEvolved.exe eval <path-to-fen-file>` (`evalrunner()`,
  `StratChessEvolved/StratChessEvolved.cpp`) scores one FEN per line and prints
  `<fen>\t<score>` to stdout — nothing else on stdout, so the output is directly consumable by
  a corpus-scale tool. **The printed score is the raw, side-to-move-relative value
  `EvalManager::Evaluate()` returns, with no sign transformation** — this is deliberate: it's a
  single source of truth with what the search calls, and it's exactly what #127's planned
  before/after byte-identity check needs to diff. A consumer that wants a White-relative score
  already has the side-to-move field parsed out of the FEN and can flip the sign itself. Blank
  lines and `#`-comment lines are skipped silently; a line with fewer than 4 space-separated
  FEN fields (placement/side-to-move/castling/en-passant — halfmove/fullmove are optional) is
  rejected with a line-numbered warning to stderr and the run continues (issue #46: a missing
  side-to-move field otherwise silently defaults to Black-to-move — for a large tuning corpus
  that's garbage fitted silently). Lines that clear that floor but are still rejected by
  `FENParser::ParseFEN` are likewise warned and skipped rather than silently scoring a
  still-default (empty) `Board`.
- `StratChessTests/UCITests.cpp` `[uci]` cases: `eval` works before any `position` command;
  the printed score is asserted equal to a direct `EvalManager::Evaluate()` call on the same
  FEN (the "honesty invariant" that makes the tool trustworthy for #117/#127 — proves `eval`
  never computes a parallel score); output contains neither `bestmove` nor `info`; the
  White-POV line matches the stated sign convention on two whole-rook-imbalance positions (one
  White-to-move, one Black-to-move) so a sign bug can't hide behind a near-zero score.

### Notes
- **Scope**: `StratEngine/Eval.cpp`/`Eval.h` are untouched — deliberately. Per-term score
  breakdown is phase 2 of #129, gated on #127 (`EvalContext` restructure) landing first;
  reimplementing per-term extraction ahead of that would duplicate `Evaluate()`'s internals and
  risk drifting out of sync with the real evaluator. See
  `.claude/plans/uci-eval-command-term-breakdown.md` (D3).
- No ELO match: this changes no score, only adds introspection.

Plan: `.claude/plans/uci-eval-command-term-breakdown.md`.

## 2026-07-27 — Rook Open-File Definition Fix (issue #126)

### Fixed
- `EvalComplex::Evaluate` (`StratEngine/Eval.cpp`) tested the rook's open-file bonus against
  `all_black`/`all_white` — any enemy piece on the file, including a knight, bishop, queen, or
  the enemy king — instead of enemy pawns only. The standard definition of "open file" is no
  pawns of either color; an enemy piece sharing the file is usually a target for the rook, not a
  reason to demote the bonus. Narrowed both rook cases to `black_pawns`/`white_pawns`.
- Removed the now-dead `all_white`/`all_black` locals (only consumers were the two sites above)
  and the two commented-out `else if` branches referencing a `pBitBoards` variable that no longer
  exists (D6 in `.claude/plans/passed-and-backwards-pawn-terms.md`).

**Net behavioural delta**: a rook whose file contains an enemy non-pawn piece (including the
enemy king) but no pawns of either color now scores `OPEN_FILE - HALF_OPEN_FILE` (5 cp) higher
for that rook's side than before. Positions where the file already contained an enemy pawn, or
was genuinely clear of everything, are unchanged. The existing own-pawn-behind-the-rook behaviour
(D5) is deliberately kept as-is and now documented in a comment at the call site.

**Measurement**: validated with `Run-EloMatch.ps1 -Sprt NonRegression`, per CLAUDE.md's rule that
anything expected to be worth less than ~25 Elo needs an SPRT to be decidable at all — a
fixed-length 500-game batch resolves only ±25 Elo here, so its interval would contain zero either
way. The `NonRegression` preset (`elo0 = -5, elo1 = 0`) asks "did this hurt?", which is the right
question for a correctness fix.

Result: **inconclusive** — 500 games, `+23.66 +/- 25.70` Elo, LLR 0.76 of ±2.94, 204W/170L/126D
(53.40%), LOS 96.51%. The SPRT hit its game cap without crossing a bound. Read this as **no
regression detected**, not as a +23.66 Elo gain: the 95% interval is [-2.04, +49.36] and still
contains zero. A 5 cp static-eval shift was never expected to be resolvable at this sample size,
and deciding it either way would need a far larger budget than a correctness fix justifies. See
`Docs/EloLog.md` for the full row. The `[eval]` regression tests and a clean extended-test run
(including the full tactical tiers) remain the primary evidence.

### Added
- `StratChessTests/EvalTests.cpp`: an enemy knight sharing the rook's file no longer demotes an
  open file (fails on pre-fix HEAD by exactly 5 cp); an enemy pawn on the file still demotes it
  to half-open (guard); an own pawn behind the rook still leaves the file half-open/open (pins
  D5). The knight-on-file and pawn-on-file positions were also added to the #125 color-symmetry
  case list.

## 2026-07-26 — Eval Color Symmetry Fix (issue #125)

### Fixed
- `EvalManager::getEvalBoard` (`StratEngine/Eval.h`) mapped a Black piece's square with
  `63 - square`, a 180-degree rotation that mirrors files as well as ranks, instead of the
  intended vertical flip (`square ^ 56`). Invisible for file-symmetric PSTs, but wrong for
  file-asymmetric ones — a prerequisite for future terms like king-side castling/pawn-storm
  bonuses (epic #110) and for #117 (Texel tuning), which needs a symmetric evaluator to fit
  against.
- Queen PST (`StratEngine/defines.h`) had one asymmetric pair (c6 = 4, f6 = 3) that made the
  above defect observable; f6 corrected to 4.

**Net behavioural delta**: because all seven PSTs are file-symmetric once the queen typo is
fixed, a 180-degree rotation and a vertical flip produce identical values for every square —
so the `getEvalBoard` change on its own is exactly score-identical to the previous behaviour.
The entire scoring change is the one corrected table cell, enumerated exhaustively: a **White
queen on f6** and a **Black queen on c3** each gain 1 cp. Every other position is byte-identical.
No ELO match was run: 1 cp on a single PST cell is orders of magnitude below the instrument's
±25 Elo resolution and is directionally unbiased (it applies equally to both colors), so a
500-game batch would cost an hour to return a number indistinguishable from zero. The
color-symmetry tests plus a clean extended-test run are the stronger evidence here (see
`.claude/plans/eval-color-symmetry-and-queen-pst-fix.md`).

### Added
- `StratChessTests/EvalTests.cpp`: direct `getEvalBoard` mirroring cases via a test-local
  `EvalProbe` subclass, plus whole-position color-symmetry cases (`Evaluate(fen) ==
  Evaluate(MirrorFen(fen))`, both evaluators) using a new file-local FEN color-mirror helper.

## 2026-07-26 — Validation Scoped to the Diff (issue #124)

### Added
- `Scripts\Get-ChangeTier.ps1` — classifies a diff into `Docs` / `Tooling` / `Build` / `Engine`,
  strictest-wins on mixed diffs. Single source of truth, shared by `Validate-PrePR.ps1` and
  `.github/workflows/build-and-test.yml`. Ships with `-SelfTest` (18 assertions).
- CI `classify` job gating `build-and-test`, plus a `build-and-test-result` summary job so branch
  protection can require one always-present check that passes on a legitimate skip and fails on a
  real failure.

### Changed
- `Scripts\Validate-PrePR.ps1` now scopes itself: `Docs` exits immediately, `Tooling` runs a
  PowerShell syntax parse, `Build`/`Engine` run the existing four gates unchanged. `-Force` runs
  everything; `-BaseRef` overrides the diff base. The detected tier and deciding file are printed.
- `CLAUDE.md` pre-PR checklist step 2 replaced with the tier table — the judgement call is gone,
  the answer is now "just run the script".

### Why
Two real cases: PR #123 (one-line `CLAUDE.md` edit) burned a full ~6 min CI cycle, and PR #133
(SPRT support, a measurement-script change) ran a full build + extended `[slow]` tests + a 10-run
tactical stability suite + self-play. None of those gates can observe a script that is never
compiled and never invoked by the engine.

### Notes
- **Fails closed**: the default rule is `Engine`. There is no "else → cheap" branch — an
  unrecognised path costs time rather than skipping validation. Asserted in `-SelfTest`, including
  a case for a newly-added script under `Scripts/` (classifies `Engine` until deliberately listed).
- **No self-exemption**: `Validate-*.ps1`, `Get-ChangeTier.ps1` and `build.ps1` are `Build` tier.
  If a change to them could take its own shortcut, a classifier bug would be self-concealing —
  disabling validation and then declining to validate the change that disabled it.
- **Pushes to `main` always run the full CI job**, deliberately: that run also warms the
  per-branch `actions/cache` deps cache, so skipping it on docs-only pushes would make the next
  code PR pay a cold-cache build.
- Classification lives in its own CI job because it needs `fetch-depth: 0` to diff; `build-and-test`
  keeps its cheap shallow checkout.

### Files
- `StratChessEvolved/Scripts/Get-ChangeTier.ps1` (new), `Scripts/Validate-PrePR.ps1`,
  `.github/workflows/build-and-test.yml`, `CLAUDE.md`
- Plan: `.claude/plans/validation-change-tiers.md`

---

## 2026-07-26 — SPRT Support in Run-EloMatch.ps1 (issue #130)

### Added
- `Scripts\Run-EloMatch.ps1` gains `-Sprt` (`NonRegression` / `Gain` / `Custom`), plus `-Elo0`,
  `-Elo1`, `-Alpha`, `-Beta` and `-SprtModel`. A sequential test stops as soon as the result is
  decisive instead of always playing `-Games` games; `-Games` becomes an upper bound.
- The SPRT verdict (`H0 accepted` / `H1 accepted` / `inconclusive @ N games`) is recorded in the
  `Docs/EloLog.md` row alongside the Elo point estimate, and a new
  "Choosing SPRT vs a fixed batch" section documents when to use which.

### Why
The instrument could not resolve most of what epic #110 asks it to validate. `Docs/EloLog.md`
measures 500 games ≈ **±25 Elo** for this engine, while the remaining eval terms (bishop pair,
connected rooks, castling-done, outposts, queen activity) are each worth roughly 5-20 Elo — inside
the noise floor. The mop-up measurement (`+15.94 ± 27.62`) is the existing demonstration: a result
equally consistent with "+16 Elo", "no change", and "−10 Elo". Prerequisite for #110 Tiers 3-4;
replaces the completed #119 at the head of that epic.

### Notes
- `model=logistic` is pinned so `-Elo0`/`-Elo1` mean literal Elo, the same scale `Docs/EloLog.md`
  reports everywhere. fastchess's own default is `normalized` (nElo) — a different scale on which
  `elo1=10` would silently mean something else.
- The verdict/LLR parse patterns were derived from the actual output of the pinned fastchess 1.8.0
  build, not from documentation: a pattern matching nothing would degrade every run to
  "inconclusive", a failure mode that looks like a result.
- An inconclusive run is recorded as a first-class outcome and flagged in the console — it means
  "smaller than elo1, or out of budget", never "measured zero".
- Validated by exercising all parameter-rejection paths, a real SPRT match that terminated early on
  an H0 acceptance, and a non-SPRT regression run confirming the default path is unchanged.

### Files
- `StratChessEvolved/Scripts/Run-EloMatch.ps1`, `Docs/EloLog.md`, `CLAUDE.md`
- Plan: `.claude/plans/elomatch-sprt-support.md`

---

## 2026-07-26 — ELO Match Concurrency Default Raised to 6

### Changed
- `Scripts\Run-EloMatch.ps1`'s `-Concurrency` default raised from 4 to 6 for the current dev
  machine (12 physical cores / 24 logical threads) — sized off physical cores ÷ 2 single-threaded
  engine processes per game, not logical/SMT thread count, to avoid oversubscribing physical
  execution resources (which can introduce timing noise into fixed real-time-control measurements)
  while still meaningfully cutting wall-clock time per match. Documented in `Docs/EloLog.md`'s
  pinned measurement setup table alongside the Machine row it depends on.

## 2026-07-26 — ELO Match Resume Support (issue #119)

### Added
- `-ResumeDir`/`-AutosaveInterval` parameters on `Scripts\Run-EloMatch.ps1`, letting an
  interrupted match resume from fastchess's own autosaved checkpoint (`config.json`) instead of
  restarting the full batch — discovered as a prerequisite during #70's validation, where a
  500-game match was killed by a background-task duration cap at 491/500 games after ~60 minutes
- Fixed `Docs/EloLog.md`'s recorded game count to come from fastchess's own final `Games: N`
  summary rather than the (in resume mode, meaningless) `-Games` parameter
- Validated: a real kill-and-resume test (interrupted after 2/20 games, resumed via `-ResumeDir`)
  correctly continued from game 3 with no replay/duplication, finishing at exactly 20/20

## 2026-07-26 — Mop-Up Evaluation for Won Pawnless Endgames (issue #70)

### Added
- Mop-up evaluation term in `EvalComplex::Evaluate` giving the engine a gradient toward
  converting decisively-won pawnless endgames (e.g. KQ vs KR), driving the losing king
  toward a corner once material lead clears `MOPUP_MATERIAL_THRESHOLD`; `Tactical -
  QFORK-001` (issue #66 regression test) hidden via Catch2 `[.]` tag pending a broader
  WAC-style tactical suite — see issue #118
- Validated: build clean (Level4/`/WX`, 0 warnings); full extended suite 174/174 passing
  (`Tactical - QFORK-001` intentionally hidden, see above; exe tactical suite now 30/31,
  see `Docs/TestDesign.md`); 1 self-play game to checkmate (move 217), no crash; ELO
  +15.94 ± 27.62 over 491/500 games (partial batch, see `Docs/EloLog.md`)

## 2026-07-23 — Lazy SMP Parallel Search (PR #109)

### Added
- Multi-threaded search for `AIPerplex`: `GetMove()` at `threads_ > 1` spawns
  `threads_ - 1` helper `std::jthread`s (`AIPerplex::helper_loop`) that run a plain
  iterative-deepening loop from depth 1, sharing the transposition table with the main
  search thread; main thread's result stays authoritative (helpers never report moves,
  no depth-skip patterns, no voting) — best move propagates through TT timing alone
- `PlayerAiBase::SetThreads(unsigned)` (virtual no-op on legacy AIs); `AIPerplex::threads_`
  clamped to `[1, 32]`; UCI `setoption name Threads value N` (advertised via
  `option name Threads type spin default 1 min 1 max 32`); `game_settings.json` per-player
  `"threads"` key (default 1, unchanged by this PR — flipping it to the measured-best
  value for actual play is a post-merge, user-decided follow-up)
- `nodes_since_check_` moved from `PlayerAiBase` into `ThreadData`, gated to
  `thread_id == 0` for clock checks; helpers rely solely on the existing `IsAborted()`
  relaxed-atomic check (no clock calls off the main thread)
- `tactical stability N [file] [threads]` CLI form (`StratChessEvolved.exe`) — forwards a
  threads arg through `TacticalTestRunner::run_stability_suite` → `run_test_suite` →
  `run_position`; the pre-merge nondeterminism detector for once threads race on a shared
  TT (byte-identical node-count equivalence stops being available past `threads=1`)
- `Scripts\Run-EloMatch.ps1`: `-CandidateOptions`/`-ReferenceOptions` (arbitrary fastchess
  per-engine UCI option tokens, e.g. `option.Threads=4`) and `-ReferenceExe` (point the
  reference side at an explicit exe instead of always rebuilding from a pinned git tag) —
  needed to measure the same binary against itself under different `Threads` settings

### Fixed
- UCI `Threads` option no longer resets on `ucinewgame`: `UciHandler::cmd_ucinewgame()`
  rebuilds `ai_` from scratch via `init_ai()`, which previously left a fresh `AIPerplex`
  defaulted to `threads_ == 1` — silently discarding any prior `setoption name Threads
  value N` under standard UCI usage (`setoption` once at session start, `ucinewgame` before
  every game), making `Threads` effectively non-functional. Cost real time during this PR's
  own NPS measurement (below) before a probe comparing total nodes at threads=1 vs threads=4
  under a fixed `movetime` caught it. Fixed via `UciHandler::configured_threads_`, set by
  `cmd_setoption()` and reapplied by `init_ai()` on every call (initial `run()` startup and
  every `ucinewgame`); regression-tested in `UCITests.cpp` (`[uci][smp]`).

### Three-gate validation
- **Gate 1 (inert at threads=1)**: byte-identical node/move/score/depth sequence vs the
  pre-SMP baseline — the single-threaded path never touches any thread machinery
- **Gate 2 (stable at threads=4)**: `tactical stability 20 tactical_test_cases.json 4` —
  20/20 runs, 31/31 positions each run, 0 failing runs, 0 flipped positions;
  `Validate-PrePR.ps1` full pass (extended Catch2 tiers, deep perft 640/640, AIAgent
  self-play unaffected — legacy AIs stay single-threaded)
- **Gate 3 (measured gain)**: `Run-EloMatch.ps1`, same binary, `option.Threads=4` vs
  `option.Threads=1`, 500 games @ 10+0.1: **Elo +128.55 ± 28.36, LOS 100.00%**
  (286W/109L/105D, 67.70%) — comfortably clears the positive-score/LOS>95% merge bar.
  Recorded in `Docs/EloLog.md`'s history table.

### NPS scaling (31-position tactical suite, fixed depth 8, driven directly over UCI)

| Threads | Total nodes | Total time (ms) | Aggregate NPS | Scaling vs 1T |
|---|---|---|---|---|
| 1 | 1,534,954 | 1,281 | 1,198,247 | 1.00x |
| 2 | 2,702,646 | 1,122 | 2,408,775 | 2.01x |
| 4 | 5,224,431 | 1,186 | 4,405,085 | 3.68x |
| 8 | 8,901,291 | 1,283 | 6,937,873 | 5.79x |

Depth 8 finishes fast per position (25–105 ms at threads=1), and `go depth N` is
depth-limited rather than time-limited, so total wall time stays roughly flat across
thread counts while combined node throughput scales — more threads do more work in the
same wall time rather than finishing sooner. That is a real but secondary signal; Gate 3's
ELO match is what actually measures the production benefit (better move quality per unit
of *game clock* time). Sub-linear scaling past 2 threads matches Lazy SMP expectations
(shared-TT search overlap between helpers, diminishing marginal value per added thread,
plus physical core count capping it).

Plan: `.claude/plans/lazy-smp.md`. `Docs/TestDesign.md` documents the `tactical stability`
threads arg.

### Follow-ups (deferred; not filed as GitHub issues yet — pending triage)
- **Lockless TT (Hyatt XOR)**: replace the TT's per-bucket `shared_mutex` with
  self-validating atomic entries; own PR, gated on measured NPS scaling + a
  non-regression ELO match. v1 deliberately keeps per-bucket locks — already thread-safe,
  and the pre-SMP single-threaded search already paid the lock cost, so it's a
  zero-regression starting point
- **Persistent thread pool**: current design is spawn-per-search (`GetMove()` spawns
  `threads_ - 1` helper `std::jthread`s and joins them before returning); only worth
  revisiting if profiling ever shows thread-spawn cost mattering against seconds-per-move
  search budgets
- **`game_settings.json` `"threads"` flip**: post-merge, user-decided — flip both
  players' `"threads"` from 1 to the measured-best value (4, per this measurement) to
  actually play with Lazy SMP enabled

## 2026-07-04 — Roadmap → GitHub Issues migration (PR #105)

### Changed
- Active backlog (all open Roadmap items) migrated to GitHub Issues under a new label
  taxonomy: `category:*` (search/eval/test/elo/infra/refactor/build-tooling),
  `priority:*` (critical/high/medium/longterm), `status:incoming` (new idea, awaiting a
  triage sweep), `type:epic`, `low-hanging-fruit`
- Native GitHub sub-issues used for the Build System Modernization epic (#81 → #82-84,
  #92); native "Blocked by" links used for real dependencies (#83, #84, #92 blocked by
  #82)
- `Docs/Roadmap.md` gutted to principles + this changelog; nothing lost — the two items
  that lived in now-deleted sections (Near-Term Sequence outcome, GetMove SearchLimits
  refactor) were promoted to changelog entries first

## 2026-07-04 — GetMove SearchLimits Refactor (PR #80)

### Changed
- Every `GetMove()` call now takes an explicit `const SearchLimits&`
  (`StratEngine/SearchLimits.h`: clock/movetime/depth/infinite), resolved via a pure
  `Engine::resolve_limits()` function; `PlayerAiBase::ApplyLimits()` replaces
  `StartTimer()` + the `clock_info_set_` flag dance
- `SetClockInfo()` deleted entirely; `UCIHandler::cmd_go` and `Game::SetPlayerParams`
  both build a `SearchLimits` and pass it per call instead of pre-configuring AI state —
  removes the pre-call setter-ordering contract a Lazy SMP helper thread could otherwise
  violate
- `game_settings.json` migrated to a `"search_limits"` block; legacy
  `max_depth`/`time_limit` keys still work via a fallback with a one-time deprecation
  warning
- Delta from the original roadmap sketch (a `GameInfo`-field `TimeControl` struct):
  rejected because `GameInfo` is copied into `info_seq` at every search ply

### Fixed
- `search-reviewer` caught a real gap in the legacy-config fallback (a `max_depth`-only
  config would have silently gotten an unbounded 1-hour search instead of the old 15s
  cap) — fixed before merge

Validated: byte-identical fixed-depth self-play node counts vs. pre-refactor baseline,
AIAgent self-play regression (base classes changed), full `Validate-PrePR.ps1` gate, UCI
smoke tests across all `go` modes. Plan: `.claude/plans/getmove-searchlimits-refactor.md`.
**Unblocks**: Lazy SMP — no remaining refactoring blockers.

## 2026-07-03 — ELO Baseline Measurement (PR #75)

### Added
- `Scripts\Run-EloMatch.ps1`: one-command differential strength measurement — candidate
  build vs. pinned reference (`elo-reference-v1` tag), fastchess v1.8.0, 250 committed
  openings (color-swapped pairs), 10+0.1 TC, adjudication, per-engine working dirs;
  auto-rebuilds the cached reference exe from its tag via a temp worktree on cache miss;
  appends every result to `Docs/EloLog.md`

### Fixed
- Games longer than `MAX_PLY` (256) plies overflowed the ply-indexed history arrays
  during UCI `position` replay (Release access violation) — found on first contact with
  a real match runner. `cmd_position` reset the undo cursor only after the whole replay
  loop; fixed by per-move `ResetSearchDepth()` (matching `Game.cpp`); issue #53
  follow-up; 300-ply regression test via new `UciHandlerTestFixture` (`[uci]`)

Sanity baseline: identical builds (SHA256-verified) pooled −1.4 ELO over 2×500 games — no
instrument bias; measured per-batch noise ±25 ELO at this draw ratio. Plan:
`.claude/plans/elo-baseline-measurement.md`; full setup/interpretation: `Docs/EloLog.md`.

## 2026-07-03 — Extract ThreadData Structure (PR #74)

### Changed
- All per-search mutable state used by `AIPerplex` now lives in a single `ThreadData`
  struct (`StratEngine/ThreadData.h`): thread-local `Board` copy, node counter,
  `PVTable`, `GameInfo` sequence, killers, history, null-move flags — plus the
  maintenance methods that operate on them
- `ThreadData&` passed explicitly as the **first** parameter through the whole search
  call chain (`iterative_deepening` → `search_with_aspiration` → `pvs` → `quiescence`
  and helpers); `TranspositionTable` stays a separate explicit parameter (shared across
  threads under Lazy SMP)
- The search runs entirely on `td.board`; the only remaining side effect on the real
  board — root game state — is propagated back explicitly after the search returns
- `PlayerAiBase::StopTimerAndAdjustVars(size_t node_count)` takes the node count
  explicitly; legacy AIs (`AIBasic`/`AIAgent`/`ABIterative`) pass `m_SearchCount`

Validated: byte-identical move/score/depth/nodes sequence across a full 137-move
fixed-depth self-play game vs. pre-refactor baseline; deep perft 640/640; all Catch2
tiers + exe tactical suite 31/31. Plan: `.claude/plans/extract-threaddata-structure.md`.
**Unblocks**: Lazy SMP, and the "with ThreadData extraction" C++23 slice (`std::mdspan`).

## 2026-07-02 — Near-Term Sequence before Lazy SMP (PR #71, #72)

### Changed
- Ordering agreed after the issue #66 post-mortem (PR #71): (1) tactical suite
  expansion; (2) Extract ThreadData Structure; (3) ELO baseline + deferred-suite scope
  revisit before Lazy SMP
- Step 1 implemented same day (PR #72): WAC mate-in-2/3/4 + non-mate tactical wins,
  8 → 31 gated positions, 100%-mate-category pass policy
- **Stability mode** (`tactical stability N`) adopted as the pre-SMP correctness
  artifact — runs the gated suite N consecutive times, fails on any per-run gate failure
  or any position flipping pass/fail between runs; gated at N=10 in
  `Validate-PrePR.ps1` Step 3. Chosen because once Lazy SMP threads race on a shared TT,
  byte-identical node-count equivalence stops being available, so a flakiness detector
  was needed before that point
- BT2630/ECM-GCP tactical suite additions deferred until deeper search (SEE/futility
  pruning); endgame tablebase positions scheduled alongside future eval progress work

## 2026-07-02 — NMP Single-Piece Zugzwang Guard (PR #69, issue #66)

### Fixed
- QFORK-001 (`8/8/8/3r4/4k3/8/8/3QK3 w`, KQ vs KR) regressed to 7/8 on the exe tactical
  suite when null-move pruning landed: the zugzwang guard only refused NMP for a side
  with *zero* non-pawn material, so a lone-rook side could "pass," hiding the
  domination/zugzwang rook win. `should_try_null_move()` now requires ≥ 2 non-pawn
  pieces for the side to move
- Verification gap closed: the exe tactical suite (never run by any automated gate — how
  the 7/8 went unnoticed) now runs in `Validate-PrePR.ps1` Step 3; QFORK-001 mirrored as
  a Catch2 `[tactical]` case

Plan: `.claude/plans/nmp-single-piece-zugzwang-guard.md`.

## 2026-07-02 — De-Singleton Board (PR #67)

### Changed
- `Board::Instance()` singleton accessor removed entirely; `Board` is now an ordinary
  constructible, copyable value type
- `MoveGenerator` and `EvalManager::Evaluate` take an explicit `const Board&` parameter
  instead of reaching for the singleton
- `PlayerBase::Create()` and every player constructor take `Board&` by injection
- `Game` and `UciHandler` each own their own `Board board_` member; `Config`'s FEN/board-
  setup methods take an explicit `Board&`
- All Catch2 test files construct their own local `Board` — no more shared global board
  state between `TEST_CASE`s; three dead legacy test headers retired
- `zobrist::initialize()` changed to a thread-safe run-once magic static, so two boards
  built from the same FEN are guaranteed to hash identically
- `GetBitBoards()` changed to a `const` accessor returning `std::span<const BITBOARD>`

Validated: perft 640/640 with identical node counts at every phase; self-play byte-
identical vs. pre-refactor baseline through both `PlayerAI`/`PlayerBase` hierarchies.
Plan: `.claude/plans/de-singleton-board.md` (7 phases). **Unblocks**: Extract ThreadData
Structure, and the "with De-Singleton Board" C++23 item (`std::expected`).

## 2026-06-20 — Decouple `Board::currentPly_` from Game Length (PR #57, issue #53)

### Fixed
- `currentPly_` indexes four fixed `MAX_PLY=256` ply-history arrays but was never reset
  after a permanently-committed move, so it grew with total game length while search
  recursion added depth on top — overflowed the arrays after ~250 real moves (self-play
  access violation around move 247-249)
- `Board::ResetSearchDepth()` added, called after every permanent move commit, so
  `currentPly_` only ever spans in-flight search recursion depth, never game length
- `assert(currentPly_ < MAX_PLY)` guard added in `DoMove`/`UndoMove` as defense in depth

## 2026-06-20 — Null-Move Pruning (PR #55)

### Added
- `tuning_.null_move_enabled` defaults to `true`; guard helper `should_try_null_move()`
  centralises every condition: zugzwang (no non-pawn material), mate-score
  contamination, consecutive-null, PV/in-check, min-depth

### Fixed
- `Board::DoNullMove()` wasn't forfeiting en-passant rights (zobrist/EP desync)
- `PlayerAiBase::m_infoSeq` wasn't sized for null-move plies (out-of-bounds crash the
  first time NMP recursed in a real self-play game, not caught by unit tests alone)

Plan: `.claude/plans/null-move-pruning.md`.

## 2026-03-14 — UCI Protocol (PR #42)

### Added
- `UCIHandler` class: synchronous command loop with search on `std::thread`. Commands:
  `uci`, `isready`, `ucinewgame`, `position` (startpos/fen/moves), `go`, `stop`, `quit`.
  Time control: `movetime`, `wtime`/`btime`/`winc`/`binc`/`movestogo`, `depth`,
  `infinite`. `AIPerplex::GetLastResult()` exposes `SearchResult` for the post-search
  `info` line. `UCIHandler` moved to `StratEngine/` per `CLAUDE.md` structure.
- 8 `parse_go` `[uci]` test cases (324 total assertions, all pass)

### Fixed
- `movetime 5000` was completing in 11s+ instead of ~5.3s — three root causes: hard
  limit was 3× soft (reduced to 1.5×, so opening moves no longer consume the full hard
  budget); no fast-exit path on timeout (`IsAborted()` latch + check added to `pvs()`
  and `quiescence()`, collapsing the stack in O(depth) instead of O(tree_size)); the
  "move changed" soft-limit extension had no cap (new `extra_depth_used` flag limits it
  to exactly one extra depth per search)
- `std::cout` fallback removed from `StopTimerAndAdjustVars()`; `SetVerboseLogging(true)`
  removed from the `AIPerplex` constructor (verbose logging is now opt-in per call site)

Validated: pipe-based functional smoke test; `go movetime 5000` completes in ~5.3s;
tested in CuteChess GUI (human vs. engine and engine vs. engine).

## 2026-03-12 — Time Management: Clock-Aware Soft/Hard Limits (PR #41)

### Added
- `Engine::compute_budget(remaining, increment, moves_to_go)` free function in
  `TimeUtils.h/cpp` — pure math, independently testable; formula:
  `soft = usable/horizon + inc*80%`, `hard = min(soft*3, usable/2)`
- `TimeManager` gains a two-arg `start(soft, hard)` overload +
  `should_stop_iteration()` (soft-limit gate); one-arg `start(allocated)` delegates
  unchanged for backward compatibility
- `PlayerAiBase::SetClockInfo()`: computes the budget and arms the timer;
  `clock_info_set_` flag prevents `StartTimer()` from overwriting clock-aware budgets
  (this method and flag were later superseded and deleted by the GetMove SearchLimits
  refactor, 2026-07-04)
- Node-based time polling in `pvs()` every 1,024 nodes (amortises `chrono::now()`
  overhead on deep searches)
- `[time_mgr]` test tag: 10 assertions (6 formula, 4 timing)

Plan: `.claude/plans/time-management-clock-aware.md`.

## 2026-03-10 — Late Move Reductions + Move Sorting Extraction (PR #38)

### Added
- **Late Move Reductions**: sqrt formula
  `R = min(max(1, sqrt(depth-1) * sqrt(si-1)), depth-1)`, applied to quiet/non-killer/
  non-evasion/non-PV-node moves (si≥3, depth≥3); 2-step re-search. Kill-switch
  `tuning_.lmr_enabled`. Observed depth 13-15 vs 8-9 (without LMR) in the same
  15-second budget; ~31M vs ~36M nodes/move. 10 `[search]` test cases via
  `AIPerlexTestFixture`. Plan: `.claude/plans/lmr-and-search-tests.md`.

### Changed
- **Move Sorting**: inline move scoring loop extracted from `pvs()` into
  `MoveSorter::ScoreMoves()` static method; precondition asserts + `isKiller1`
  short-circuit for fast killer detection. 5 `[sort]` test cases, 14 assertions.
  Plan: `.claude/plans/move-scoring-extraction-and-sort-tests.md`.

## 2026-03-08 — spdlog Level Gate + outLegalMoves Removal (PR #34)

### Changed
- 3-line per-call logging boilerplate in `AIPerplex` replaced with a spdlog level gate
  (`s_logger->set_level(...)`)
- Global `outLegalMoves` stream removed; board/root-move diagnostics now flow through
  the default spdlog logger at `debug` level
- `Board::test_bitboards` signature simplified

Plan: `.claude/plans/logging-spdlog-gate-and-outlegalmoves-removal.md`.

## 2026-03-07 — Aspiration Windows, C++20 Adoption, PCH Expansion, SearchTuning exposure (PR #29, #30, #31, #32)

### Added
- **Aspiration Windows in Iterative Deepening** (PR #30): narrow alpha/beta window
  around the previous depth's score, gradual widening (25cp → 75cp → full) on
  fail-high/fail-low. Kill-switch `tuning_.aspiration_enabled`;
  `search_with_aspiration()` extracted into its own method.
- **Expose SearchTuning via game_settings.json** (PR #31): all 8
  `AIPerplex::SearchTuning` parameters (including the `barelySearched`/
  `probablyIncomplete`/`pvTooShort` thresholds) exposed as a `search_tuning` JSON
  block, parsed into `Config::SearchTuningConfig`; absent block leaves AIPerplex
  defaults untouched. Also consolidated `m_MaxDepth`/`max_depth_` into a single
  canonical depth field across all AI types, and added a `time_limit` JSON key.

### Changed
- **C++20 Adoption — `<bit>` and `<format>`** (PR #32): `std::countr_zero` replaces
  `_tzcnt_u64` `#ifdef` in `Board::GetFirstPiece`; `std::format` replaces
  `std::stringstream` in `Move::Output()`. C++23 upgrade path documented in
  `.claude/plans/cpp23-upgrade.md`.
- **Expand PCH Coverage in StdAfx.h** (PR #29): 9 STL headers added; redundant per-TU
  includes removed from 7 source files. Zero warnings enforced (`/WX`).

## 2026-03-04 — Introduce MoveFormatter (PR #26)

### Added
- Stateless class centralizing move presentation in
  `StratEngine/MoveFormatter.h/cpp`: `ToShort` (pseudo-LAN + `+` check annotation),
  `ToVerbose` (verbose English), `ToUCI` (wire format), `FromUCI` (parse from board
  pre-`DoMove` state)
- Fixed gaps: verbose line restored; `+` now in `gamelist.txt`; fragile `\n`-surgery in
  `PrintBoardAndMove` removed; promotion-captures get a suffix in perft divide output
- 65 assertions, 6 test cases (`[formatter]`)

`Move::Output()`/`Move::Output(ePiece)` kept as-is (callers migrated separately — see
the "Migrate Move::Output() callers" issue). `ToSAN` omitted, deferred until PGN export
is needed. Plan: `.claude/plans/move-formatter.md`.

## 2026-03-03 — Move class → 16-bit layout, Phases 3-4 (PR #24)

### Changed
- **Phase 3**: Removed `MovPiece` field — `Board::GetEffectiveMovPiece()` added;
  `MoveFactory` drops the movPiece param; `MoveHelper`/`GameState`/`Sort` thread it
  explicitly instead; a Zobrist hash corruption bug in `UndoMove` fixed along the way
- **Phase 4**: Removed `Content` (captured-piece) field — `sizeof(Move) == 2` enforced
  via `static_assert`; 4 `PROMOTION_*_CAPTURE` MoveType variants added (capture bit 2 +
  promotion bit 3); `Board::GetCapturedPiece()` public API added; `MoveFactory` drops
  the captured param; `Move::Value`/`MoveHelper::IsValid`/`IsPieceCapturedAt` gain an
  explicit `content` param; `IsCapture`/`IsPromote` simplified to pure flag-bit tests

`BoardTests.cpp` added (6 `[board]` test cases); all 47 tests pass.

## 2026-03-01 — Phase 0 Test Coverage (PR #18)

### Added
- `[tt]` — TranspositionTable unit tests (store/probe, mate normalization, replacement,
  counters)
- `[eval]` — Evaluation position tests (symmetry, material advantage, doubled pawns,
  rook bonus)
- `[tactical]` — Fast search regression tests (mate-in-1, hanging piece capture via
  AIPerplex depth 4)
- `STRAT_ENABLE_TEST_ACCESS` friend stub added to `AIPerplex.h` for future Phase 1
  search tests

## 2026-03-01 — Restrict Board Piece-Setup API to Private (PR #21, commit 8bef567)

### Changed
- `ClearBoard`, `SetInitialColor`, and `AddPieceToBoard` moved to `private:` — no longer
  called from test code after PR #20; `SetupFromFEN` is the sole public board-setup API

## 2026-02-28 — Move class Phases 1-2 + Catch2 v3 migration (PR #12, #16)

### Changed
- **Move class → 16-bit layout, Phases 1-2** (PR #12): removed `Move::IsCheck` field
  (Phase 1); removed `[from|to]IsNoSquare` fields (Phase 2)
- **Migrate to Catch2 v3** (PR #16): 2-file amalgamated drop-in, no pre-build step
  required; test project `StratChessTests/StratChessTests.vcxproj` rebuilt from empty
  placeholder; tests migrated: `RepetitionTests.cpp` (TC1-TC7, TC9), `MoveFieldTests.cpp`,
  `PerftTests.cpp`; retired `TestFramework.h`, `Unittests.h`, `Perft_unittests.h`; tags
  `[repetition]`, `[moves]`, `[perft]`

## 2026-02-27 — Delta Pruning in Quiescence (PR #11)

### Added
- `tuning_.delta_pruning_margin = 200` added to `SearchTuning`; guard skips captures
  where `stand_pat + piece_value + margin < alpha` (not in check, not promotion)

Consistently deeper search — mate at depth 14 observed where it wasn't reached before.

## 2026-02-25 — Archive Broken Algorithms (PR #9)

### Removed
- `ABIterTrans.cpp/h` and `AITrans.cpp/h` moved to `StratEngine/Archived/`,
  `Archived/README.md` explains historical context, both removed from the build

## 2026-02-22 — Threefold / Twofold Repetition Correctness

### Fixed
- `push_position()` now called after `ChangePlayer()` in both `DoMove()` branches
- `is_repetition()` loop start corrected (parity fix)
- Twofold-in-search branch was unreachable; fixed dead condition
- Castling rights and en-passant square changes now included in Zobrist hash;
  `zobrist_hash_` widened from `unsigned int` to `uint64_t`

## 2026-02-20 — Killer Moves + History Heuristic (PR #6)

### Added
- Fully implemented in `AIPerplex`: `killers_[MAX_PLY][MAX_KILLERS]`,
  `history_[2][64][64]`; methods `clear_killers`, `store_killer`, `clear_history`,
  `age_history`, `update_history`
- Killers cleared at search start; history aged each iteration (halved to prevent
  overflow); scoring integrated inline in `pvs()` (relocation to `MoveSorter` handled
  separately, see 2026-03-10)

## 2026-02-19 — Perft Testing Framework (commit 0798a951)

### Added
- `StratEngine/Tests/Perft.h/cpp` + `PerftRunner.cpp`; `Tests/perft_test_cases.json`
  (128 positions, depth 1-6); integrated into main binary
  (`StratChessEvolved.exe perft test`)
- Surfaced design issues around `GameInfo` state (en-passant, etc.) that had been
  stored in `Game` rather than `Board`; one `MoveGenerator` defect found and fixed
- All 128 test cases passing at every depth where sensible; ~21M NPS in Release

Direct commit, predates PR-based workflow (before PR #1) — not part of any pull request.

## 2026-02-11 — Search Algorithm Fixes + iterative_deepening Refactor

### Fixed
- Iterative deepening timeout move selection bug
- Score=0 acceptance on interrupted searches
- PV table/move mismatch issue
- Mate emergency infinite loop
- Mate-found not stopping iteration
- False-positive score-swing rejections (Case 5b)

### Changed
- Extracted helper methods: `assess_iteration_quality`, `should_stop_early`,
  `handle_empty_move_emergency`
- Introduced `SearchResult` struct for a clean interface
- Added `SearchTuning` struct for runtime parameters
- Improved log levels (debug/info/critical)

## Undated

These entries have no PR reference in the original Roadmap.md text and couldn't be
matched to a specific PR with confidence via title/content search. Believed to fall
roughly within the Feb–March 2026 window based on their original position in the
document, but treat the ordering here as unverified.

### Changed
- **TranspositionTable Thread-Safety**: per-bucket `shared_mutex` locks (probes
  `shared_lock`, stores `unique_lock`); global `shared_mutex` for resize/clear; TT only
  cleared on new game.
- **GameInfo History in Board**: history arrays moved from Player into `Board`;
  prerequisite for De-Singleton Board.
- **Move sorting: stack-allocated sort buffer**: `pvs()` uses a stack `std::array`
  instead of heap allocation per call.

### Removed
- **Remove Dead Code**: commented-out old `Search()` method body removed from
  `AIPerplex.cpp`.

### Fixed
- **Fix `zobrist::initialize()` Never Called**: now called in the `Board` constructor;
  all castling/en-passant/side-to-move Zobrist keys initialised before any `Board`
  method runs.
