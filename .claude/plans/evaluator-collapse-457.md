# Collapse evaluator selection to one concrete evaluator — Design

**Issue:** #457

## Goal

The engine carries a three-value `EvalManager::EvalTypes` selector, an abstract base with a factory,
and a second evaluator (`EvalSimple`) that nothing ships, tunes, benchmarks or measures. The cost is
real and recurring: every evaluator-interface change has to keep an enum, a factory, a rejection
path, heap ownership and a set of selection tests alive for a code path with no demonstrated user.
`SIMPLE` is not an oracle either — it shares the production material values and PST helpers, so the
tests that compare the two only assert properties already covered for the production evaluator.
Remove the alternative and the selector, and let the one evaluator that ships be a plain concrete
type.

## Scope

**This change will:**

- Replace the `EvalManager` / `EvalSimple` / `EvalComplex` hierarchy with one concrete `Evaluator`
  class holding the current `EvalComplex` behaviour verbatim.
- Remove `EvalTypes`, `EvalManager::Create`, `AIPerplexConfig::evaluator`,
  `create_search_evaluator`, `PlayerAiBase::SetEvalEngine`, `Config::PlayerConfig::eval`, the
  `evaluator_type` / `evaluator_name` helpers in `PlayerFactory.cpp`, and both `"eval"` keys in
  `StratChessEvolved/game_settings.json`.
- Own the evaluator **by value** at every site that currently holds a `unique_ptr` to the base
  (`AIPerplex`, `PlayerAiBase`, `UciHandler`).
- Update tests and `Docs/TestDesign.md` to the single evaluator, deleting the tests whose only
  subject is selection.

**This change will not:**

- Change any evaluation term, weight, table, scaling or tapering — no score may move.
- Change the UCI `eval` command's rows or format, or any other UCI output.
- Introduce a replacement seam (interface, injection, policy template) for a future second
  evaluator. When one is measured to be worth having, it can bring its own seam.
- Rewrite historical `Docs/Changelog.md` entries.

## Decisions

### D1: One concrete class, not a one-value enum or a retained interface

`EvalComplex` becomes `Evaluator`, a standalone class; `EvalManager`'s protected PST helpers
(`GetPositionalScore`, `getEvalBoard`) move into it, along with the Lazy SMP sharing-contract
comment, which is the load-bearing part of that base class. Rejected: keeping `EvalManager` as an
interface with a single implementation (that is the cost the issue is removing), and keeping a
one-value `EvalTypes` (a selector that cannot select).

The `virtual` on `Evaluate()` goes away. This removes an indirect call from the qsearch/eval leaf —
a plausible small win, not a claimed one; see Validation.

### D2: Ownership by value, and the name

`AIPerplex::evaluator_`, `PlayerAiBase::Eval` and `UciHandler::eval_` become `Evaluator` members by
value. The class is stateless (no data members beyond compile-time constants), so a value member is
free, removes an allocation and a null state, and keeps the "safe to share unsynchronized across
Lazy SMP threads" contract trivially true. `PlayerAiBase::Eval` is renamed `eval_` to match the
repo's member convention while it is being touched.

Rejected: keeping `unique_ptr<Evaluator>`. It would preserve a null state that nothing can produce.

### D3: `GetType()` survives as a non-virtual `"Complex"`

Two user-visible strings read it (`PlayerAiBase::getDescription`, and `search_description` in
`PlayerFactory.cpp`, which currently routes through `evaluator_name`). Keeping
`static constexpr const char* GetType()` returning `"Complex"` keeps both strings byte-identical,
which is an acceptance criterion, and lets `search_description` drop its enum parameter.

### D4: A stale `"eval"` key in an external JSON file is ignored, not rejected

`Config.cpp` reads `p.value("eval", 0)` and the parser ignores unknown keys. Deleting the field means
an old external `game_settings.json` that still carries `"eval": 2` keeps working and simply has no
effect — and, unlike today, an arbitrary integer can no longer be `static_cast` into an out-of-range
enum. Rejected: a warning on the unknown key, which would need a general unknown-key policy this
issue is not the place to introduce.

### D5: The #125 mirroring probe keeps reaching the PST helpers by derivation

`StratChessTests/EvalTestFixture.h`'s `EvalProbe` derives from `EvalManager` purely to expose the two
protected static helpers. Keep that shape: `Evaluator` is **not** `final`, the helpers stay
`protected static`, and `EvalProbe` derives from `Evaluator` (it needs no virtuals — drop its
`Evaluate`/`GetType` overrides). This preserves the direct mirroring regression coverage without
making evaluation internals public. Rejected: making the helpers public (widens production surface
for test convenience) and a `STRAT_ENABLE_TEST_ACCESS` friend (more machinery for the same reach).

## Assumptions I cannot verify from the code

- **No external tooling reads `"eval"` from `game_settings.json`.** The strength-lab and match
  scripts drive UCI, which never exposed evaluator choice, and the field is absent from every script
  in `Scripts/`. Verified by grep across the repo; an out-of-tree private config would be silently
  unaffected anyway under D4.
- **`AIPerplexConfig::evaluator` has no out-of-tree consumer.** `AIPerplexConfig` is a source-level
  service API introduced by #256 and never exposed through UCI; the repository is its only known
  client. Not verifiable beyond this tree — accepted as a deliberate source-contract break, which is
  what the issue asks for.

## Invariants

- The production evaluator's score is bit-identical for every position and setting: `Evaluate()`,
  `Breakdown()`, the UCI `eval` rows, and search results all unchanged.
- `getDescription()` still ends `\n\tEvaluation:\tComplex\n` for both the AIPerplex and legacy paths.
- The evaluator remains stateless — no data members beyond compile-time constants, `Evaluate()` and
  `Breakdown()` `const` — so one instance stays safe to share unsynchronized across Lazy SMP threads.
- No active source, test, shipped config, or current doc mentions `EvalTypes`, `NONE`, `SIMPLE`,
  `EvalSimple`, `EvalManager`, `AIPerplexConfig::evaluator`, `PlayerConfig::eval` or `SetEvalEngine`.
- Direct regression coverage for vertical Black PST mirroring (#125) still exists.

## Validation

Engine tier.

- `pwsh -File Scripts\Run-Tests.ps1` (fast tier) and a Debug-build test run — the evaluator is
  engine code and Release hides out-of-bounds reads.
- `pwsh -File Scripts\Compare-SearchEquivalence.ps1 -BaselineRef origin/main -After <exe>` must
  report identical per-iteration output and best moves at `Threads=1`. This is the gate: a
  difference means the collapse changed behaviour and is no longer a refactor — stop and investigate
  rather than re-baselining.
- `Scripts\Validate-PrePR.ps1` before the PR.

**No Elo match.** The evaluation formula is untouched and equivalence is proven exactly by
`Compare-SearchEquivalence`; an Elo match could only measure noise. A `Run-Bench.ps1` before/after
nps comparison is worth recording because D1 removes an indirect call from the eval leaf, but any
delta stays an nps figure — it must not be converted into an Elo claim.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Lazy SMP sharing contract, now stated for one concrete stateless `Evaluator` | class comment on `Evaluator` in `Eval.h` (moved, not rewritten) |
| Why `EvalProbe` derives rather than the helpers being public (D5) | comment on `EvalProbe` in `EvalTestFixture.h` |
| A stale `"eval"` key in an external config is ignored (D4) | PR body; `Docs/Changelog.md` entry |
| Single evaluator, no selection, in the test-writing guidance | `Docs/TestDesign.md` |
| nps before/after, if measured | PR body only — not an Elo claim |
