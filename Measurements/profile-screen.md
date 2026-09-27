# Profile-screen noise

Instrument: `Scripts/Compare-SearchProfile.ps1` on `-DSTRAT_SEARCH_PROFILE=1` builds, clang-cl
Release, `Threads=1`, one process per search. Noise source: `STRAT_PROFILE_TIEBREAK_SEED`, which
reorders tied moves in `ScoreMoves` by a seeded hash. That is a neutral change, so any difference
between two seeds is tree chaos and not an ordering effect. Like the move-quality ledgers, this file
holds a table per calibration and no `Verdict`.

**Reading a screen.** A late-cut or node delta counts only when it falls outside the Screen block's
±2 SE. With `-Seeds 0` there is no error bar, and the single-run row below is the noise to beat.

## 2026-09-28 — base `f58f052` + tie-break hook, `Tests/profile-screen.fen` (#653)

Every figure is the 95th percentile of |delta| over 2,000 random disjoint splits of seeds 1-16 into
two sides of K seeds. The delta is the mean over positions of the per-position mean-log ratio, which
is what the Screen block prints. The set had 121 positions; one stopped early under a seed and was
removed afterwards, so the committed file has 120.

| Depth | K seeds per side | Late-cut work (`latenodes`) | Nodes | Script ±2 SE covers |
|---|---|---|---|---|
| 12 | 1 | ±10.8% | ±5.6% | n/a |
| 12 | 2 | ±7.6% | ±4.4% | 93-94% |
| 12 | 4 | ±5.1% | ±3.0% | 94-95% |
| 12 | 8 | **±3.6%** | **±2.1%** | 94-95% |
| 16 | 1 | ±11.0% | ±7.9% | n/a |
| 16 | 4 | ±5.6% | ±4.0% | 92-94% |
| 16 | 8 | ±3.9% | ±2.8% | 92-94% |

Cost, sequential, both sides: 0.5 s per search at depth 12 and 3.9 s at depth 16. So K=8 on 120
positions costs about 16 min at depth 12 and about 2 h at depth 16.

### Row detail

- **What the rule catches.** The rule flags a change when it falls outside ±2 SE. Injecting a
  real effect into the after side of the null splits gives these catch rates for late-cut work:

  | Setting | 10% change | 5% change | no change (false alarms) |
  |---|---|---|---|
  | `-Seeds 8 -Depth 12` | 100% | 77% | 6% |
  | `-Seeds 4` | 94-96% | 44-49% | 6-7% |

  So a 10% ordering effect is reliably caught at either setting. A 5% effect needs K=8 and is
  still missed about 1 time in 4.
- **Per position, the noise is large.** At the median, one seed change moves a position's
  late-cut work by 0.34 (d12) or 0.38 (d16) in log sd, and its nodes by 0.21 or 0.25. The table
  under Per-position noise lists every position. The 4 endgames have a median of 0.39 at d12 and
  0.49 at d16.
- **Positions are close to independent.** The sd of the screen statistic over seeds is 1.0-1.17
  times the value predicted from independent positions. A seed does not shift all positions
  together, so the band shrinks as 1/sqrt(positions × K). That is also why the script's ±2 SE,
  which assumes independence, covers 92-95% of the null splits.
- **Noise does not carry across depths.** A seed's deviation at d12 predicts its deviation at d16
  with r = 0.09 (nodes 0.13). A position's noisiness does carry over (Spearman 0.42-0.47). So
  depth 16 is an independent second sample, not a confirmation of depth 12.
- **Robust statistics do not help.** The median and the 20%-trimmed mean have the same band as the
  mean. The majority-sign rule has a null range of 0.34-0.66 at 38 positions and 0.41-0.59 at 120,
  so "most positions moved" means nothing at either size.
- **Earlier 38-position set** (#651's; 34 book positions and 4 endgames, 17 seeds): one run per side
  gives ±19% late-cut work (d12) and ±23% (d16), ±11% and ±13% for nodes. With K=8 it gives ±7%.

### Amplifiers

| Trait | Evidence | Kind |
|---|---|---|
| Root best-move change | An iteration where the root best move changes costs +25% (d12) and +34% (d16) more than the same iteration under other seeds (+32% and +43% on the 38-position set). Positions whose final best move varies across seeds are noisier (Spearman 0.22 at d12, 0.33 at d16). | Inherent: a new best move re-searches the root |
| LMR re-search share | Within a position, the seeds with more re-search nodes also do more late-cut work (Spearman 0.51 at d12, 0.54 at d16). Across positions, a higher re-search share goes with more noise (0.24-0.28). | Inherent: a reduced search that beats alpha triggers a full-depth re-search |
| Short-PV early stop (#652) | Neutral seeds trigger it. On #652's two FENs, 4 of 68 seed runs stop, one of them on the same repetition PV as #651's candidate. In the wider book set, `r1bq1rk1/ppp3pp/2np1n2/4pp2/2P5/2PP2PN/P3PPBP/1RBQK2R w K - 5 9` stops at d16 on 6 of 9 seeds, **including generation order, so stock `main`**, and `r1bq1rk1/bpp2ppp/p1np1n2/4p3/2P5/2NPPNP1/PP3PBP/R1BQ1RK1 w - - 2 9` stops on 1 of 16. Iterations with a PV margin of 2 or less cost +31%. | **Discontinuity: an engine fix, tracked in #652** |
| Aspiration re-searches | They fail on under 3% of searches. Within a position their fail count does not track the cost (Spearman 0.02); across positions it goes slightly with less noise (-0.09 to -0.21). | Not an amplifier |
| Score swing of 30 cp or more | +1% to +7% per iteration. | Weak |

The positions that trigger the #652 stop are left out of `Tests/profile-screen.fen`, because
`Compare-SearchProfile.ps1` refuses a search that stops short of `-Depth`.

### #651 (history malus) read against this band

The candidate predates the hook, so it has one run only (generation order), compared against the
mean of all seeds:

| Set | Late-cut work d12 | d16 | Nodes d12 | d16 |
|---|---|---|---|---|
| 38 positions | +3.7% | +8.4% | +1.0% | +2.1% |
| 118 book positions | -7.7% | stopped (#652) | -4.9% | stopped (#652) |

All four readings are inside the single-run band, and the sign flips between the two sets. The
figures #651 recorded against generation order (+16% to +28%) mostly measured the base's own draw:
the reverse-order control read -0.2% against seed 0 but -11.1% against the seed mean. #651 is
therefore unresolved rather than failed. To re-screen it, rebase it on this hook and run
`-Seeds 8 -Depth 12 -Positions Tests/profile-screen.fen`.

### Per-position noise

The log sd over seeds 1-16, one row per line of `Tests/profile-screen.fen` (# is the FEN's line number, headers excluded). Divide by sqrt(K) for the sd of one side's K-seed mean.

| # | FEN | late-cut d12 | d16 | nodes d12 | d16 |
|---|---|---|---|---|---|
| 1 | `rn1qk2r/pb1pbppp/1p2pn2/8/Q1P5/5NP1/PP2PPBP/RNB2RK1 w kq - 2 9` | 0.31 | 0.37 | 0.22 | 0.28 |
| 2 | `r2qkb1r/pppn2pp/3p1nb1/3Pp1B1/2P1p3/2N3N1/PP3PPP/R2QKB1R w KQkq - 8 9` | 0.25 | 0.24 | 0.13 | 0.14 |
| 3 | `r1b2rk1/pppn1pp1/3ppq1p/8/1b1PP3/2N5/PPPQ1PPP/1K1R1BNR w - - 4 9` | 0.38 | 0.43 | 0.24 | 0.28 |
| 4 | `r1bqkb1r/pp2pppp/2n2n2/3p4/N2P4/5N2/PP1BPPPP/R2QKB1R w KQkq - 8 9` | 0.27 | 0.35 | 0.21 | 0.26 |
| 5 | `rn2k1nr/ppq1bpp1/2p1p3/2PpPb1p/3P3P/1QN5/PP3PP1/R1B1KBNR w KQkq - 1 9` | 0.24 | 0.36 | 0.12 | 0.25 |
| 6 | `rn1qkb1r/pp3ppp/2b1p1n1/2p1P3/3P4/1P1B1N2/P2P1PPP/RNBQ1RK1 w kq - 1 9` | 0.17 | 0.16 | 0.08 | 0.07 |
| 7 | `rnbqkbnr/pp3ppp/2p1p3/8/2B1P3/2N5/PP1BQPPP/R3K1NR w KQkq - 2 9` | 0.44 | 0.66 | 0.25 | 0.39 |
| 8 | `r1b1kb1r/pp1p1ppp/1q2pn2/3n4/3P1B2/1Q2PN2/PP3PPP/RN2KB1R w KQkq - 3 9` | 0.22 | 0.41 | 0.11 | 0.24 |
| 9 | `rnbqk2r/pp3ppp/8/2PpP3/1b4n1/2N5/PP2NPPP/R1BQKB1R w KQkq - 1 9` | 0.30 | 0.33 | 0.23 | 0.26 |
| 10 | `rn1q1rk1/2p2ppp/bp2pn2/p2p4/1bPP4/1P3NP1/P2BPPBP/RN1Q1RK1 w - - 0 9` | 0.42 | 0.51 | 0.32 | 0.36 |
| 11 | `rn1q1rk1/pb2bppp/1p1ppn2/2p5/4P3/2PP1NP1/PP3PBP/RNBQR1K1 w - - 1 9` | 0.29 | 0.38 | 0.22 | 0.30 |
| 12 | `r1bqk2r/1pp1bppp/p3pn2/4N3/2pn4/6P1/PP1NPPBP/R1BQ1RK1 w kq - 2 9` | 0.34 | 0.25 | 0.23 | 0.16 |
| 13 | `rn1qkb1r/3b1ppp/1p2pn2/1BPp4/1P6/4PN2/P4PPP/RNBQK2R w KQkq - 2 9` | 0.18 | 0.14 | 0.08 | 0.07 |
| 14 | `rnbqkb1r/pp3ppp/5n2/8/2pNp3/6P1/PP2PPBP/RNBQK2R w KQkq - 0 9` | 0.31 | 0.29 | 0.22 | 0.15 |
| 15 | `r3k1nr/pp1qpp1p/2np2p1/2pP4/2P1P3/2b2N2/PP3PPP/R1BQK2R w KQkq - 0 9` | 0.20 | 0.50 | 0.06 | 0.26 |
| 16 | `r1b1kb1r/p1p1q1pp/2p2n2/3p1P2/4p2N/2N5/PPPPQPPP/R1B1K2R w KQkq - 0 9` | 0.33 | 0.37 | 0.22 | 0.22 |
| 17 | `r1bq1rk1/pppn1ppp/4pb2/8/3P4/6P1/PPP1NPBP/R1BQK2R w KQ - 2 9` | 0.23 | 0.32 | 0.15 | 0.23 |
| 18 | `rnbq1rk1/1p2bppp/p2p1n2/4p3/2B1P3/1NN1B3/PPP2PPP/R2QK2R w KQ - 4 9` | 0.37 | 0.32 | 0.16 | 0.18 |
| 19 | `r3kb1r/ppqn1ppp/2p1pn2/3p4/4P3/3P1QPP/PPPN1PB1/R1B1K2R w KQkq - 3 9` | 0.35 | 0.49 | 0.23 | 0.34 |
| 20 | `rn1q1rk1/ppp1ppbp/2b2np1/8/2QP1B2/2N2N2/PP2PPPP/R3KB1R w KQ - 5 9` | 0.43 | 0.53 | 0.23 | 0.33 |
| 21 | `r1bq1rk1/pp2ppbp/n5p1/2Pn4/8/5NP1/PP2PPBP/RNBQ1RK1 w - - 1 9` | 0.47 | 0.44 | 0.26 | 0.28 |
| 22 | `r1b1k2r/pp3ppp/1qn1pn2/3p4/1bPN4/4P1P1/PP1B1PBP/RN1QK2R w KQkq - 0 9` | 0.30 | 0.31 | 0.18 | 0.18 |
| 23 | `r1bq1rk1/pp1pppbp/2n3p1/1B1nP3/3P4/5N2/PP3PPP/RNBQ1RK1 w - - 1 9` | 0.24 | 0.54 | 0.15 | 0.35 |
| 24 | `r1b1kb1r/1p1n1ppp/pq1pp3/2p3B1/3PP3/2N2N2/PPP2PPP/R2Q1RK1 w kq - 2 9` | 0.36 | 0.35 | 0.22 | 0.23 |
| 25 | `r1bqnrk1/pp2bppp/2np4/2p1p3/2P5/2NP2P1/PP2PPBP/R1BQNRK1 w - - 3 9` | 0.30 | 0.34 | 0.20 | 0.25 |
| 26 | `r1bq1rk1/p2pnpbp/1pn1p1p1/2p5/2P5/P1NP2PN/1P1BPPBP/R2QK2R w KQ - 0 9` | 0.36 | 0.29 | 0.25 | 0.18 |
| 27 | `r2q1rk1/pppb1ppp/1bnp1n2/1B2p3/N3P3/2PP1N1P/PP3PP1/R1BQK2R w KQ - 1 9` | 0.24 | 0.31 | 0.14 | 0.19 |
| 28 | `rn1q1rk1/pb1pppbp/1p3np1/8/2PQ4/1PN1PN2/PB3PPP/R3KB1R w KQ - 1 9` | 0.34 | 0.40 | 0.18 | 0.23 |
| 29 | `rn2k2r/p2q1ppp/bp2pn2/2bp4/Q1P5/P3PN2/1P1N1PPP/R1B1KB1R w KQkq - 2 9` | 0.27 | 0.22 | 0.13 | 0.09 |
| 30 | `rn1qk2r/pb1pppbp/1n4p1/1Pp5/4P3/6B1/1PPN1PPP/R2QKBNR w KQkq - 3 9` | 0.28 | 0.23 | 0.17 | 0.17 |
| 31 | `rnb2rk1/pp3ppp/2q1pn2/2b5/2B5/5N2/PPPNQPPP/R1B1K2R w KQ - 6 9` | 0.45 | 0.52 | 0.28 | 0.39 |
| 32 | `r1bqk2r/1p3pbp/2np1np1/p1p1p3/2P5/P1N2NP1/1P1PPPBP/1RBQ1RK1 w kq - 0 9` | 0.26 | 0.36 | 0.18 | 0.23 |
| 33 | `rn2kb1r/pbpp1ppp/1p2pq2/8/2PPP3/P7/1P1N1PPP/R1BQKB1R w KQkq - 1 9` | 0.35 | 0.37 | 0.20 | 0.28 |
| 34 | `r2qk2r/pppnbppp/2np4/4p2b/4P3/1BPP1N1P/PP1N1PP1/R1BQK2R w KQkq - 1 9` | 0.27 | 0.36 | 0.20 | 0.23 |
| 35 | `r1bq1rk1/1ppnbppp/2n5/p2pp3/8/2PP1NP1/PPQNPPBP/R1B2RK1 w - - 4 9` | 0.36 | 0.35 | 0.21 | 0.20 |
| 36 | `rnbq1rk1/pp3ppp/4pn2/2p5/2BP4/P1P1P3/5PPP/R1BQK1NR w KQ - 0 9` | 0.52 | 0.36 | 0.36 | 0.25 |
| 37 | `rnb2rk1/pp1p1pbp/1q3np1/2pp4/2P5/4PNP1/PP3PBP/RNBQ1RK1 w - - 0 9` | 0.35 | 0.41 | 0.16 | 0.26 |
| 38 | `r1b1k2r/pp2bppp/1q1ppn2/2p5/2PnP3/P1N2N2/1P1PBPPP/1RBQ1RK1 w kq - 1 9` | 0.28 | 0.43 | 0.16 | 0.30 |
| 39 | `r1bqkb1r/1p1p1ppp/p1n1p3/8/3pP3/1P6/PBPPBPPP/RN1Q1RK1 w kq - 2 9` | 0.42 | 0.30 | 0.33 | 0.21 |
| 40 | `rnbq1rk1/pp4pp/2pbpn2/5p2/2pP4/2NBPP2/PPQ1N1PP/R1B1K2R w KQ - 0 9` | 0.27 | 0.28 | 0.14 | 0.18 |
| 41 | `r1bq1rk1/p1pn1ppp/2pp4/4p3/1b1PP3/2N2N2/PPP2PPP/R1BQR1K1 w - - 1 9` | 0.18 | 0.49 | 0.10 | 0.26 |
| 42 | `r1bq1rk1/1pp1bpp1/p1np1n1p/4p3/B3P3/2P2N1P/PP1P1PP1/RNBQR1K1 w - - 0 9` | 0.51 | 0.49 | 0.26 | 0.29 |
| 43 | `rn1qkb1r/pp3ppp/4b3/8/4n3/5N2/PP1P1PPP/R1BQKB1R w KQkq - 0 9` | 0.34 | 0.21 | 0.19 | 0.14 |
| 44 | `rn1qkb1r/pp3ppp/3ppn2/2pP3b/8/2P2N1P/PP2BPP1/RNBQ1RK1 w kq - 1 9` | 0.29 | 0.45 | 0.16 | 0.29 |
| 45 | `r2qkbnr/pp1b1pp1/2n1p2p/2ppP2P/3P2P1/2P4B/PP3P2/RNBQK1NR w KQkq - 0 9` | 0.42 | 0.34 | 0.22 | 0.24 |
| 46 | `r3kbnr/ppqn1ppp/2p5/4p3/4P3/6P1/PPP1QPBP/R1B1K1NR w KQkq - 0 9` | 0.43 | 0.33 | 0.29 | 0.24 |
| 47 | `r2qk2r/pp1n1ppp/2p1pn2/3p4/1b2P3/2NP1QPP/PPP2PB1/R1B1K2R w KQkq - 1 9` | 0.31 | 0.23 | 0.16 | 0.13 |
| 48 | `rnbq1rk1/p1p2ppp/1p2pb2/8/2BPN3/5N2/PPP2PPP/R2QK2R w KQ - 0 9` | 0.42 | 0.43 | 0.25 | 0.31 |
| 49 | `r2qk2r/1p1nppbp/p1p2np1/3p4/PPPP2b1/2N1PN2/4BPPP/R1BQK2R w KQkq - 3 9` | 0.42 | 0.41 | 0.29 | 0.25 |
| 50 | `r1bqr1k1/pp2ppbp/n1pp1np1/P7/2PP4/5NP1/1P2PPBP/RNBQ1RK1 w - - 1 9` | 0.36 | 0.47 | 0.22 | 0.31 |
| 51 | `r1b1k2r/ppp1qp1p/2p2np1/8/3pP3/5N2/PPP2PPP/RN1Q1RK1 w kq - 0 9` | 0.33 | 0.63 | 0.15 | 0.40 |
| 52 | `rnbq1rk1/pp2b1pp/2pp1n2/5pB1/2PPp3/2N1P3/PP2NPPP/R2QKB1R w KQ - 0 9` | 0.44 | 0.21 | 0.25 | 0.14 |
| 53 | `r3kb1r/pp1n1ppp/2p1pn2/q2p4/8/1P1PPQ1P/PBPN1PP1/R3KB1R w KQkq - 2 9` | 0.37 | 0.29 | 0.21 | 0.18 |
| 54 | `rnbqk2r/p2pbppp/3n4/1ppP4/8/3B1N2/PP3PPP/RNBQ1RK1 w kq - 4 9` | 0.36 | 0.41 | 0.20 | 0.27 |
| 55 | `r1bq1rk1/pp2bppp/2n2n2/2pp4/3P4/2N2NP1/PPP2PBP/R1BQ1RK1 w - - 7 9` | 0.31 | 0.42 | 0.18 | 0.28 |
| 56 | `rnbqkb1r/pp1n1ppp/4p3/4P3/2Bp1P2/5N2/PP4PP/RNBQK2R w KQkq - 0 9` | 0.26 | 0.24 | 0.16 | 0.15 |
| 57 | `r2qkbnr/pp3ppp/2n1p1b1/3pP3/3P4/2N3N1/PP3PPP/R1BQKB1R w KQkq - 2 9` | 0.39 | 0.31 | 0.25 | 0.20 |
| 58 | `r1bqkb1r/1p1n1pp1/p2ppn1p/8/3NP2B/2N5/PPP1QPPP/R3KB1R w KQkq - 0 9` | 0.50 | 0.42 | 0.33 | 0.34 |
| 59 | `r1bq1rk1/1p2bppp/p1n1pn2/2pp4/2PP4/PPN1PN2/1B3PPP/R2QKB1R w KQ - 3 9` | 0.21 | 0.25 | 0.14 | 0.13 |
| 60 | `rnbqk2r/1p3ppp/2p1pn2/p5B1/PbPPN3/5N2/1P3PPP/R2QKB1R w KQkq - 1 9` | 0.32 | 0.54 | 0.15 | 0.33 |
| 61 | `r1b1k2r/pppn1pb1/3ppq1p/6p1/P2P4/2P2NP1/1P1NPP1P/R2QKB1R w KQkq - 1 9` | 0.66 | 0.69 | 0.45 | 0.53 |
| 62 | `rn1qk2r/pbp2ppp/1p2p3/3pP3/1b1P4/2nB1N2/PPP1QPPP/R1B2RK1 w kq - 0 9` | 0.22 | 0.26 | 0.11 | 0.11 |
| 63 | `rnbq1rk1/pp2nppp/2pb4/8/2pP4/3B1N1P/PP3PP1/RNBQ1RK1 w - - 0 9` | 0.29 | 0.36 | 0.21 | 0.24 |
| 64 | `r1b1k2r/p1qnppbp/2pp1np1/1p6/3PP3/2NBBN1P/PPPQ1PP1/R3K2R w KQkq - 1 9` | 0.62 | 0.54 | 0.39 | 0.39 |
| 65 | `r1b1k2r/ppq2ppp/2nbpn2/2p5/2pP4/1P1BPN2/PB3PPP/RN1Q1RK1 w kq - 0 9` | 0.34 | 0.44 | 0.20 | 0.32 |
| 66 | `r1bq1rk1/pppnppb1/3p2pp/7n/2BPP2B/2P2N2/PP1N1PPP/R2QK2R w KQ - 2 9` | 0.36 | 0.28 | 0.26 | 0.19 |
| 67 | `r1b2rk1/pp2ppbp/nq3np1/2pp4/3P1B2/1QP2NP1/PP2PPBP/RN3RK1 w - - 6 9` | 0.37 | 0.40 | 0.27 | 0.32 |
| 68 | `rnbqk1nr/1p2bp1p/p2pp1p1/8/4P1Q1/1N1B4/PPPN1PPP/R1B1K2R w KQkq - 0 9` | 0.32 | 0.32 | 0.18 | 0.18 |
| 69 | `r1bq1rk1/pp3pbp/2np1np1/2p1p3/2P5/1PN2NP1/PB1PPPBP/R2Q1RK1 w - - 2 9` | 0.42 | 0.53 | 0.27 | 0.42 |
| 70 | `r1bqk1nr/1pp2p2/3p2pb/p1nPp2p/2P1P2P/2N2N2/PP2BPP1/R1BQK2R w KQkq - 4 9` | 0.42 | 0.50 | 0.15 | 0.28 |
| 71 | `r1bqkb1r/5ppp/p1nppn2/1p6/2B1PB2/2NQ1N2/PPP2PPP/R3K2R w KQkq - 0 9` | 0.46 | 0.61 | 0.22 | 0.33 |
| 72 | `r1bq1rk1/ppp1bppp/2n2n2/3p4/3P4/P1N2NP1/1P2PPBP/R1BQK2R w KQ - 0 9` | 0.41 | 0.60 | 0.30 | 0.44 |
| 73 | `r2qkb1r/pp2pppp/2n2n2/3P2B1/2p5/2N2b2/PP3PPP/R2QKB1R w KQkq - 0 9` | 0.17 | 0.15 | 0.06 | 0.07 |
| 74 | `rn1q1rk1/pbp2ppp/1p1ppn2/8/2PP4/P2BPN2/1P1Q1PPP/R1B1K2R w KQ - 0 9` | 0.34 | 0.52 | 0.21 | 0.34 |
| 75 | `rnbqkb1r/5ppp/p1npp3/1p4P1/3NP3/2N1B3/PPP2P1P/R2QKB1R w KQkq - 0 9` | 0.44 | 0.36 | 0.28 | 0.25 |
| 76 | `rn1qk2r/pp3ppp/2pb1n2/3p4/3P1B2/2NQP3/PP3PPP/R3K1NR w KQkq - 1 9` | 0.27 | 0.23 | 0.19 | 0.16 |
| 77 | `rn1q1rk1/pb1p1ppp/1p2pn2/2p5/1PP5/P1Q2NP1/3PPP1P/R1B1KB1R w KQ - 1 9` | 0.58 | 0.49 | 0.37 | 0.34 |
| 78 | `rnb1kb1r/1p3pp1/pq1p1n1p/4p3/3NP1P1/P1N2P2/1PP4P/R1BQKB1R w KQkq - 0 9` | 0.28 | 0.25 | 0.16 | 0.14 |
| 79 | `r1b1k2r/ppp1nppp/2p2q2/8/2BbP3/2N5/PPPQ1PPP/R1B1K2R w KQkq - 4 9` | 0.34 | 0.35 | 0.22 | 0.23 |
| 80 | `rq2kbnr/pp1b1ppp/2n1p3/1N1p4/4P3/BP6/P1P2PPP/RN1QKB1R w KQkq - 0 9` | 0.09 | 0.17 | 0.03 | 0.07 |
| 81 | `r1bq1rk1/p3bppp/1pn1pn2/2pp4/4P3/2PP1NP1/PP2QPBP/RNB2RK1 w - - 0 9` | 0.28 | 0.39 | 0.26 | 0.28 |
| 82 | `r2qk2r/1pp2ppp/p1p2n2/4p3/1b2P3/2NP1b1P/PPP2PP1/R1BQK2R w KQkq - 0 9` | 0.62 | 0.54 | 0.37 | 0.35 |
| 83 | `r1b1kb1r/p1qp2pp/2p1ppn1/2p1P3/8/3P1N2/PPPBQPPP/RN2K2R w KQkq - 2 9` | 0.17 | 0.32 | 0.05 | 0.16 |
| 84 | `rn1qk2r/pp3ppp/2ppbn2/8/2Pp4/1P2P1P1/P2Q1PBP/RN2K1NR w KQkq - 0 9` | 0.39 | 0.20 | 0.17 | 0.13 |
| 85 | `r2qk2r/pp1nbppp/2p1p3/3p1b2/2PPn3/1PN2NP1/P3PPBP/R1BQ1RK1 w kq - 3 9` | 0.44 | 0.36 | 0.25 | 0.26 |
| 86 | `r3kb1r/pp1qnp1p/2n1p1p1/2pp4/4PP2/2NP1N2/PPP3PP/R1BQ1RK1 w kq - 0 9` | 0.34 | 0.44 | 0.23 | 0.32 |
| 87 | `r1bqk2r/pp2n1pp/2np1b2/2p1pp2/2P5/1PN1PN2/PB1PBPPP/R2Q1RK1 w kq - 3 9` | 0.43 | 0.48 | 0.24 | 0.32 |
| 88 | `rn1q1rk1/pbp1bpp1/1p2pn1p/3p4/2PP3B/1QN1PN2/PP3PPP/R3KB1R w KQ - 2 9` | 0.74 | 0.43 | 0.32 | 0.22 |
| 89 | `r1bqkb1r/p2n1ppp/2p1p3/3pP3/8/5N2/PPP2PPP/R1BQKB1R w KQkq - 0 9` | 0.33 | 0.17 | 0.19 | 0.10 |
| 90 | `r1bq1rk1/ppp2pbp/2np1np1/8/2Pp4/1PN1PN2/PB2BPPP/R2QK2R w KQ - 0 9` | 0.37 | 0.48 | 0.24 | 0.34 |
| 91 | `r1bqkb1r/5ppp/p1p1pn2/3p4/2P1P3/3B4/PP3PPP/RNBQ1RK1 w kq - 1 9` | 0.43 | 0.42 | 0.32 | 0.30 |
| 92 | `r1bqkb1r/ppn3pp/2n2p2/2p1p3/1P6/P1N2NP1/3PPPBP/R1BQK2R w KQkq - 0 9` | 0.30 | 0.27 | 0.21 | 0.15 |
| 93 | `rnb2rk1/pp2ppbp/1qp2np1/8/2pP4/1QN2NP1/PP2PPBP/R1B2RK1 w - - 0 9` | 0.52 | 0.38 | 0.25 | 0.22 |
| 94 | `rn1qkb1r/pp1b1ppp/1n2p3/8/3P4/3B1N2/PP3PPP/RNBQK2R w KQkq - 1 9` | 0.42 | 0.40 | 0.28 | 0.28 |
| 95 | `rnbq1rk1/p3bppp/2p1pn2/1p1p4/2PP4/1Q3NP1/PP1BPPBP/RN3RK1 w - - 0 9` | 0.31 | 0.35 | 0.20 | 0.22 |
| 96 | `r1bq1rk1/p2n1ppp/1pp1pn2/3p4/1bPP4/1PN1PN2/P2BBPPP/R2QK2R w KQ - 0 9` | 0.17 | 0.30 | 0.09 | 0.21 |
| 97 | `r1b1kb1r/1pq2ppp/p1nppn2/8/P3PP2/1NN5/1PP3PP/R1BQKB1R w KQkq - 0 9` | 0.23 | 0.60 | 0.16 | 0.36 |
| 98 | `r1bqk2r/1pp2ppp/3p1n2/p1b1p3/2PnP3/2NP4/PP1NBPPP/R1BQ1RK1 w kq - 4 9` | 0.36 | 0.33 | 0.26 | 0.20 |
| 99 | `r2qk2r/pbpnppbp/1p3np1/8/4p3/3P1NP1/PPPN1PBP/R1BQR1K1 w kq - 0 9` | 0.30 | 0.38 | 0.24 | 0.27 |
| 100 | `rn1qk2r/pp3pp1/2p1p2n/3pPbbp/2PP3P/2N3P1/PP3P2/R2QKBNR w KQkq - 0 9` | 0.24 | 0.28 | 0.15 | 0.15 |
| 101 | `r1bqk2r/ppp1bpp1/4pn1p/8/3P4/4BN2/PPP2PPP/R2QKB1R w KQkq - 2 9` | 0.64 | 0.53 | 0.40 | 0.40 |
| 102 | `r1bqk2r/3pnpbp/2p3p1/p1p1p3/4P3/PPN2N2/2PP1PPP/R1BQ1RK1 w kq - 1 9` | 0.32 | 0.43 | 0.19 | 0.35 |
| 103 | `r2q1rk1/pp1n1ppp/2pbpn2/3p4/2P3b1/1P1PPN2/PB2BPPP/RN1Q1RK1 w - - 1 9` | 0.25 | 0.42 | 0.18 | 0.34 |
| 104 | `r1bqr1k1/ppp1ppbp/2n3p1/3p4/3Pn3/2N1BNPP/PPP1PPB1/R2Q1RK1 w - - 3 9` | 0.29 | 0.37 | 0.17 | 0.25 |
| 105 | `rnbq1rk1/p3b1pp/1pp1pn2/3p1p2/2PP4/1P3NP1/P1Q1PPBP/RNB2RK1 w - - 0 9` | 0.61 | 0.42 | 0.41 | 0.35 |
| 106 | `rnbq1rk1/4ppbp/p1pp1np1/1p6/2P5/2NP1NP1/PPQ1PPBP/R1B2RK1 w - - 0 9` | 0.77 | 0.51 | 0.49 | 0.39 |
| 107 | `r1bqk2r/pp1nppbp/2p3p1/3nN3/2BP3P/2N5/PPP2PP1/R1BQK2R w KQkq - 1 9` | 0.19 | 0.18 | 0.14 | 0.11 |
| 108 | `2r1kb1r/ppqn1ppp/3ppn2/2p5/4P3/N1P2N2/PP1P1PPP/R1BQR1K1 w k - 0 9` | 0.45 | 0.43 | 0.29 | 0.27 |
| 109 | `r1bq1rk1/pp1nppbp/3p1np1/3p4/2P5/2N2NP1/PP2PPBP/R1BQ1RK1 w - - 0 9` | 0.41 | 0.37 | 0.25 | 0.27 |
| 110 | `r2qk1nr/1pp3pp/p1pbbp2/8/3QP3/5N2/PPPN1PPP/R1B2RK1 w kq - 2 9` | 0.28 | 0.41 | 0.20 | 0.33 |
| 111 | `1rbq1rk1/1p1pppbp/p1n2np1/2p5/2P2P2/2NP2P1/PP1BP1BP/2RQK1NR w K - 2 9` | 0.26 | 0.58 | 0.17 | 0.46 |
| 112 | `r1bq1rk1/pppn1pbp/5np1/4p1B1/2B1P3/2P2N2/PP1N1PPP/R2QK2R w KQ - 0 9` | 0.33 | 0.32 | 0.19 | 0.25 |
| 113 | `rnb1k1nr/1p2ppbp/p5p1/q1P1P3/N6P/2p5/PP3PP1/R1BQKBNR w KQkq - 0 9` | 0.78 | 0.67 | 0.35 | 0.41 |
| 114 | `r1bq1rk1/pp1p1pbp/2n1p1p1/2p2n2/5P2/P1NP1NP1/1PP1P1BP/R1BQ1RK1 w - - 1 9` | 0.27 | 0.41 | 0.15 | 0.26 |
| 115 | `r1bqkb1r/1p3ppp/2n1pn2/p1Pp4/3P4/P4N2/1P1N1PPP/R1BQKB1R w KQkq - 0 9` | 0.25 | 0.29 | 0.12 | 0.18 |
| 116 | `rnbq1rk1/p2p1ppp/1p2p3/2b4n/2P2B2/P1N2N2/1PQ1PPPP/R3KB1R w KQ - 2 9` | 0.27 | 0.54 | 0.11 | 0.33 |
| 117 | `4k3/8/8/8/8/8/8/2rQK3 w - - 0 1` | 0.81 | 0.55 | 0.16 | 0.35 |
| 118 | `r4rk1/pp3ppp/8/8/8/8/PP3PPP/3RR1K1 w - - 0 1` | 0.34 | 0.45 | 0.24 | 0.36 |
| 119 | `5rk1/p4ppp/8/8/8/8/P4PPP/3RR1K1 w - - 0 1` | 0.43 | 0.24 | 0.25 | 0.14 |
| 120 | `7k/p7/1R5K/6r1/6p1/6P1/8/8 w - - 0 1` | 0.27 | 0.53 | 0.07 | 0.13 |
