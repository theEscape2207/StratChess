# Static-Eval Introspection: UCI `eval` + Batch FEN Scoring

**Issue**: #129 · **Epic**: #110 Tier 1 (enabler) · **Depth**: full plan · **Status**: not started

## Goal

Make the static evaluation observable:

1. A UCI-mode `eval` command printing the static score for the current position (interactive debugging).
2. A CLI batch mode scoring a file of FENs, one static eval per line, machine-parseable (bulk use by
   #117's tuner and by any before/after score-identity check).
3. A per-term breakdown — **deferred to a follow-up PR gated on #127**, see D3.

**Scope limit**: read-only introspection. No change to any score, no new eval term, no change to the
UCI protocol surface a GUI depends on.

## Why this is a Tier 1 enabler

There is currently no way to see what the evaluator thinks, at any granularity, without a debugger.
`UciHandler::run` (`StratEngine/UCIHandler.cpp:261-268`) dispatches exactly `uci`, `isready`,
`ucinewgame`, `position`, `go`, `setoption`, `stop`, `quit`. `EvalComplex::Evaluate` returns a single
`int` and keeps every intermediate in a local `bonusScore[2]`.

Three consequences:

- **#117 (Texel tuning) cannot be built without batch scoring.** A tuner needs static eval over
  hundreds of thousands of labelled quiet positions. Driving that through `go depth 1` is both wrong
  (quiescence and search bounds contaminate the number — note `Evaluate()` is only ever called from
  `AIPerplex::quiescence`, so a depth-1 search score is a qsearch result, not a static eval) and
  far too slow.
- **#127 (EvalContext restructure) needs a cheap score-identity check.** Its whole claim is
  "behaviour-preserving"; proving that means scoring the same corpus before and after and diffing.
  Without batch scoring, that verification is a bespoke throwaway harness.
- **Every new term needs contribution-level verification.** `EvalTests.cpp` can currently only assert
  whole-position scores and A-vs-B deltas between two hand-built FENs — which is exactly why the
  existing mop-up tests are all written as deltas rather than direct assertions.

Most engines ship this (Stockfish's `eval`). It is the smallest change in the epic with the widest
leverage.

## Design decisions

**D1 — Own the `EvalManager` instance in `UciHandler`; do not reach into `ai_`.** `EvalManager` and its
subclasses are documented in `Eval.h` as holding no mutable state, `Evaluate()` is `const`, and a
single instance is already shared unsynchronized across all Lazy SMP helper threads. So a second
instance is free and avoids adding an accessor to `PlayerAiBase` purely for debugging. Create it with
`EvalManager::Create(EvalManager::EvalTypes::COMPLEX)` — matching what `init_ai` configures
(`UCIHandler.cpp:48`) — and hold it as a member alongside `board_`.

**D2 — `eval` operates on `board_`, the position `position` already set up.** No FEN argument on the
UCI command: `position fen … ` then `eval` is the natural flow and reuses `cmd_position`'s parsing
rather than duplicating it. A FEN argument form (`eval fen <fen>`) is a convenience that can come later
if wanted; it is not needed by anything in the epic.

**D3 — Ship the total score and batch mode first; defer the per-term breakdown until #127 lands.**
This is the plan's main sequencing decision. A breakdown requires per-term values, which today exist
only as intermediates inside one 130-line function. There are two ways to get them now, and both are
worse than waiting:

- Reimplement the terms in a debug path → guaranteed to drift out of sync with the real `Evaluate()`,
  producing a debugging tool that lies. Unacceptable for something #117 will trust.
- Refactor `Evaluate()` to emit them → that *is* #127, done under a different issue number and without
  #127's byte-identity discipline.

So: **#129 phase 1** = total score + batch mode (which is what #127 needs to verify itself), then
**#127**, then **#129 phase 2** = the breakdown, derived from #127's per-term functions. This ordering
also resolves the apparent circular dependency between the two issues. Split the issue's checklist
accordingly rather than leaving phase 2 implicit.

**D4 — Batch mode as a CLI subcommand, not a UCI command.** `StratChessEvolved.cpp`'s `main()` already
has this exact pattern for `perft`, `tactical`, `test-fen` and `game`. `StratChessEvolved.exe eval <path>`
fits it, avoids the UCI read loop entirely (throughput matters at corpus scale), and keeps a bulk
data-processing mode out of a protocol handler. Note `main()` routes no-args and `uci` into
`UciHandler::run()`, and anything unrecognised falls through to `Game::Run()` — so the new branch must
be added *before* that fallthrough or it will silently start a game.

**D5 — Output formats.** Interactive `eval`: human-readable, side-to-move-relative total, plus a line
stating which color is to move (the sign convention is the single most confusing thing about this
engine's score and should not require reading source to interpret). Batch: `<fen>\t<score>` per line,
nothing else on stdout — no banner, no progress, so the file is directly consumable. Errors (unparsable
FEN) go to stderr with the line number, and the run continues rather than aborting a 200k-line corpus
on one bad line.

**D6 — UCI conformance.** `eval` is non-standard; unknown commands are already ignored silently, so
adding it is purely additive and cannot affect a GUI. It must not be advertised in `cmd_uci()`'s option
list.

## Files changed

| File | Change |
|---|---|
| `StratEngine/UCIHandler.h` | `cmd_eval()` declaration; `std::unique_ptr<EvalManager> eval_` member |
| `StratEngine/UCIHandler.cpp` | `cmd_eval()`; dispatch in `run()`; construct `eval_` |
| `StratChessEvolved/StratChessEvolved.cpp` | `evalrunner()` + `eval` branch in `main()` before the `Game::Run()` fallthrough |
| `StratChessTests/UCITests.cpp` | `[uci]` cases (see Validation) |
| `Docs/TestDesign.md` | §UCI case list |
| `Docs/Changelog.md` | Dated entry |

## Step-by-step

### 1. `UciHandler` — the member and the command

Add `std::unique_ptr<EvalManager> eval_;` to the header and initialise it in the constructor (not in
`init_ai()` — `init_ai` is re-run by `cmd_ucinewgame()` and the evaluator has no per-game state to
reset). `Eval.h` is already included by `UCIHandler.cpp`.

`cmd_eval()`: evaluate `board_`, print the total with an explicit perspective line, e.g.

```
static eval: 34 cp (from White's point of view — White to move)
```

Report the score *and* the color, converting the side-to-move-relative value to a fixed White-relative
figure as well if that is clearer; decide one convention and document it in the function comment. Do
not print `info` — this is not a search response and must not look like one to a GUI.

Dispatch in `run()` as `else if (line == "eval")`, placed with the other exact-match commands.

### 2. Batch mode in `main()`

`static int evalrunner(int argc, char** argv)` following `perftrunner`'s shape: read the FEN file line
by line, `board.SetupFromFEN(line)`, evaluate, print `fen\tscore`. Skip blank lines and `#` comments.
On a malformed FEN, write the line number to stderr and continue (D5).

Two known FEN-parser hazards apply and must be handled rather than inherited: a missing side-to-move
field silently defaults to Black (#46), and illegal positions are silently accepted (#45). For a tuning
corpus that means garbage in, garbage fitted. Reject lines whose FEN has fewer than the expected fields
with an explicit stderr warning — the corpus is exactly the situation where silent defaults do the most
damage.

Add the branch in `main()` **before** the final `Game game; game.Run();` fallthrough.

### 3. Tests (`UCITests.cpp`)

- `eval` is recognised and prints a line matching the expected shape.
- **The invariant that keeps this honest**: the score printed for a FEN equals
  `EvalManager::Create(COMPLEX)->Evaluate(Board(fen))` for that same FEN. This is the assertion that
  makes the tool trustworthy for #117 and #127, and it is the one that would catch a future drift
  between a breakdown path and the real evaluator.
- `eval` before any `position` command works on the startpos default without crashing.
- `eval` does not emit `bestmove` or `info` (a GUI must not mistake it for a search response).

Follow the existing `UCITests.cpp` conventions for driving the handler.

## Validation plan

```powershell
.\build.ps1 all
.\build.ps1 run-tests "[uci]"
.\build.ps1 run-tests                # full fast tier
```

Manual, from `StratChessEvolved/` (working directory matters — `game_settings.json` and `logs/`):

```bash
(printf "uci\nisready\nposition fen 8/8/8/3r4/4k3/8/8/3QK3 w - - 0 1\neval\nquit\n" \
  | ../x64/Release/StratChessEvolved.exe) 
```

Batch mode against a small hand-built FEN file, including a deliberately malformed line to confirm it
warns and continues rather than aborting.

**No ELO match** — this changes no score. State that in the PR body.

**Pre-PR**: `Scripts\Validate-PrePR.ps1` (source diff). `eval-reviewer` dispatch is **not** required by
CLAUDE.md's checklist here as long as `Eval.cpp`/`Eval.h` are untouched — which is a deliberate
property of this plan (D1/D3). If the implementation ends up modifying the evaluator, that is a signal
it has drifted into #127's scope; stop and reconsider rather than dispatching the reviewer and
proceeding.

## Key correctness properties

1. **Zero score impact**: no evaluation value changes. Verifiable by node-count-identical search
   results at fixed depth on a set of positions.
2. **Single source of truth**: the printed score comes from the same `Evaluate()` call the search uses
   — never a parallel computation. (The phase-2 breakdown must inherit this from #127's per-term
   functions.)
3. **Sign convention is stated, not implied**: output makes the perspective explicit.
4. **UCI conformance preserved**: no new advertised option; no `info`/`bestmove` from `eval`; GUIs
   unaffected.
5. **Batch output is machine-clean**: stdout contains only `fen\tscore` lines; diagnostics go to
   stderr.
6. **Corpus hygiene**: malformed or side-to-move-less FENs are reported, not silently scored from a
   defaulted position (#45/#46).
