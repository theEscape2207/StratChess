# Multi-PV UCI surface — Design

**Issue:** #94 (slice 2 of 2, per the [2026-11-20 re-triage](https://github.com/theEscape2207/StratChess/issues/94#issuecomment-7052168048)); consumers `theEscape2207/StratChessUI#91` (`great` = an only move, dropped until this lands) and `theEscape2207/StratChessUI#68` (hint alternatives). Slice 1, PR #987, gave `AIPerplex::SetMultiPV` and `alternatives` on `SearchResult`/`IterationInfo`.

## Goal

The search can rank the top N root moves, but only a C++ caller can ask for it. A UCI client — the
web UI's verdict and hint code, or any analysis GUI — has no option to set N and no way to tell one
rank's line from another. This slice puts slice 1 on the protocol, the way GUIs already expect
(`MultiPV` option, `multipv k` field), and leaves single-PV output byte-identical.

## Review focus

- **D1, no `multipv` field at N = 1.** Every script and harness parses N = 1 output; is keying the
  field on the configured N (not on the result) right for the fallback and one-legal-move roots?
- **D3, the final block repeats the whole accepted set.** Is the last per-iteration block always
  the accepted set, so the final block can never disagree with it (slice 1 D6: rejected iterations
  emit nothing, and N > 1 never reaches `ACCEPTED_INTERRUPTED`)?

## Scope

**This change will:**

- advertise and apply a `MultiPV` spin option;
- print `multipv k` on every per-iteration and final `info depth` line when N > 1, one line per rank;
- correct `Scripts/bisect_uci_option.py`'s docstring, which says the engine has no MultiPV;
- add UCI protocol tests, and document the option and field in `Docs/EngineGuide.md` (including its
  Supported UCI contract) and `Docs/EngineContracts.md`.

**This change will not:**

- change the search, `SetMultiPV` or the result shape (slice 1);
- add a set-start rule under a clock (D5), `searchmoves`, per-rank `seldepth` or node counts;
- measure N > 1 fixed-time depth loss (#985 owns it), or add `UCI_LimitStrength`/`UCI_Elo` (#985).

## Decisions

### D1: `multipv k` goes after `seldepth`, and only when the configured N > 1

Field order follows the convention GUIs parse: `info depth D seldepth S multipv K score …`. The
handler reads `ai_->MultiPV()` at `go` and tags lines only when it is above 1. So N = 1 output is
unchanged byte for byte, and `Compare-SearchEquivalence.ps1`, `Run-Bench.ps1`, `UciDriver.ps1`,
the match harnesses and the existing reporting tests see nothing new. A search at N > 1 tags every
line, even when it ranks one move (one legal move, or slice 1's first-depth fallback): a client that
asked for ranks always gets a `multipv` field to key on.

Rejected: always printing `multipv 1`, as Stockfish does. Legal, but it changes every parsed line
for single-PV clients, which gain nothing from it. Rejected: tagging only when the result carries
alternatives. Then the same client would see the field come and go between searches.

### D2: Each accepted iteration prints one line per rank, in rank order

Rank 1 comes from `IterationInfo::score`/`pv`, ranks 2..k from `alternatives`. Every line in a
block shares the iteration's `depth`, `seldepth`, `nodes`, `nps`, `hashfull` and `time`:
the set is one iteration and slice 1 tracks no per-rank count. The lines are sent one after another
from the search thread, so no other output lands between them.

Rejected: per-rank `nodes` or `seldepth`. Nothing reads them, and they need search changes
(slice 1 D1 kept one `seldepth` per set).

### D3: The final block repeats the whole accepted set, with the final totals

At `bestmove` time the completion handler prints rank 1 (`multipv 1`, from `best_score`/`pv`)
and then each of `result.alternatives` as `multipv 2..k`, each with the final `nodes`, `nps`,
`hashfull` and `time`, as the single final line does today (#912). Then `info string` lines and
`bestmove` as now. `bestmove` is `result.best_move`, which slice 1 made the sorted rank 1, so it
always equals `multipv 1`'s first move.

Rejected: rank 1 alone on the final line. A client that reads the last block before `bestmove`
would have to splice ranks 2..k from an earlier block, and a lone `multipv 1` would look like the
fallback. Rejected: no final line at N > 1. It breaks #912's guarantee that the final line carries
the accepted line with the search's totals.

### D4: `MultiPV` is a supported spin, 1..218, applied like `Threads`

`option name MultiPV type spin default 1 min 1 max 218`, the max being `AIPerplex::MAX_MULTIPV`
(`MoveList::MAX_MOVES`, the most legal moves a position can have). `setoption` parses it as
`Threads` is parsed: idle only (refused mid-search), a non-numeric or empty value ignored, a value
out of range clamped by `SetMultiPV`, no `info string` echo, no TT clear, kept by `ucinewgame`.
It joins `Threads` and `Hash` in the Supported UCI contract, because the web UI depends on it.

Rejected: a lower cap such as 5. The search already clamps to the legal count, and the cost of a
big N is the asking client's choice; an analysis GUI's "show all moves" would silently get fewer.
Rejected: an `info string MultiPV n` echo like the tuning options. `Hash` echoes because rounding
changes the value; here every info line shows the ranks.

### D5: No set-start rule under a clock in this slice

A set the hard limit cuts is wasted (slice 1 D6), so under a short clock N > 1 completes fewer
depths. A "don't start a set past a fraction of the soft limit" rule needs that loss measured, and
#985 owns the measurement. The named consumers don't need it: #91 searches to depth 10 under a 2 s
cap and drops a capped search anyway, and hints search the position once.

Rejected: adding the rule now with a guessed fraction.

## Assumptions I cannot verify from the code

- **GUIs and the web UI key ranks on `multipv k` placed after `seldepth`.** The UCI spec lists the
  field and Stockfish prints it there. Not verified against the UI code, which doesn't parse it yet;
  #91/#68 will be written against this output.
- **No script parses N > 1 output.** Verified: `grep -rn MultiPV Scripts` on `origin/main` cd728b87 finds
  only `bisect_uci_option.py`'s docstring, which says the engine has no MultiPV; this slice corrects it.

## Invariants

- At N = 1 every UCI output line is unchanged (no `multipv` token anywhere).
- At N > 1 every `info depth` line carries `multipv k`; each block is ranks 1..k in order, k =
  1 + alternatives, with distinct first moves and non-increasing scores, one depth.
- The final block equals the last per-iteration block in depth, ranks, scores and PVs; only the
  counts and time may differ.
- `bestmove` equals the final `multipv 1` line's first PV move.

## Validation

**Engine tier** (`UCIHandler.cpp`), though the search is untouched.

- **Equivalence:** `Compare-SearchEquivalence.ps1 -After <worktree build>` against `origin/main`
  must be IDENTICAL: it proves the N = 1 lines unchanged, not only the search.
- **Tests** (`StratChessTests/UCIMultiPVTests.cpp`, `[uci][multipv]`):
  - `uci` advertises the option line exactly; `setoption` applies 3, clamps 0 to 1 and 999 to
    218, ignores `abc`, is refused mid-search, survives `ucinewgame`;
  - `go depth 5` at N = 3 from the start position: every block holds the invariants above; the
    final block equals the last iteration block; `bestmove` equals `multipv 1`;
  - N = 5 with 3 legal moves: blocks of 3;
  - a checkmated root at N = 3: one final `multipv 1` line with `pv 0000`, `bestmove 0000`;
  - the first-depth fallback through `go nodes` (slice 1's `4k3/8/8/8/8/8/8/RN1QKBNR` position):
    a single `multipv 1` final line;
  - N = 1: no `multipv` token in the output of `go depth 4`.
- **nps / Elo:** none. No per-node code changes and N = 1 output is identical; slice 1's N = 1
  bench result stands as recorded.

## Cost

- **Size:** 50–200 lines, about half tests. Files: `UCIHandler.cpp`, a new test file, the UCI test
  fixture (a `MultiPV` accessor), `Docs/EngineGuide.md`, `Docs/EngineContracts.md`,
  `Docs/Changelog.md`.
- **Blast radius:** Engine tier; the UCI output contract, gated so N = 1 cannot change.
- **Review:** one code review (170–270k tokens).

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1: field placement, and no field at N = 1 | `Docs/EngineGuide.md` → Interpret search output; source comment in `cmd_go` |
| D3: final block = accepted set; `bestmove` = rank 1 | `Docs/EngineContracts.md` → Telemetry and output; source comment on the final block |
| D4: option range and setoption behaviour; joins the supported contract | `Docs/EngineGuide.md` → Configure a run and Supported UCI contract |
| D5: no set-start rule; #985 measures the loss | `Docs/Changelog.md` entry |
