# Instruments and traps

## Corpora

Every strength-lab run uploads 18 shards of annotated PGN (~20,000 games, ~70 MB), kept 90 days.
fastchess writes each side's own report into the move comment, `{score/depth time}`, from the
mover's perspective. Download command: `Docs/MoveQuality.md` → Regenerating.

Downloaded corpora and the datasets derived from them live in `StratChessSupport\`, beside the main
checkout. Its `README.md` indexes the folders and each folder's README its naming convention: look
there before downloading — it is the only copy of a run past retention — and add a row for anything
you store.

## Which script answers which question

Flags live in each script's `--help`.

| Question | Script | Read first |
|---|---|---|
| Where (phase, piece) the engine loses quality by its own judgement | `Scripts/analyze_move_quality.py` | `Docs/MoveQuality.md` |
| Mistakes the engine does not know it made (needs Stockfish in `EngineTesting/`) | `Scripts/analyze_external_quality.py` | `Docs/MoveQuality.md` |
| Positions joined to engine eval and an oracle, for eval-term questions | `Scripts/measure_eval_error.py` | its docstring |
| The option value at which `bestmove` changes | `Scripts/bisect_uci_option.py` | its docstring |
| A FEN corpus from repository assets | `Scripts/build_corpus.py` | its docstring |

## Traps that invert a headline

- **The contested filter is for blunder rates.** Counting blunders, restrict to positions the mover
  scored within ±150 cp — unrestricted, already-lost positions dominate: the endgame looks 6× more
  blunder-prone and king moves look like a third of all blunders. Comparing loss magnitude across
  classes, or measuring eval error, leave it off: it selects on the engine's own score and keeps
  94.8% of opening rows against 48.6% of endgame rows, widening the opening–endgame gap from 18.7
  to 24.0 cp (`Docs/MoveQuality.md` → Tier 2 findings). `measure_eval_error.py` applies none by
  design.
- **Self-check first.** `analyze_move_quality.py --self-check` and `--self-test` before trusting a run.
- **Exclusive bands.** Material-class score bands do not nest; quote the cumulative row.
- **Horizon before eval.** A comparison at the engine's own depth cannot tell an evaluation that
  misjudges from a refutation beyond the horizon. Re-score a sample at depth + 6 before blaming
  evaluation — it moved half of #481's population from eval to search.
- **Position class, not grand mean.** The lab population is skewed ~2.4:1 toward White, so a signed
  mean restates the calibration line.
- **Missing terms.** Per-term correlation cannot detect a term that is absent (zero variance is
  exonerated by construction), and a low |r| on a low-range term is arithmetic, not innocence.
- **Split before attributing.** Split a counter by cause before explaining its rise.
- **Depth pre-filter.** `bisect_uci_option.py` aborts the whole run on the first position that
  stops short of `--depth`; filter a `build_corpus.py` corpus first (~1% never reach it).
- **Prove the option applied.** A ~0 result for a one-sided UCI option may mean it never reached the
  engine; the per-move scores in the PGN comments settle which.
- **Driving UCI.** `go` is asynchronous: read until `bestmove`. Close each engine inside its worker
  task — at exit is too late, and Windows orphans it.
