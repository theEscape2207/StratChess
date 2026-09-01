# Upgrading the match runner

How to move the pinned fastchess release forward. Roughly a twice-a-year procedure; the pin itself
and everything else about the instrument live in
[`../Measurements/README.md`](../Measurements/README.md).

Every automated use of fastchess is text-scraping its console output: `Run-EloMatch.ps1`'s Elo,
game-count, LLR and SPRT-verdict patterns, both harnesses' diagnostic classification, and
`pool_pentanomial.py`'s `Ptnml(0-2)` parse. A release that rewords a line degrades a run to
"inconclusive" or refuses to pool a shard — failure modes that look like results. So the bump is
gated on the new binary's output, not on its changelog.

1. **Run the same short SPRT under both binaries**, same build on both sides:
   `-Games 20 -Sprt Custom -Elo0 0 -Elo1 200` (`-Sprt` cannot be combined with `-Smoke`). All four
   result patterns must still match — one that silently stops matching reports "inconclusive" rather
   than failing.
2. **Make the new build check the pooling arithmetic.** Feed its printed `Ptnml(0-2)` to
   `.github/scripts/pool_pentanomial.py` and require the pooled figure to reproduce the
   `Elo: x +/- y` the binary printed itself. Add that triple to `--self-test`.
3. **Re-extract the diagnostic wordings.** Both harnesses classify by fastchess's exact message
   strings, because a keyword sweep also matches engine output echoed into the log. The release
   archive ships `app/src`, so the authoritative list is one grep away:
   `grep -rhoE '"(Warning|Error);[^"]*"' app/src`. Keep `$pvWarnRe`/`$fatalRe` in `Run-EloMatch.ps1`
   byte-identical to `pv_warn_re`/`fatal_re` in `strength.yml`. A wording neither names is reported
   as an unclassified diagnostic rather than silently tolerated — a prompt to update the lists, not
   a substitute for doing it.
4. **Provoke the warning classes rather than hoping a match emits them.**
   `.github/scripts/fastchess_probe_engine.py` is a mock UCI engine that reports an illegal PV
   (`badpv`) or plays an illegal move (`illegalmove`); two of them playing each other over two games
   is enough. The first must be counted and tolerated, the second must fail the run — under the old
   binary as well as the new one, so that any difference is attributable to the version.

Then update the pin in `.github/workflows/strength.yml`, the pinned-components table in
[`../Measurements/README.md`](../Measurements/README.md), and the local
`EngineTesting\fastchess.exe`.

## Traps

- The Windows zip and the Linux tar both extract to a **subdirectory**. The CI fetch step depends on
  that and breaks quietly if a release changes it.
- The calibration runs append rows to a ledger — [`../Measurements/local.md`](../Measurements/local.md)
  locally, [`ci-calibration.md`](../Measurements/ci-calibration.md) in CI. Revert them: both sides are the
  same binary, so they carry no strength information.
- Elo history stays comparable only while the time control and adjudication settings are untouched.
  That is why they are their own rows in the pinned-components table.
