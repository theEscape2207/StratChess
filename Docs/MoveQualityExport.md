# Tier 2 Blunder Evidence Export — Format

A Tier 2 scan reports rates. `--worst-jsonl` additionally writes **every row it counted as an oracle
blunder**, with enough context to identify the observation and replay its position, so a later
attribution stage can sample from the evidence instead of re-running the corpus.

[`MoveQuality.md`](MoveQuality.md) is the surrounding method — what Tier 2 measures and what it
cannot see. This file is the artifact contract: who is in the file, what a record means, and what an
interrupted run leaves behind.

```sh
python Scripts/analyze_external_quality.py pgn --depth 12 \
       --worst-jsonl evidence.jsonl --source-run 33989392373
```

`--source-run` is a free-text run identifier recorded in the manifest. It is never inferred from the
corpus directory — it is a claim about where the games came from, and only the operator can make it.
It is rejected without `--worst-jsonl`.

## The stream

One UTF-8 JSON object per line: exactly one `manifest`, then zero or more `blunder` records, then
exactly one `complete`. Every record carries `schema_version: 1`. Each is flushed as it is written.

`Scripts/external_quality_export.py` is the only definition of the schema; `read_artifact()` reads a
file back and **rejects** a missing or duplicated footer, records after the footer, malformed JSON,
an unknown schema version or counts that do not reconcile. Read artifacts through it rather than
parsing the lines yourself.

## Who is in the file

The population is Tier 2's existing contested rows, unchanged — a finished game the parser accepted,
a numeric mover score whose absolute value is ≤ 150 cp, and a numeric score again on that side's next
turn two plies later. A row is **exported when its legacy clamped loss is at least 150 cp**, which is
the same predicate the report's `blu%` column counts. Every exported row is one blunder event, and
that identity is what lets the footer reconcile against the report.

Nothing else narrows it: the self-swing does not, and neither do the worst-row list's display
threshold of 300 cp or its five-per-game and twenty-global caps. Self-admitted blunders are included.

Two exclusions are properties of the population, not of the export, and any conclusion drawn from
this file must state them: the last two plies of every game are never eligible, and a row whose loss
is hidden by the ±1000 cp clamp is not selected — see the clipping counter below.

## Interpretation boundary

This is a selected-positive diagnostic artifact, not a tuning corpus. It contains blunders but no
row-level sample of the eligible positions that did not cross the threshold. The footer's aggregate
eligible counts therefore cannot supply denominators for a feature or motif. Use exported rows to
replay observations, form stratified diagnostic samples and test falsifiable mechanisms; do not use
their proportions as prevalence, relative-risk or evaluation-weight evidence. Parameter fitting
needs a separately labelled, denominator-bearing corpus split by whole game.

More search is an intervention, not a causal classifier. A replay selecting an oracle-acceptable move
at a larger budget establishes that the outcome is budget-sensitive; failure to do so leaves the cause
unresolved. Likewise, fixed-node best-move flips under small evaluation changes can be discontinuous
and non-monotone as the changed score alters the searched tree. They can falsify a proposed mechanism,
but they are not an ordinal tuning objective. Re-adjudicate distinct replay moves by their loss rather
than requiring exact agreement with the oracle's first move.

## Scores

Both endpoints are from the **original mover's** point of view, including the after-position. Each
carries a raw score in exactly one of two shapes plus the legacy projection the report is built from:

| Field | Meaning |
|---|---|
| `score` | `{"kind":"cp","value":-350}` or `{"kind":"mate","moves":3,"winner":"opponent"}` |
| `legacy_cp` | The centipawn projection the report uses, clamped to ±1000 |
| `finite_clipped` | True only for a finite raw score strictly outside ±1000 (exactly ±1000 is not) |
| `source` | `search`, `checkmate`, `stalemate` or `insufficient_material` |
| `best_move_uci` | First oracle PV move; null when absent or terminal |

Mate `moves` is nonnegative, in the oracle's UCI mate-distance units, and `winner` is `mover` or
`opponent` — which is what keeps mate-in-zero unambiguous. Terminal positions are assigned directly
and cost no search: checkmate is ∓1000 with `moves: 0`, stalemate and insufficient material are 0.

Two losses are derived per row. `legacy_loss_cp` is `max(0, before − after)` on the clamped scale and
is what selection uses. `raw_loss_cp` is the same difference on the finite scale, and is **null
unless both endpoints are finite** — there is no mate-to-centipawn conversion in this file beyond the
legacy one.

No exactness or bound flag is exported. python-chess merges info dictionaries, so a `lowerbound` flag
from an earlier line can survive onto a later exact score; publishing it would be false provenance.
These are depth-limited observations, and v1 claims nothing more.

## History

A row identifies itself by `row_id` (`input_index:game_index:ply_index`), unique within an artifact.
Across artifacts the identity is the input's SHA256 plus `game_index` and `ply_index` — **do not
deduplicate by FEN.** The same position recurs across games, builds, clock states and histories, and
those are different observations.

Replaying `setup_fen` then `moves_before_uci` reproduces `before_fen` exactly, and pushing
`played_move_uci` reproduces `after_fen`. The prefix covers only what the PGN holds: for a game
starting from a setup position, the real history before it is unknown.

`ply_since_book_exit` is nonnegative or null, and `book_exit_basis` says why:

- `explicit_book_prefix` — book annotations form a nonempty leading prefix; the first ply after it
  is 0.
- `setup_assumed` — no book annotations anywhere, and `SetUp "1"` with a `FEN`; the ply index is used
  directly. This follows the corpus convention rather than asserting it.
- `unknown` — neither signal, or a book annotation *after* real play, which makes the boundary
  ambiguous for the whole game. `ply_since_book_exit` is then null.

`engine_time_s` and the original `TimeControl` header are exported, but remaining clocks are not
reconstructed. That needs the original PGN, so **preserve the source PGNs** if clock analysis matters
later; losing them makes that analysis unavailable, not the exported positions invalid.

## The footer, and the clipping counter

`complete` carries `eligible_rows`, `exported_rows`, `finite_clipping_misses`, `scored_games` and a
per-`{build, phase}` cell breakdown, all read off the same counters the report is built from. The
builder refuses a footer whose cells do not sum to its globals.

`finite_clipping_misses` counts, over **all** eligible rows rather than exported ones, the rows where
the ±1000 clamp hid the blunder: `legacy_loss_cp < 150` while `raw_loss_cp ≥ 150`. It costs no extra
search and exports nothing. It is the size of the blind spot the clamp creates — so it can never
exceed `eligible_rows − exported_rows`, and the reader enforces that. Mate endpoints never qualify,
since `raw_loss_cp` is null; a finite row losing exactly 150 raw does.

Because the missed rows are by definition absent from the file, this number cannot be recomputed from
the artifact. It is the only way to know how much the clamp cost this scan.

## Failure and retry

The destination is checked **before the oracle starts** — an existing file, a missing parent
directory or a collision with `--json` fails the run immediately, so hours of searches cannot end on
a destination that was unusable from the outset.

The footer means "scoring finished", and is written as soon as the last row is scored, before the
report is built. So:

- **An interrupted or failed scan leaves a footer-less file** holding exactly what was scored before
  it stopped. `read_artifact()` rejects it. Nothing synthesizes a completion record.
- **A failed report or `--json` write leaves a complete, valid artifact.** The run still exits
  nonzero; completion is a property of scoring, not of presentation.
- **A successful scan with zero exported rows is a valid artifact**, including a corpus with no
  eligible rows at all. That case keeps its existing "no contested rows found" CLI error, and the
  artifact records the empty result.

There is no overwrite, resume or append. The file is opened with exclusive creation, so a partial one
is never silently destroyed: **retry with a new output path**, or remove or rename the partial file
first. Export is restart-from-scratch by design — a resumable scan is a separate piece of work.
