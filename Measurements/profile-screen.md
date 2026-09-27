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

- **Per position, the noise is large.** At the median, one seed change moves a position's late-cut
  work by 0.30 (d12) or 0.40 (d16) in log sd, and nodes by 0.18 or 0.27. Single positions swing
  from -75% to +440%. Endgames are noisier (median 0.44 at d12).
- **Positions are close to independent.** The sd of the screen statistic over seeds is 1.0-1.13
  times the value predicted from independent positions. A seed does not shift all positions
  together, so the band shrinks as 1/sqrt(positions × K). That is also why the script's ±2 SE,
  which assumes independence, covers 91-95% of the null splits.
- **Noise does not carry across depths.** A seed's deviation at d12 predicts its deviation at d16
  with r = 0.09 (nodes 0.13). A position's noisiness does carry over (Spearman 0.42-0.56). Depth 16
  therefore adds an independent sample. It does not confirm depth 12.
- **Robust statistics do not help.** The median and the 20%-trimmed mean have the same band as the
  mean. The majority-sign rule has a null range of 0.34-0.66 at 38 positions and 0.41-0.59 at 120,
  so "most positions moved" means nothing at either size.
- **Earlier 38-position set** (#651's; 34 book positions and 4 endgames, 17 seeds): one run per side
  gives ±19% late-cut work (d12) and ±23% (d16), ±11% and ±13% for nodes. With K=8 it gives ±7%.

### Amplifiers

| Trait | Evidence | Kind |
|---|---|---|
| Root best-move change | An iteration where the root best move changes costs +25% (d12) and +34% (d16) more than the same iteration under other seeds. On the 38-position set: +32% and +43%. Positions whose final best move varies across seeds are the noisiest (Spearman 0.40 at d12, 0.50 at d16). | Inherent: a new best move means a re-searched root |
| LMR re-search share | Within a position, the seeds with more re-search nodes also do more late-cut work (Spearman 0.44 at d12, 0.56 at d16). | Inherent: the reduced-then-full re-search is a threshold |
| Short-PV early stop (#652) | Neutral seeds trigger it. On #652's two FENs, 4 of 68 seed runs stop, one of them on the same repetition PV as #651's candidate. In the wider book set, `r1bq1rk1/ppp3pp/2np1n2/4pp2/2P5/2PP2PN/P3PPBP/1RBQK2R w K - 5 9` stops at d16 on 6 of 9 seeds, **including generation order, so stock `main`**, and `r1bq1rk1/bpp2ppp/p1np1n2/4p3/2P5/2NPPNP1/PP3PBP/R1BQ1RK1 w - - 2 9` stops on 1 of 16. Iterations with a PV margin of 2 or less cost +31%. | **Discontinuity: an engine fix, tracked in #652** |
| Aspiration re-searches | They fail on under 3% of searches, and their fail count does not correlate with noise (Spearman -0.08 to +0.14). | Not an amplifier |
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
