# Contempt for drawn scores — Design

**Issue:** #452

## Goal

Every draw the search detects scores exactly `GameValues::Draw` (0), so "draw now" and "equal
position, play on" are indistinguishable and the engine takes a repetition whenever the alternative
evaluates at or below 0. The Tier 1 move-quality scan of run `33215162562` puts 22.0% of 19,980
games (4,393) on threefold repetition, with 99.9% of them ending with both sides reporting under
50 cp — the engine repeats in positions it believes are equal. Whether declining those draws is
worth Elo is a playing-policy question, not a bug, and its sign is not obvious. This change adds the
mechanism — a signed contempt offset on search-detected draw scores — shipping it disabled, so the
merge cannot change playing strength and the measurement becomes a separate, separately-funded
decision.

## Scope

**This change will:**

- Add one `SearchTuning` field, `contempt`, default `0`, domain `[0, 100]`, exposed over UCI as
  `Contempt` and bindable from `game_settings.json`.
- Route the four search-detected draw endpoints through one helper, `draw_score(td)`:
  `pvs()`'s `check_draws` exit (`AIPerplex.cpp:652`), `quiescence()`'s `check_draws` exit
  (`:1258`), the stalemate return in `adjustScoreForGameState()` (`:1133`), and the bare-king
  stalemate return in `quiescence()` (`:1313-1317`). Two TT stores carry the resulting value: the
  bare-king store at `:1314` and the terminal store at `:1079-1083`, which caches whatever
  `adjustScoreForGameState()` returned. That pair is the complete list D3 argues over.
- Generalise `assess_iteration_quality()`'s CASE 4 from `metrics.current_score == 0` to the
  contempt-shifted draw band (D2), so the SCORE_DROP rejection keeps working at non-zero contempt.
- Capture the root side to move per search and clear the transposition table whenever the contempt
  context it was filled under no longer matches (D3).

**This change will not:**

- Change the abort and time-limit unwind returns (`:646`, `:649`, `:1224`, `:1227`). Those are
  fabricated values for a frame that searched nothing, not game results, and must stay neutral.
- Change mate or stalemate *adjudication*, or any evaluation term. In particular
  `Evaluator::Evaluate`'s scale-0 dead-drawn material return (`Eval.cpp:1088`) stays at 0, with a
  consequence worth naming: at `Contempt=20` a repetition scores −20 while liquidating into a dead
  K+B vs K scores 0, so the engine prefers the genuinely dead position to the repetition it is being
  taught to avoid. That is the most likely way a flat contempt term misfires, and it is D5's first
  hypothesis if the measurement comes back negative. Closing it means teaching contempt about
  eval-detected draws, which is a larger change and needs the flat term measured first.
- Decay contempt with game phase, material deficit or remaining time. Adding a decay before the flat
  term has been measured moves two variables at once and answers neither question.
- Enable contempt by default, or run the CI strength lab. Both belong to the default-flip decision
  (D5), which is a separate PR and a separate spend.

## Decisions

### D1: Sign from the node's side to move against the captured root colour, not ply parity

`draw_score(td)` returns `td.board.GetCurrentColor() == root_color_ ? -contempt : +contempt`, so a
positive `contempt` makes a draw a small loss **for the side the engine is playing**, expressed in
the negamax perspective of the node that reports it. Getting this backwards makes the engine avoid
draws when it is losing, which is the exact failure mode #452 names.

Ply parity (`ply % 2`) is the cheaper and more common formulation and was rejected: null-move
pruning flips the side to move while *incrementing* ply (`AIPerplex.cpp:768-774`), so below a null
move parity no longer tracks the side to move and the sign inverts for that whole subtree. Reading
the board's colour costs one member read on a cold path — draw endpoints only, never per node — and
is correct under null moves, singular verification frames (which re-enter at the parent's ply and
the parent's colour) and any future ply-shifting extension.

`root_color_` is an `AIPerplex` member written in `Search()` before helper threads are spawned and
read-only for the rest of the search, exactly as `tuning_` already is. Helpers seed from the same
root board, so they share the value. It is declared `eColor root_color_ = WHITE;` rather than left
indeterminate, because `pvs()` is reachable without `Search()` in the test build
(`SearchTestFixture.h:270-275` calls it directly); the sign tests therefore set it explicitly
through a new `AIPerlexTestFixture::set_root_color()` poke, beside the existing
`set_last_move_was_null()`, instead of asserting against a default.

### D2: A runtime integer option defaulting to 0, and one consumer generalised with it

`contempt` is one `TUNING_FIELD` entry. `0` reproduces today's arithmetic exactly — the helper
returns `GameValues::Draw` unchanged — which makes the zero-contempt identity arithmetic rather than
a second code path. A compile gate was rejected: the feature has no hot-path cost to gate away, and
standing practice is that a compile gate is a temporary state whose end point is a default-true
runtime flag or deletion.

The domain is `[0, 100]`, not the symmetric `[-100, 100]` an earlier draft assumed. Negative
contempt — deliberately *seeking* draws, for a defensive posture — looked free to allow, and is not:
`SearchTuningSchema::read_uci` rejects any character outside `0123456789` before parsing
(`SearchTuningSchema.cpp:120`), answers `not applied`, and leaves the engine on its default. A
negative value would therefore be settable from `game_settings.json` and silently ignored over UCI,
which is the worse of the two possible failures. Widening the domain and teaching `read_uci` to
parse a sign was rejected as scope: that parser is shared by every arithmetic tuning field, nothing
has asked for draw-seeking, and the change is a separate issue with its own rationale if it ever
does. The asymmetry is a deliberate YAGNI, not an oversight, and `Validate()` enforces it.

One existing consumer depends on a drawn score being literally zero and would be silently defeated:
`assess_iteration_quality()` CASE 4 (`AIPerplex.cpp:1560-1563`) rejects an iteration as SCORE_DROP
when `metrics.current_score == 0` while the running best is above `score_draw_threshold`. It tests
the literal, not `GameValues::Draw`, so a grep for the constant does not find it. At non-zero
contempt an iteration that collapses into a repetition reports −contempt, the guard stops firing,
and a suspicious iteration is accepted. The test becomes
`std::abs(metrics.current_score) <= std::abs(tuning_.contempt)`, which is exactly `== 0` at the
shipped default and so changes nothing there. Silently defeating the guard was rejected: it is a
search-quality gate, and losing it is a behaviour change nobody asked for.

### D3: Clear the TT whenever the contempt context the table was filled under changes

A contempt-derived draw score propagates into parent entries through both stores named in Scope, so
a stored bound depends on which colour the search was favouring and by how much — context the
Zobrist key does not carry. This is the root-perspective hole the triage flagged, separate from
#347's clock hole.

In normal play the hole is unreachable: a UCI engine searches only its own moves, so the root colour
is constant for a whole game and `StartNewGame()` clears the TT between games; in `game` mode each
`SearchPlayer` owns its own `AIPerplex` and therefore its own table. It becomes reachable only when
one service searches alternating colours, or under a changed contempt value, without `ucinewgame` —
analysis, the tactical runner, tests.

So `Search()` keeps the `(root_color, contempt)` pair its table was filled under and clears the TT
when the incoming pair differs. Keying on the pair rather than on a colour flip alone is deliberate:
gating the clear on "contempt is currently non-zero" would let a search at 20 on a white root be
followed by a `Contempt 0` search on a black root that consumes tinted entries — which would make
the zero-contempt identity false inside that process — and would leave entries from an old magnitude
in place after 20 → 60. `StartNewGame()` resets the stored pair to unset.

Rejected alternatives: encoding the context in the entry (no spare bits — #534 left `PackedEntry` at
16 bytes with 3 reserved metadata bits, and #347 already wants them); suppressing score reuse across
contexts (same storage problem, plus it costs cutoffs in the common case where the context never
changes); accepting the pollution (bounded at 2×contempt, but it is an unforced silent wrong answer
in exactly the analysis setting where a user would notice). The chosen option costs a TT clear per
context change in analysis and nothing at the shipped default, where the guard is two integer
comparisons per `Search()`.

### D4: #347 is not a blocker and this change does not widen it

The rule-50 draw score already depends on the halfmove clock and is already cached under a
clock-independent key; #347 measured that error at 535 cp. Contempt changes the *value* of such an
entry by at most `|contempt|` (≤ 100 cp by domain) and introduces no new context into the key — the
clock dependency is unchanged in kind and in reachability. #347 is parked with zero observed
rejections (#549). Treating it as a hard blocker was therefore rejected; the interaction is recorded
here and on #347.

### D5: The lab run is not a merge gate; it is the default-flip decision, pre-registered here

**The merge of this PR is gated on correctness only** — unit tests, the equivalence run, the full
suite. Nothing the lab could report would change what lands, because the shipped default is 0 either
way. Saying so plainly is the point: a pre-registered table whose every cell reads "merge as-is" is
not a decision rule, and spending ~3 h and 18 of 20 CI slots on a run that cannot change this PR is
not justified by this PR.

The run belongs to the follow-up question — *should the default be non-zero?* — and is pre-registered
here so the bar exists before any numbers do:

- **Configuration.** `gh workflow run strength.yml --ref <branch> -f candidate_uci_options="Contempt=20"`,
  against the merge base as reference. The lab gained per-engine UCI options in #564, so the binary
  that plays is the one that merges — no probe branch, and nothing to say about a measured binary
  differing from the shipped one. The dispatch fails fast if the engine does not advertise
  `Contempt`, which also makes a run against a merge base that predates this change impossible to
  start by accident.
- **Power.** One run, ~20,000 games, ~±4 Elo. Its lower bound clears 0 only for a true effect of
  roughly +4 Elo or more. Peer self-play is close to the worst case for contempt — the term is
  defined relative to the opponent's strength — so a null result is the expected outcome and is
  informative only as "not worth enabling against a peer".

| Outcome | Action |
|---|---|
| CI lower bound > 0 | Flip the default to 20 in the follow-up PR, citing the run. |
| CI contains 0 | Leave the default at 0. Contempt is a neutral-in-peer-play style option; record the interval and close #452 on that basis. |
| CI upper bound < 0 | Leave the default at 0 and record that contempt at 20 cp is measurably harmful in peer play. Check the `Eval.cpp:1088` interaction named in Scope as the first hypothesis before testing another magnitude. |

A weaker-opponent panel is **not** required, because neither this PR nor the follow-up asserts a
weaker-opponent gain. Such a claim needs its own experiment and its own issue.

Acceptance criterion 4 of #452 ("the threefold share must move") is replaced, per the triage's item
6, by a measured comparison with a failure condition: the candidate's threefold share must differ
from the reference side's of the same run by more than the binomial interval at that game count, and
both intervals get reported. A bare "there is a difference" is unfalsifiable and would be satisfied
by sampling noise.

### D6: Run under the standard adjudicator, and report the draw classes separately

`strength.yml:416` and `Run-EloMatch.ps1:686` adjudicate a draw when **both** engines report
|score| ≤ 10 cp for eight consecutive moves from move 40. At `Contempt=20` the candidate reports
±20 in exactly the lines that adjudicator targets, so it stops firing on candidate games: the two
sides of one match run under different effective rules, candidate games run longer against a
wall-clock budget calibrated with adjudication firing, and the threefold share rises mechanically
because games formerly cut at move 48 now reach a repetition.

#564 made the engine *options* dispatchable per side; it did not touch the adjudication flags, which
are still hard-coded and deliberately identical to `Run-EloMatch.ps1` so the two instruments differ
only in toolchain and hardware. So the run uses the standard adjudicator, and the confound is
made visible rather than hidden: the re-scan reports **adjudicated draws and rules draws (threefold,
fifty-move, stalemate) separately for each side**, and the D5 threefold comparison is made on the
rules-draw counts. Testing at a magnitude under the 10 cp bar was rejected — it would keep
adjudication symmetric but measure a term too small to resolve at ±4 Elo, answering nothing. The
wall-clock risk is real and is checked by the shard timing the run itself reports; a run that
overruns is re-dispatched with fewer rounds rather than re-interpreted.

## Assumptions I cannot verify from the code

- **No consumer of the *reported* score depends on a draw being exactly 0, other than the one D2
  fixes.** The grep that found `assess_iteration_quality()` was a grep for the literal, not for
  `GameValues::Draw`, because the constant does not appear there. The enumeration, at
  `contempt != 0`: `info score cp` (a number, no zero-special-casing — verified in
  `UCIReportingTests.cpp` expectations); fastchess draw adjudication (D6 — it *does* depend on the
  value, which is why D6 exists); `assess_iteration_quality()` CASE 4 (D2); `should_stop_early()`;
  and the Tier 1 move-quality scan's score bands, whose "< 50 cp" convention in `Docs/MoveQuality.md`
  is a threshold, not an equality, and so absorbs a 20 cp shift without changing class. Closed by
  re-running that enumeration against the implementation at `Contempt=50`, not by the equivalence
  run — at contempt 0 no consumer can observe a shifted score at all, so that run is the regression
  guard for the shipped default and nothing more.
- **The lab's ~±4 Elo interval at ~20,000 games holds.** Taken from skill `measure-strength` and past
  runs, not re-derived. Verified by the run's own reported interval; the D5 table is applied to the
  interval actually measured.
- **`Contempt=20` is a sensible first magnitude.** Taken from the 10–30 cp range in #452, which cites
  general engine practice rather than a repository measurement. Not verified and not verifiable
  without spending further runs on a sweep; the D5 table therefore treats a null result at 20 as a
  verdict on 20, not on every magnitude.

## Invariants

1. In a process where `contempt` has never been set non-zero, the search is node-identical and
   move-identical to `origin/main` at `Threads=1`. No new work runs on any per-node path at any
   contempt value: the helper is called only at draw endpoints, and the D3 guard once per `Search()`.
2. A drawn score is negative for the side the engine is playing and positive for its opponent, at
   every node, including below a null move and inside a singular verification frame.
3. Abort and time-limit unwind returns stay exactly `GameValues::Draw` at every contempt value.
4. No TT entry produced under one `(root_color, contempt)` pair is consumed by a search under a
   different pair.
5. SCORE_DROP still rejects an iteration that collapses to a drawn score, at every contempt value.
6. `Validate()` rejects a contempt outside `[0, 100]`; the UCI `Contempt` spin advertises that
   domain, and every value inside it is one the engine's UCI parser actually accepts.

## Validation

Search tier. Evidence, in the order it is produced:

- **Unit tests** (`StratChessTests/SearchContemptTests.cpp`, `[search][contempt]`), driving `pvs()`
  through `search_node_after` with `root_color_` set by the new fixture poke:
  - Both root colours on a repetition position: the returned score is `-contempt` when the drawing
    node's side to move is the root colour and `+contempt` when it is not. Asserted at an even and
    an odd ply, and once below a null move, so a ply-parity implementation cannot pass — invariant 2.
  - The same three shapes at the fifty-move limit and at a stalemate, so all four endpoints are
    covered rather than the repetition one standing in for them.
  - At `contempt = 0` each endpoint returns `GameValues::Draw`. This cannot fail for any sign helper
    that negates, so it is not evidence for invariant 1 — it guards against the helper being handed a
    non-zero offset at the default, and nothing more.
  - The abort path returns `GameValues::Draw` with `contempt = 50` set — invariant 3.
  - A marker entry planted in the TT is gone after a `Search()` whose `(root_color, contempt)` pair
    differs, and survives when it matches: colour flip at contempt 20, magnitude change 20 → 60
    under one colour, and the 20-then-0 sequence A2 of the review names. Same-pair searches leave it
    alone — invariant 4.
  - SCORE_DROP fires on a drawn-score iteration at `contempt = 0` and at `contempt = 20`, in the
    existing SCORE_DROP test family — invariant 5.
  - `SearchTuningTests.cpp`: domain rejection at `-1` and `101`, acceptance at `0` and `100`.
    `UCITests.cpp`: the `Contempt` option is advertised with the right type and bounds, and a
    `setoption name Contempt value -20` leaves the field untouched — the engine's own parser refuses
    it, and the test pins that rather than leaving it to be discovered by a lab run — invariant 6.
- **`Compare-SearchEquivalence.ps1 -After <worktree exe>`** at the default `Contempt=0` — identical
  node counts and best moves against the merge-base build at `Threads=1`. The regression guard for
  invariant 1 at the shipped default. It cannot observe a shifted draw score and so closes nothing
  about non-zero contempt.
- **`Run-Bench.ps1`, alternating, candidate vs. merge base.** Not required by the standing rule — the
  diff adds no per-node work — but cheap, and the only thing that would catch an accidental hot-path
  read, e.g. the D3 capture landing inside `pvs()` instead of `Search()`. Pass bar: nps within the
  run's own noise band.
- **Full test suite** including `[slow]`, then `Validate-PrePR.ps1`.
- **No Elo match for this PR**, per D5: the shipped default makes the merge strength-neutral by
  construction, and the equivalence run proves it. The pre-registered lab run in D5 and D6 belongs to
  the default-flip decision and is a separate spend.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Why the sign comes from the board's colour and not ply parity (null move breaks parity) | source comment on `draw_score()` |
| Root colour and magnitude are TT context the key does not carry; the pair-change clear is what closes it, it is inert at the default, and it does not widen #347 | source comment on the guard in `Search()`, `Docs/EngineContracts.md` → search internals, and a comment on #347 |
| The D5/D6 pre-registered bar, and any measured interval and re-scan numbers it later produces | `Docs/Changelog.md`, the PR body, and a comment on #452 |
