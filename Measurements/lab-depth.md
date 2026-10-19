# Lab depth

Mean search depth each side of a CI strength-lab run reached, read from the PGN move comments
(`{score/depth time}`) by `Scripts/compare_lab_depth.py`. The PGNs are kept in
`StratChessSupport\StrengthLabPgn\<date>_strength-<run>`.

**Δ is the instrument; the means are context.** Δ pairs the candidate's and the reference's own
move k from the same opening, same colour, and its 95% interval clusters by opening pair. Mean depth
across runs measures the search's shape as much as its speed: the same reference (`719af11`) read
12.01 and 12.14 in consecutive runs, so a cross-run difference under about 0.15 plies is noise.
Δ reads as speed only when the change keeps the tree: an eval change with no speed effect read
+0.02 (run 36495197163), and a change that reshapes the tree moves Δ for that reason alone. The
Elo of each run is in [`ci-per-change.md`](ci-per-change.md).

10+0.1 and `Threads=1` unless the row says otherwise. Runs before 2026-10-06 built with GCC 13 on
`ubuntu-24.04`, later ones with GCC 15 on `ubuntu-26.04` (#476).

| Run date | Run | Candidate | Reference | Games | Cand. mean | Ref. mean | Δ [95%] |
|---|---|---|---|---|---|---|---|
| 2026-08-28 | 33215162562 | ff5d2f5, LMR depth clamp keeps one ply (#363) | be5ca11 | 19980 | 11.60 | 11.67 | −0.081 [−0.085, −0.078] |
| 2026-08-31 | 33429454765 | e792a81, drawish-ending scaling (#128) | b3ab55a | 19980 | 11.65 | 11.67 | −0.011 [−0.014, −0.008] |
| 2026-09-01 | 33568346899 | 0e5e9c3, KR vs KR scaled (#436) | 8fd70c3 | 19980 | 11.69 | 11.68 | +0.003 [−0.000, +0.006] |
| 2026-09-05 | 33989392373 | 86877f7, minor-piece outposts (#112) | 9708c65 | 19980 | 11.53 | 11.54 | −0.017 [−0.020, −0.014] |
| 2026-09-11 | 34657777302 | ad7a422, compact TT + `Hash` 256 (#442), **18+0.18** | c191d08 | 20000 | 13.33 | 13.32 | +0.013 [+0.010, +0.017] |
| 2026-09-13 | 34788035846 | 0722ee5, TT generation per search (#544) | 19ff12b | 19980 | 12.28 | 12.20 | +0.012 [+0.008, +0.015] |
| 2026-09-18 | 35336326192 | e2ff7d2 at `Contempt=20` (#452) | e2ff7d2 | 19980 | 12.62 | 12.63 | −0.004 [−0.008, −0.001] |
| 2026-09-27 | 36354288096 | e11a58a, deepening stops early only on mate (#652) | f58f052 | 19980 | 12.69 | 12.54 | +0.163 [+0.155, +0.170] |
| 2026-09-29 | 36495197163 | 455352e, opposite-bishop scale (#599) | 0c01b00 | 19980 | 12.86 | 12.85 | +0.020 [+0.017, +0.024] |
| 2026-09-29 | 36599205249 | 17cf5a3, continuation history 2 plies (#664) | 481aa97 | 19980 | 12.72 | 12.66 | +0.005 [+0.001, +0.008] |
| 2026-09-30 | 36782774432 | 05fc0d5, toolchain null: GCC 15 / `ubuntu-26.04` (#476) | 7f72225 | 28400 | 12.76 | 12.76 | +0.001 [−0.002, +0.003] |
| 2026-10-02 | 37020636422 | 48a31ad, singular extensions on (#95) | 48a31ad, `SingularExtensions=false` | 19980 | 12.23 | 12.83 | −0.579 [−0.583, −0.575] |
| 2026-10-03 | 37154126192 | ee45484, singular min depth 6, margin factor 1 (#702) | 4dafbdd, previous singular defaults | 19980 | 11.68 | 12.16 | −0.526 [−0.529, −0.522] |
| 2026-10-06 | 37518249596 | c82dc6e, lock-free TT (#747), **`Threads=4`** | 79c3217 | 6624 | 12.51 | 12.30 | +0.199 [+0.192, +0.205] |
| 2026-10-09 | 37964064691 | c16e692, TT prefetch (#776) | f07859d | 26640 | 11.74 | 11.60 | +0.133 [+0.130, +0.136] |
| 2026-10-12 | 38312922668 | cd97a1f, `LmrHistoryDivisor` screen, arms A/B/C (#816) | cd97a1f | 19980 | 11.75 / 12.13 / 11.90 | 11.95 | pooled −0.021 [−0.025, −0.018] |
| 2026-10-15 | 38825283241 | cced4d1, legal-only in-check move lists (#823) | 663e2c5 | 23976 | 11.97 | 11.79 | +0.194 [+0.191, +0.198] |
| 2026-10-16 | 38906169058 | 83b4cf9, static eval in the TT entry (#832) | 0fb63d0 | 19980 | 12.04 | 11.92 | +0.120 [+0.117, +0.124] |
| 2026-10-16 | 38981207653 | df34ce5, pawn-structure cache (#837) | 295fa5f | 19980 | 12.13 | 12.05 | +0.095 [+0.092, +0.099] |
| 2026-10-17 | 39035597892 | eac04e2, staged move generation (#839) | 12bf7d1 | 19980 | 12.19 | 12.17 | +0.030 [+0.027, +0.034] |
| 2026-10-18 | 39128458652 | 3d0e053, staged killer stage (#839) | 9b3d416 | 19980 | 12.24 | 12.22 | +0.018 [+0.014, +0.021] |
| 2026-10-18 | 39162362280 | d7b7939, `LmrImprovingOffset=1` (#848) | 719af11 | 19980 | 12.11 | 12.01 | +0.080 [+0.077, +0.084] |
| 2026-10-19 | 39208302248 | 6b24787, `LmrImprovingRelief=1` (#848) | 719af11 | 19980 | 11.93 | 12.14 | −0.225 [−0.229, −0.221] |

## Row detail

**Trend, 2026-08-28 to 2026-09-29.** At 10+0.1, reference depth went from 11.5-11.7 (08-28 to
09-05) to 12.2-12.9 (09-13 to 09-29). Then it fell to 11.60 by 2026-10-09, and singular extensions account for all of it: about
−1.1 plies from two steps, −0.58 for turning them on (#95) and −0.53 for the #702 retune, which
extends more. The toolchain did not move depth: 36782774432 runs GCC 15 / `ubuntu-26.04` at 12.76,
against 12.66-12.72 on GCC 13 the day before. 09-11 (18+0.18) and 10-06 (`Threads=4`) are not on
the 10+0.1 single-thread scale.

**36354288096 changes the tree:** deepening that no longer stops early on a large score completes
more iterations, so +0.16 is search policy, not speed.

**Trend, 2026-10-09 to 2026-10-19.** Reference mean depth rose from 11.60 to 12.0-12.2, about
+0.5 plies, mostly the speed changes #776, #823, #832 and #837 landing on `main`.

**The node-identical speed changes order the same way by Δ as by Elo:** #823 +0.19 / +30.4,
#776 +0.13 / +16.6, #832 +0.12 / +14.8, #837 +0.10 / +10.8, #839 +0.03 / +3.0 (inconclusive).

**38312922668** pools three arms into one Δ, so its pooled figure answers nothing; the arm means
against the reference are A (16) −0.20, B (32) +0.18, C (128) −0.05. Depth moved both ways with no
Elo difference between the arms.

**39128458652** has no `ci-per-change.md` row; the #839 progress record gives +2.30 ± 3.54,
inconclusive.

**39162362280 and 39208302248 change the tree, so Δ is not speed.** The offset reduces more and
reads deeper (+0.08). Relief (R − 1 at improving nodes) reduces less, so the tree is larger and the
same clock completes fewer plies: −0.225,
flat across game phase (−0.23 at own moves 0-9, −0.20 at 40+). The script's −13% speed / −22 Elo
conversion does not apply; the run's measured −13.7 Elo is the variant's own cost.
