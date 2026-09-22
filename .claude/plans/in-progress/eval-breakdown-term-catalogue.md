# Evaluation breakdown term catalogue — Design

**Issue:** #604

## Goal

The list of evaluation terms is maintained by hand in five places. `EvalBreakdown`'s 13 per-color
fields (`StratEngine/Eval.h:188-200`), the 13 assignments that fill them
(`StratEngine/Eval.cpp:1203-1221`), the 13 `send(eval_term_row(...))` calls plus the 14-operand
`net_sum` expression in `cmd_eval` (`StratEngine/UCIHandler.cpp:287-313`), the byte-identical copy of
that expression in `BreakdownWhitePov` (`StratChessTests/EvalTestFixture.h:477-486`), and the
`kBreakdownTerms` name list the reporting test sums over
(`StratChessTests/UCIReportingTests.cpp:234-236`). Adding a term means editing all five, and four of
the five fail *silently* when missed: a term left out of a sum simply contributes zero.

This is not hypothetical. The comment above `kBreakdownTerms` records that the list "passed for a
while with `bishops` and `castling` absent only because both were zero in the positions tested here"
— the honesty invariant was vacuous and nothing said so. `BreakdownWhitePov`'s own comment says it
was written once "so a new term is added to it in exactly one place and cannot be silently omitted",
which is the right instinct applied to one of the five places.

The already-demonstrated defect is the whole case; no claim about upcoming work is needed to carry
it. (Eval epic #110 is all but finished — 21 sub-issues, 20 closed, only #117 Texel tuning open —
so this is not urgent. It is simply cheap and already paid for once.)

The fix is to make the term list a single enumeration that every consumer iterates, so the four
derived sites cannot go stale.

## Scope

**This change will:**

- Add `EvalTerm` (a scoped enum over the 13 per-color terms) and a constexpr display-name catalogue
  to `StratEngine/Eval.h`.
- Replace `EvalBreakdown`'s 13 named `int[NUM_COLORS]` fields with one enum-indexed array plus
  enum-typed accessors, and move the white-POV summation onto the struct.
- Rewrite `cmd_eval`'s row printing and `net_sum` as loops over the catalogue.
- Delete `BreakdownWhitePov` and point its two callers at `EvalBreakdown::white_pov()`.
- Replace `kBreakdownTerms` with the shared catalogue.
- Update the term-level tests mechanically to enum access.

**This change will not:**

- Touch `Evaluate()`, `RawWhitePov()`, `BuildContext()`, `BlendPhase()` or any `eval_*` term
  function. Nothing on the per-node path changes, which is what keeps this off the bench gate.
- Change the printed `eval` output in any way — row order, names, column widths, the `endgame` dash
  row and the `sum (white pov)` line all stay byte-identical.
- Add, remove or retune an evaluation term.
- Introduce the review's `EvalTerms.def` X-macro variant, which reaches `RawWhitePov()` (see D3).
- Take on Candidates 2, 3 or 4 of the same review.

## Decisions

### D1: Enum-indexed array with a display-name catalogue, not `rows[] { name, white, black }`

The review proposes a row array carrying its own name string. Rejected: the term-level tests read
terms by identity, not by label — `terms.pst[color]` and `terms.mopup[color]` in
`StratChessTests/EvalPawnAndTaperTests.cpp`, and every term checked against its evaluator function in
`StratChessTests/EvalTermTests.cpp:846-853`. A rows-with-names shape forces those to look up a string
label, trading a compile-time coupling for a stringly one. Worse, it does not solve the stated
problem: a mistyped or missing label is exactly the silent failure being removed.

The enum is the single source of truth. The catalogue is **not** a parallel name array indexed by
the enum: an explicitly sized `std::array<const char*, NUM_EVAL_TERMS>` accepts too few initialisers
and value-initialises the rest to null, so a missing entry compiles cleanly and the printer and the
reporting test then agree on the same bad catalogue. Instead each entry carries both halves, the
array length is deduced, and the pairing is checked at compile time:

```cpp
struct EvalTermEntry {
	EvalTerm term;
	const char* name;
};

// Length deduced, so a missing entry changes std::size() and fails the assert below.
inline constexpr EvalTermEntry EVAL_TERMS[] = {
	{EvalTerm::Material, "material"}, /* ... */ {EvalTerm::KingAttack, "kingattack"}};

static_assert(std::size(EVAL_TERMS) == NUM_EVAL_TERMS, "every EvalTerm needs a catalogue entry");
static_assert(
    [] {
	    for (std::size_t i = 0; i < std::size(EVAL_TERMS); ++i) {
		    // Entry i must describe term i: catches an insertion or a reorder that
		    // silently re-labels every row below it.
		    if (static_cast<std::size_t>(EVAL_TERMS[i].term) != i)
			    return false;
		    if (EVAL_TERMS[i].name == nullptr || EVAL_TERMS[i].name[0] == '\0')
			    return false;
	    }
	    return true;
    }(),
    "EVAL_TERMS must be in enum order with a non-empty name for each term");
```

That is what makes I1's compile-error claim true rather than asserted. A missing entry, a duplicate,
a reorder and an empty name are each a build failure.

### D2: Enum access only — no per-term named accessors

Call sites become `terms.at(EvalTerm::Pst, WHITE)` rather than `terms.pst(WHITE)`. Rejected the 13
convenience accessors: each is a per-term line in `Eval.h` whose *absence* is silent, which
reintroduces a smaller copy of the problem. `at()` is compile-time checked and satisfies the
"keep typed access" requirement on its own. If the call sites prove genuinely noisy in review, named
accessors can be added later as pure sugar over `at()` — that is an additive change and does not
need this document revisited.

### D3: Hand-written enum and catalogue, not an X-macro `EvalTerms.def`

An X-macro would collapse the enum, the name table and the population site to one line per term.
Rejected on readability and proportionality alone: macro-generated declarations obscure the struct
for a cold reader and fight clang-format and clang-tidy, for a saving of roughly 13 lines. The
compile-time checks in D1 give the same completeness guarantee without the generation.

The bench gate is **not** part of this argument. An X-macro can expand the enum, the names and the
cold `Breakdown()` population without touching `RawWhitePov()` at all; it is the review's particular
sketch that reaches the per-node path, not X-macros as such. What permits skipping the bench pass is
the scope guard — no code the search executes changes — and nothing else.

### D4: `Breakdown()` populates one explicit `set(EvalTerm::X, white, black)` per term

Rejected an ordered braced initialiser of the array: C++ has no designated initialisers for array
elements, so positional init makes a misordering silent — the table would print plausible numbers
against the wrong names. Each `set()` names its term.

Population completeness is checked **structurally**, not by value. The earlier draft of this document
claimed the existing sum invariant would catch a forgotten `set()`, because the row reads zero while
`total` still includes the term. That is exactly the failure mode this document's own Goal cites as
having already happened: `bishops` and `castling` were absent and it passed, because both were zero
in the positions tested. A value-based equality cannot be a completeness check, and the direct
breakdown test covers only eight of the thirteen rows
(`StratChessTests/EvalTermTests.cpp:846-853`).

So `set()` records the term in an enum-indexed presence mask, and the struct can report on itself:

```cpp
std::uint16_t populated_{};  // bit i set once term i has been written
constexpr bool complete() const { return populated_ == (1u << NUM_EVAL_TERMS) - 1u; }
```

`Breakdown()` asserts `complete()` before returning, and a focused test asserts it on a returned
breakdown — the test, not the assert, is the gate, because Release ships with `assert` disabled.
Storage is explicitly value-initialised (`int terms_[NUM_EVAL_TERMS][NUM_COLORS]{};`) so an
unpopulated row is zero by construction rather than by luck of the caller's initialiser.

The mask costs two bytes in a struct the search never constructs and one OR per `set()` on a path
that runs once per interactive `eval` command.

### D5: `phase`, `endgame_scale`, `endgame_adjustment` and `total` stay as named scalar fields

They are not per-color term rows. `endgame_adjustment` in particular is net-only — the printed dash
row exists to say exactly that — and giving it a catalogue entry would require every consumer to
special-case it anyway. `white_pov()` adds it to the summed nets, in one place, as `net_sum` does
today.

### D6: `BreakdownWhitePov` is deleted, not kept as a forwarder

Its two callers (`StratChessTests/EvalEndgameTests.cpp:323`, `StratChessTests/EvalTermTests.cpp:881`)
move to `terms.white_pov()`. The helper existed only to centralise the expression that is now a
member function; keeping it would leave a second name for one thing.

## Assumptions I cannot verify from the code

None. The one thing that would live here — whether anything outside the repository parses the `eval`
command's table — is made moot by I2: the printed output is byte-identical, so no external consumer
can observe this change.

## Invariants

- **I1** — Adding an evaluation term requires editing exactly two places: the `EvalTerm` enum plus
  its `EVAL_TERMS` entry in `Eval.h`, and its `set()` line in `Breakdown()`. Every other consumer
  derives from the enumeration. Both omissions fail structurally, not by value: a missing, duplicated,
  reordered or empty catalogue entry is a **compile error** (D1); a missing `set()` leaves
  `EvalBreakdown::complete()` false and fails a focused test (D4).
- **I2** — `cmd_eval`'s output is byte-identical to `origin/main`'s for any position: same 13 rows in
  the same order under the same names, same column widths, same `endgame` dash row, same
  `sum (white pov)` line and value.
- **I3** — `Evaluate()` returns the same score for every position, and the search visits identical
  nodes at `Threads=1`.
- **I4** — The D9 honesty invariant stays non-vacuous: the reporting test sums *every* term the
  breakdown carries, because it iterates the same catalogue the printer does.

## Validation

Engine tier, but with no per-node work added — no bench pass and no Elo match, because no code the
search executes changes. `Evaluate()`, `RawWhitePov()`, `BuildContext()` and the `eval_*` term
functions are untouched; `EvalBreakdown` is read-only introspection that search never constructs
(`Eval.h:173-174`).

| Risk | Evidence that closes it |
|---|---|
| I2 (printed output drift) | Capture `position fen <x>` + `eval` output on `origin/main` and on the branch for the 7 FENs in `UCIReportingTests.cpp:245-255`; the diff must be empty. Drive it from a scratchpad script, not by eye. |
| I3 (search behaviour) | `Compare-SearchEquivalence.ps1 -After <worktree exe>` — identical node counts and best moves at `Threads=1`. Cheap, and it is the documented gate for a behaviour-preserving claim; #555 is why this is run rather than asserted. |
| I1, I4 (the maintenance goal actually holds) | Three local exercises, each reverted. **(a) Positive:** add a 14th dummy term to the enum, `EVAL_TERMS` and `Breakdown()`; it must appear in the printed table and the reporting test's sum with no other edit. **(b) Catalogue omission:** add the enumerator but not its `EVAL_TERMS` entry, then separately swap two entries — each must **fail to compile**. **(c) Population omission:** add the enumerator and its catalogue entry but not the `set()` line — the `complete()` test must fail. Without (b) and (c) the two structural guarantees in I1 are claimed, not demonstrated. |
| Everything else | `Run-Tests.ps1` full fast tier, then `Validate-PrePR.ps1`. |

## Harvest

| Decision / rationale | Lands in |
|---|---|
| I1 — the two places a new term is edited, and why the rest derive | source comment on the `EvalTerm` enum in `Eval.h` |
| D1 — what each `static_assert` catches (omission, reorder, empty name) | the assert messages themselves, plus one comment above `EVAL_TERMS` |
| D4 — why population is `set()` per term rather than positional init, and why completeness is a mask rather than the sum invariant | source comment on `populated_` and in `Breakdown()` |
| D5 — why the endgame adjustment is not a catalogue row | already stated on the field in `Eval.h`; extend it by one clause |
| The stale-list history (`bishops`/`castling` silently absent) | the substance of the existing `kBreakdownTerms` comment, moved onto the catalogue-driven test |
| D2, D3 and their rejected alternatives | PR body |
| Any decision that changed during implementation | back into this table, before the PR |

## Implementation notes

Not part of the design; kept here because the document is being handed to another agent.

**`StratEngine/Eval.h`** — add `#include <array>` (Eval.h includes explicitly; it does not pull in
`StdAfx.h`). Shape:

```cpp
enum class EvalTerm : std::uint8_t {
	Material, Pawns, Rooks, Pst, Mopup, Bishops, Castling,
	Mobility, Outposts, KingShelter, KingStorm, KingFiles, KingAttack,
	COUNT
};
inline constexpr std::size_t NUM_EVAL_TERMS = static_cast<std::size_t>(EvalTerm::COUNT);
```

followed by `EVAL_TERMS` and its two `static_assert`s exactly as D1 gives them. Display names are
`const char*` so the UCI padding helpers take them unchanged, and the last four are deliberately not
the enum spellings — `shelter`, `storm`, `kingfiles`, `kingattack` are today's printed labels and I2
requires them unchanged.

`EvalBreakdown` keeps every existing comment; only the storage changes:

```cpp
int terms_[NUM_EVAL_TERMS][NUM_COLORS]{};          // value-initialised (D4)
std::uint16_t populated_{};                        // presence mask (D4)

constexpr int at(EvalTerm t, eColor c) const;      // one term, one color
constexpr int net(EvalTerm t) const;               // white minus black
constexpr void set(EvalTerm t, int white, int black);
constexpr bool complete() const;                   // every term written
constexpr int white_pov() const;                   // summed nets + endgame_adjustment
```

`NUM_EVAL_TERMS` is 13, so `std::uint16_t` has room; add a `static_assert(NUM_EVAL_TERMS <= 16)`
beside the mask so a 17th term fails the build rather than losing a bit.

The per-field comments at `Eval.h:188-200` (notably the `material` king-inclusive note) must survive
— hang them off the enumerators.

**`StratEngine/Eval.cpp:1203-1221`** — the `clang-format off` block becomes `EvalBreakdown out{};`
plus 13 `out.set(...)` lines in the same order, keeping the hand-aligned layout and the
`// clang-format off` fence, and `assert(out.complete())` before the return.

**`StratEngine/UCIHandler.cpp:287-313`** — the 13 `send()` calls become a loop over
`NUM_EVAL_TERMS`; `net_sum` becomes `terms.white_pov()`. `eval_term_row`/`eval_net_row`/`pad_right`
signatures are unchanged. The material-row comment at `:282-286` moves above the loop.

**`StratChessTests/UCIReportingTests.cpp:234-236`** — `kBreakdownTerms` is replaced by iteration over
`EVAL_TERMS`; keep the "endgame row absent on purpose" clause, drop the "list must stay complete"
warning, which the catalogue now makes structurally true.

**New focused test** (`EvalTermTests.cpp`) — `REQUIRE(eval.Breakdown(board).complete())` on one
position. One line, and it is what closes D4.

**Mechanical test updates** — `EvalTermTests.cpp`, `EvalPawnAndTaperTests.cpp`,
`EvalEndgameTests.cpp`: `terms.pst[WHITE]` → `terms.at(EvalTerm::Pst, WHITE)`.
