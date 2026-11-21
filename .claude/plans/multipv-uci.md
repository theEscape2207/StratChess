# Multi-PV UCI surface — Design

**Issue:** #94 (slice 2 of 2, per the [2026-11-20 re-triage](https://github.com/theEscape2207/StratChess/issues/94#issuecomment-7052168048)). Slice 1, PR #987, gave `AIPerplex::SetMultiPV` and `alternatives` on `SearchResult`/`IterationInfo`.

## Goal

The search can rank the top N root moves, but only a C++ caller can ask for it. A UCI client has no
option to set N and no way to tell one rank's line from another. This slice puts slice 1 on the
protocol in the form UCI analysis clients use (a `MultiPV` option, a `multipv k` field), and leaves
single-PV search output unchanged.

**Who it is for.** The owner commissioned this slice on 2026-11-21, naming the UI's #91 and #68.
Neither blocks on it today, so it serves general UCI analysis first. `theEscape2207/StratChessUI#91` shipped without `great` and defers it to MultiPV ("drop it
until MultiPV lands, then define it as an only move"); `theEscape2207/StratChessUI#68` chose one
single-PV hint, so it does not depend on this. **Acceptance scenario:** a client sends
`setoption name MultiPV value 3` and `go depth D` from the start position, and the last block
before `bestmove` is a coherent ranked set: three distinct moves, each with its own score and full
line, best first, with `bestmove` its rank 1.

## Review focus

- **D1, no `multipv` field at N = 1.** Every script and harness parses N = 1 output; is keying the
  field on the configured N (not on the result) right for the fallback and one-legal-move roots?
- **D3, the final block repeats the whole accepted set.** Is the last per-iteration block always
  the accepted set, so the final block can never disagree with it (slice 1 D6: rejected iterations
  emit nothing, and a multi-rank set is never accepted interrupted)?

## Scope

**This change will:**

- advertise and apply a `MultiPV` spin option;
- print `multipv k` on every per-iteration and final `info depth` line when N > 1, one line per rank;
- saturate `setoption`'s numeric parse, shared by `Threads`, `Hash` and `MultiPV` (D4);
- correct `Scripts/bisect_uci_option.py`'s docstring, which says the engine has no MultiPV;
- add UCI protocol tests, and document the option and field in `Docs/EngineGuide.md` (including its
  Supported UCI contract) and `Docs/EngineContracts.md`.

**This change will not:**

- change the search, `SetMultiPV` or the result shape (slice 1);
- add a set-start rule under a clock (D5), `searchmoves`, per-rank `seldepth` or node counts;
- measure N > 1 fixed-time depth loss (#985 owns it), or add `UCI_LimitStrength`/`UCI_Elo` (#985).

## Decisions

### D1: `multipv k` goes after `seldepth`, and only when the configured N > 1

Field order follows Stockfish's convention: `info depth D seldepth S multipv K score …`. The
handler reads `ai_->MultiPV()` once in `cmd_go`, captures it by value in both callbacks, and tags
lines only when it is above 1. `setoption` is refused mid-search, so N is fixed for the search. So
at N = 1 every `go` line is unchanged, and `Compare-SearchEquivalence.ps1`, `Run-Bench.ps1`,
`UciDriver.ps1`, the match harnesses and the existing reporting tests see nothing new. The one
N = 1 change is the added `option` line in the `uci` handshake. A search at N > 1 tags every line,
even when it ranks one move (one legal move, a terminal root, slice 1's first-depth fallback, an
emergency move): a client that asked for ranks always gets a `multipv` field to key on.

Rejected: always printing `multipv 1`, as Stockfish does. Legal, but it changes every parsed line
for single-PV clients, which gain nothing from it. Rejected: tagging only when the result carries
alternatives. Then the same client would see the field come and go between searches.

### D2: Each accepted iteration prints one line per rank, in rank order

Rank 1 comes from `IterationInfo::score`/`pv`, ranks 2..k from each `RootLine`'s own `score` and
full `pv`. Every line in a block shares the iteration's `depth`, `seldepth`, `nodes`, `nps`,
`hashfull` and `time`: the set is one iteration and slice 1 tracks no per-rank count. The search
thread sends a block's lines in rank order, but each is a separate send, and the input thread may
print `readyok` or a refusal between two of them. So a client assembles a set from `info depth`
lines by `depth` + `multipv`, ignoring other replies; and a `depth` + `multipv` pair repeats once,
in the final block (D3).

Rejected: per-rank `nodes` or `seldepth`. Nothing reads them, and they need search changes
(slice 1 D1 kept one `seldepth` per set).

### D3: The final block repeats the whole accepted set, with the final totals

At `bestmove` time the completion handler prints rank 1 (`multipv 1`, from `best_score`/`pv`)
and then each of `result.alternatives` as `multipv 2..k`, each with the final `nodes`, `nps`,
`hashfull` and `time`, as the single final line does today (#912). Then `info string` lines and
`bestmove` as now. `bestmove` is `result.best_move`, which slice 1 made the sorted rank 1, so it
always equals `multipv 1`'s first move, or `0000` with `pv 0000` when there is no move.

Rejected: rank 1 alone on the final line. A client that reads the last block before `bestmove`
would have to splice ranks 2..k from an earlier block, and a lone `multipv 1` would look like the
fallback. Rejected: no final line at N > 1. It breaks #912's guarantee that the final line carries
the accepted line with the search's totals.

### D4: `MultiPV` is a supported spin, 1..218, applied like `Threads`

`option name MultiPV type spin default 1 min 1 max 218`, the max being `AIPerplex::MAX_MULTIPV`
(`MoveList::MAX_MOVES`, the most legal moves a position can have). `setoption` parses it as
`Threads` is parsed: idle only (refused mid-search), a non-numeric, negative or empty value ignored
(the digit check), no `info string` echo, no TT clear, kept by `ucinewgame` — so a client that
reuses the process for single-PV play sets it back to 1. Any all-digit value clamps through
`SetMultiPV`: the shared parse reads `std::stoull` and saturates at `UINT_MAX` before the cast,
so `4294967296` clamps to 218 on every platform. Today `std::stoul` throws on Windows (value
ignored) but narrows to 0 on LP64 (clamped to 1); the saturation applies to `Threads` and `Hash`
too, where an absurd value then reaches the setter's own cap instead of 0. A value past
`unsigned long long` still throws and is ignored.
It joins `Threads` and `Hash` in the Supported UCI contract.

Rejected: a lower cap such as 5. The search already clamps to the legal count, and the cost of a
big N is the asking client's choice; an analysis GUI's "show all moves" would silently get fewer.
Rejected: an `info string MultiPV n` echo like the tuning options. `Hash` echoes because rounding
changes the value; here every info line shows the ranks.

### D5: No set-start rule under a clock in this slice

A set the hard limit cuts is wasted (slice 1 D6), so under a short clock N > 1 completes fewer
depths and spends more clock per move. A "don't start a set past a fraction of the soft limit" rule
needs that loss measured, and #985 owns the measurement. Analysis clients search to a depth or
without a limit, where the rule does nothing.

Rejected: adding the rule now with a guessed fraction.

## Assumptions I cannot verify from the code

- **Clients key ranks on the `multipv k` token.** The UCI specification defines the option and the
  field; Stockfish prints it after `seldepth`, and python-chess parses it by token, not position.
  How other GUIs treat an interleaved `readyok` is not verified; no GUI is exercised here.
- **No script parses N > 1 output.** Verified: `grep -rn MultiPV Scripts` on `origin/main` cd728b87
  finds only `bisect_uci_option.py`'s docstring. The tool's option discovery would accept
  `--option MultiPV` and then fail its acknowledgement check; the docstring will say it targets
  acknowledged tuning spins, and that the threshold it reads is in the option's own units, which a
  MultiPV rank gap (in score units) does not give.

## Invariants

- At N = 1 every `go` output line is unchanged (no `multipv` token anywhere); the handshake gains
  one `option` line.
- At N > 1 every `info depth` line carries `multipv k`; each block is ranks 1..k in order, k =
  1 + alternatives, with distinct first moves, non-increasing scores, one depth, and each rank's own
  score and full PV.
- Whenever an iteration block was printed, the final block equals the last one in depth, ranks,
  scores and PVs; only the counts and time may differ. A terminal root does print blocks: rank 1
  alone, `pv 0000`. Only an emergency move prints a final `multipv 1` line with no block before it.
- `bestmove` equals the final `multipv 1` line's first PV move, `0000` matching `pv 0000`.

## Validation

**Engine tier** (`UCIHandler.cpp`), though the search is untouched.

- **Search equivalence:** `Compare-SearchEquivalence.ps1 -After <worktree build> -BaselineRef cd728b87`
  must be IDENTICAL. It normalises `seldepth`, `nps`, `hashfull`, `time` and the final PV, so it
  proves the search unchanged, not the line format; the format is pinned by the tests below and the
  existing field-order and full-PV tests (`UCIGoReportingTests.cpp`).
- **Tests** (`StratChessTests/UCIMultiPVTests.cpp`, `[uci][multipv]`):
  - `uci` advertises the option line exactly, after `Hash`;
  - `setoption` applies 3; clamps 0 to 1, 999 and `4294967296` to 218; from 3, ignores `abc` and
    `-1`; is refused mid-search; survives `ucinewgame`;
  - the acceptance scenario, `go depth 5` at N = 3 from the start position: every block holds the
    invariants above; the final block equals the last iteration block; `bestmove` equals
    `multipv 1`; and the final ranks' scores and full PVs equal a fresh fixture's
    `SearchResult` at the same depth and N, so each rank shows its own `RootLine`;
  - a node budget at N = 3, `Threads = 1`, set from an unlimited run so it ends inside a later set
    (the pattern of `SearchMultiPVTests.cpp`'s stop-during-rank-N test): the final block repeats the
    last complete multi-rank block, with a depth below the cut set's;
  - N = 5 with 3 legal moves: blocks of 3;
  - one legal move at N = 3, fixed depth: `multipv 1` lines only, the final equal to the last block;
  - a checkmated root at N = 3: every line `multipv 1 … pv 0000`, the final equal to the last
    block, `bestmove 0000`;
  - the first-depth fallback, `4k3/8/8/8/8/8/8/RN1QKBNR w KQ - 0 1`, `MultiPV 218`, `go nodes 1`:
    one depth-1 `multipv 1` iteration line and a matching final `multipv 1` line, nothing else;
  - N = 1: no `multipv` token in the output of `go depth 4`.
- **nps / Elo:** none. No per-node code changes and N = 1 search is proved identical; slice 1's
  N = 1 bench result stands as recorded.

## Cost

- **Size:** 50–200 lines, about half tests (estimate). Files: `UCIHandler.cpp`, a new test file,
  the UCI test fixture (a `MultiPV` accessor and the `multipv` parse), `bisect_uci_option.py`,
  `Docs/EngineGuide.md`, `Docs/EngineContracts.md`, `Docs/Changelog.md`.
- **Blast radius:** Engine tier; the UCI output contract, gated so N = 1 `go` output cannot change.
- **Review:** one code review (170–270k tokens).

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1: field placement, and no field at N = 1 | `Docs/EngineGuide.md` → Interpret search output; source comment in `cmd_go` |
| D2/D3: blocks keyed by depth + multipv, final block = accepted set, `bestmove` = rank 1 | `Docs/EngineContracts.md` → Telemetry and output; source comment on the block printer |
| D4: option range, setoption behaviour, saturation; joins the supported contract | `Docs/EngineGuide.md` → Configure a run and Supported UCI contract; comment at the parse |
| D5: no set-start rule; #985 measures the loss | `Docs/Changelog.md` entry |
| N > 1 under a clock spends more per move (a cut set is wasted); the mate stop can end `go depth D` below D | `Docs/EngineGuide.md` → Configure a run |
