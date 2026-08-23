# Move-quality scan over strength-lab PGNs — Design

**Issue:** #77 (Tier 1 first cut)

## Goal

Every strength-lab run writes a fully annotated PGN of every game it plays, and nothing reads them.
A production run is 18 shards x 1,110 games; one shard alone carries 119,899 annotated engine moves,
so a run holds roughly 2.2 M scored positions. We consume one number from it — the pooled Elo — which
decides *whether* a change helped and is silent on *where*. Eval work (#110) and #76's stale July
tracers both need the second answer, and the data to produce it is already sitting in GitHub artifact
storage, retained 90 days. This adds the offline tool that turns those PGNs into per-phase, per-build
move-quality numbers, plus the first committed baseline to compare later runs against.

## Scope

**This change will:**

- Add `StratChessEvolved/Scripts/analyze_move_quality.py`: parse annotated PGNs, report self-swing and
  cross-build disagreement bucketed by game phase, build and remaining-clock band, and print the two
  tracers #76 needs.
- Add `Docs/MoveQuality.md`: the generated baseline for strength run `31311549549` (all 18 shards),
  the method notes, and the exact command to regenerate it.
- Add a one-line pointer to that doc from `Docs/EloMeasurement.md`.

**This change will not:**

- Touch any engine source. This is an offline reader of match output; no build, no search, no eval.
- Implement Tier 2 (external-engine ACPL adjudication). It needs an outside binary, a provenance
  decision and hours of replay compute, and it should be scoped only after Tier 1 has run once and
  shown what it does and does not resolve.
- Download artifacts itself, wrap itself in a `Run-*.ps1`, or run in CI. The `gh run download`
  command lives in the script's docstring. All three are cheap to add later and none of them is
  justified before the tool has proven what it finds.
- Change `game_settings.json`, CLAUDE.md, or any measurement procedure. Nothing here feeds a gate.

## Decisions

### D1: Report both the self-swing and the cross-build gap

In a candidate-vs-reference match the two builds never score the same position — each annotates only
positions where it is to move — so there are two distinct measurements available from one walk, and
the first cut computes both:

- **Self-swing** for build X at ply *t*: `s_X(t) - s_X(t+2)`. The engine's own admission that its
  position got worse. This is what the issue originally described.
- **Cross-build gap** at ply *t*: `s_X(t) + s_Y(t+1)`, where the addition is the perspective flip
  (see D2). Zero means the two builds agree about the position after X's move.

Rejected: self-swing only (simplest, but blind wherever both builds share a misjudgement — which is
precisely the class of eval defect worth finding); cross-build gap only (sharper at locating
differences between two builds, but meaningless on a build-vs-itself match and silent on shared
blind spots).

The gap conflates genuine disagreement with the one ply of real change that X's move caused. That is
inherent to any adjacent-ply measure and is a limit on interpretation, not a bug to fix: a large gap
marks a position where the two builds part company, and the position — not the number — is the
finding.

### D2: Scores are mover-relative, and the parser depends on it

Both formulas in D1 assume each `{+1.22/11 0.415s}` is from the perspective of the side that just
moved (standard UCI convention), not a fixed White-relative score. Verified against shard 0 of run
`31311549549`: in the round-1 game, Black reports `+10.44` and White `-10.76` at the same moment, and
Black wins. Under a White-relative reading Black's `+10.44` would mean White was winning. If this
were wrong, every self-swing would be inverted for one side and every gap would read as a constant
double-count, so the perspective check under Validation exists to fail loudly if it ever is.

### D3: Phase mirrors `Eval.h`, endgame boundary borrowed from the mop-up gate

`phase = 1*N + 1*B + 2*R + 4*Q` summed over both sides, capped at 24 (`Eval.h:348-355`,
`MAX_GAME_PHASE`). Buckets: **opening** >= 20, **middlegame** 7-19, **endgame** <= 6, where 6 is
`MOPUP_MAX_LOSER_PHASE` — so the endgame bucket is approximately where `eval_mopup` starts to apply.
(The engine gates mop-up on the *loser's* phase and this buckets on the *total*, so the alignment is
indicative, not exact.)

Rejected: bucketing by move number (no coupling and no drift risk, but a boundary at "move 30"
corresponds to nothing in `Eval.cpp`, so every finding would need translating before it could be
acted on) and non-pawn material in centipawns (the external convention, comparable with other
engines' published tables — worth adopting for Tier 2, not for a tool whose output should name our
own code).

The cost of D3 is duplicated constants that can drift if the taper weights change. Mitigation is a
comment naming `Eval.h` as the source of truth; a drifted copy shifts bucket boundaries slightly and
degrades the report, it does not produce a wrong answer inside a bucket.

### D4: Mate scores are counted, never averaged

One shard carries 1,192 `±M<n>` annotations. They are excluded from every mean and blunder-rate
statistic — a mate score is not on the centipawn scale and saturates any average it enters — and
counted separately as **mate-flips**: a build announcing forced mate and then not delivering it. A
parser treating `M` as a decimal is the most plausible way this tool silently produces wrong numbers,
so an unrecognised comment is a loud failure (D6), not a skipped move.

### D5: The remaining-clock band is derived, not read

The issue asks for a per-time-remaining breakdown as though the field existed. fastchess writes the
time *spent* on each move. Remaining clock is reconstructed exactly per side from
`[TimeControl "10+0.1"]` minus the running sum of that side's spent time, plus one increment per move
played. Cheap and exact, but it is a derivation, and a maintainer reading the output should know that
a clock-band column depends on the `TimeControl` tag being present and well-formed.

### D6: Fail loudly on an unrecognised comment; skip only truncated games

An unparseable score comment aborts with the offending text and its location: a format the parser
does not understand is a changed fastchess version (see Assumptions), and quietly dropping those
moves would bias every statistic toward whatever fastchess still spells the old way. The one
tolerated case is a **truncated game** — the shard upload runs `if: always()`, so a killed shard
yields a partial final game. That game is skipped and the count of skipped games is reported.

### D7: The baseline report is committed

The script alone leaves the first run's numbers in a terminal, which makes the before/after
comparison the issue exists for impossible six months later. `Docs/MoveQuality.md` carries the
generated table for run `31311549549`, the method notes and the regeneration command. It follows
`Docs/EloLog.md`'s role as a measurement log, and like it, it is appended to rather than rewritten.

## Assumptions I cannot verify from the code

- **The comment format belongs to fastchess `v1.8.0-alpha`** (pinned at `strength.yml:57`), and a
  future version bump could change the spelling of scores, mate scores or the trailing adjudication
  note. Verified only against artifacts from that pinned version. Would be settled by re-running the
  parser after any bump; D6 makes that a loud failure rather than a silent skew.
- **`PlyCount` counts from the setup position, not the true game start.** CI games begin at fullmove
  9 from an EPD book position (`[SetUp "1"]` + `[FEN]`), so total game length is `PlyCount + 16` and
  #76's "200 plies" is `PlyCount > 184`. Confirmed on shard 0, where all 1,110 games start at
  fullmove 9; not confirmed to hold for a book with variable-length lines, which a future
  `openings-large` file could be. The script should read each game's FEN fullmove counter rather than
  assuming 9.
- **CI PGNs contain no `{book}` comments** (the book is EPD, so every move present is an engine
  move). Confirmed on shard 0: zero occurrences. A local `Run-EloMatch.ps1` run against a `.pgn`
  book *does* emit `{book}`, so the parser must tolerate and exclude them for the local path.
- **python-chess replays these games without error.** Version 1.11.2 is installed and already used by
  `build_corpus.py`, but that script harvests FENs rather than walking full games with comments.
  Settled by the game-count invariant below on a first real run.

## Invariants

- Parsed game count equals the `[Event ` count of the input files (1,110 per CI shard), less the
  reported truncated-game skips.
- Parsed annotated move count equals the count of score comments in the input (119,899 in shard 0).
- Every reported move is attributed to a build by the `[White]`/`[Black]` tag, never by colour:
  `-repeat` plays each opening twice with the colours swapped.
- Mate annotations appear in no mean.
- Identical input yields byte-identical output.
- The tool never writes into the input directory and never mutates any repo state.

## Validation

Docs/Tooling tier — no engine source changes, so no Elo match and no bench pass. What closes each
risk:

- **Arithmetic**: three flagged swings and three flagged gaps hand-checked against the raw PGN text.
- **Perspective (D2)**: on a decisive game, the losing build's cumulative self-swing must be positive
  and the winning build's near zero or negative. An inverted sign convention fails this immediately.
- **Coverage**: the two count invariants above, run over all 18 shards of `31311549549`.
- **Robustness**: a deliberately truncated copy of a shard PGN produces a skip count and a clean exit,
  not a traceback.
- **Determinism**: two consecutive runs over the same shard set diff empty.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Scores are mover-relative (D2) and the evidence for it | module docstring in `analyze_move_quality.py` |
| Phase formula mirrors `Eval.h`, with Eval.h named as source of truth (D3) | source comment beside the phase helper |
| `PlyCount + 16`, EPD start, no `{book}` in CI PGNs | `Docs/MoveQuality.md` → method notes |
| Clock band is derived from `TimeControl`, not read (D5) | `Docs/MoveQuality.md` → method notes |
| Baseline numbers for run `31311549549` | `Docs/MoveQuality.md` |
| Refreshed #76 tracers (long games, draw-rule split) | comment on #76, sourced from the baseline |
| Where the corpus lives and how to fetch it | script docstring + `Docs/EloMeasurement.md` pointer |
| What Tier 1 cannot see (self-graded; 81% of games adjudicated, so "endgame" means the position at adjudication) | `Docs/MoveQuality.md` → limits, and issue #77 |

Also record any approved decision that changed during implementation, and why.
