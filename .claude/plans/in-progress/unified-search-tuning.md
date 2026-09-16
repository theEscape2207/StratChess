# Unified search tuning — Design

**Status:** Approved; PR 1 in progress.  
**Origin:** Architecture reviews of 2026-09-15 (candidate 3) and 2026-09-16 (candidate 2).  
**Baseline:** `origin/main` at `76477a73e3bdeab1c1f8694598e40133666021f5`.

## Goal

Search experiments currently require changing defaults and rebuilding because UCI cannot set
`SearchTuning`. Game-mode configuration also duplicates defaults and copies a partial mirror into
the engine. Establish an extensible tuning model for upcoming experiments, with six shipping UCI
controls and one build-gated experimental control. Future maintainability and extensibility take
precedence over minimizing implementation cost; default search behavior and measured search
performance must remain at parity.

## Scope

**This change will:** describe every existing tuning field in one authoritative catalogue, derive
typed settings and configuration bindings from it, expose six shipping pruning settings and a
build-gated singular toggle through UCI, and demonstrate them through the existing match harness.

**This change will not:** change defaults or pruning algorithms, expose singular numeric parameters or
floating-point iteration controls over UCI, make LMP depth/index tunable, remove compile-time
feature gates, introduce reflection/dependencies, or refactor search policy and test fixtures
beyond the configuration changes they require.

## Decisions

### D1: One authoritative catalogue, typed values in search

Define all existing fields in `StratEngine/SearchTuning.def`, a declarative X-macro catalogue,
and generate the plain `SearchTuning` value type in `StratEngine/SearchTuning.h`.
`Config::PlayerConfig::search_tuning` becomes
`std::optional<SearchTuning>`; `AIPerplexConfig::tuning` keeps that same type. Delete
`SearchTuningConfig` and `map_tuning`; the factory uses the configured value or `SearchTuning{}`.

Each entry owns its member name, C++ type, default expression, units/description, validation policy,
build availability, JSON exposure/key and UCI exposure/name/encoding. Preserve current member
order, types and default expressions, including build-dependent defaults. Generate typed member
descriptors and equality from the same catalogue; protocol adapters consume these descriptors.
Defaults come from `SearchTuning{}`, never parser literals. Preserve all
15 existing JSON keys and omitted-value defaults; add the seven keys in D2. All keys use shared
validation, including the legacy keys, as specified in D5. Previously accepted invalid or
out-of-domain inputs now fail with a diagnostic; this is an intentional compatibility change.
Except for these additions, keep currently unbound fields unbound through explicit exposure
metadata; all fields still belong to the catalogue. Preserve existing handling of unknown JSON
keys. Model Boolean, integer and floating-point types now; UCI
encoding is required only for exposed fields. Availability metadata follows existing compile gates;
an unavailable field cannot be advertised or enabled through a newly exposed binding.

Keep JSON and UCI parsing/formatting in separate cold adapters, sharing typed validation and
structured errors from the tuning module. JSON dependencies stay out of the value header. Search
continues to read ordinary members directly, with no runtime map, variant or descriptor access.
Use narrowly scoped macros, undefined after expansion; no external generator or dependency.
Keep field rationale as ordinary comments immediately above its catalogue entry, never as long
macro arguments or a second documentation list. Generated declarations retain named members for
debugging. A plain struct plus a member-pointer table can use aggregate-initialization probes to
detect field-count drift in C++20. That makes the alternative viable, but count equality alone
does not verify member identity or bindings. Prefer the catalogue's single declaration point and
one-entry extension workflow; impossibility of a missing-entry check is not the rationale.

The extension contract is one catalogue entry plus independent tests for a new setting of an
existing supported type/encoding. No edits to factory mappings, UCI dispatch branches, JSON field
lists, equality or default literals are needed. New encodings or cross-field constraints extend
the tuning module explicitly. Prefer this compile-time catalogue over separately maintained
declarations and descriptors: the extra initial work prevents future binding/default drift.
Implement in two PRs with the shared contract specified in D6.

### D2: Six shipping options and one build-gated option

| UCI name | JSON key / member | Type | Default | Accepted values |
|---|---|---|---|---|
| `ReverseFutility` | `reverse_futility_enabled` | check | true | true, false |
| `ReverseFutilityMaxDepth` | `reverse_futility_max_depth` | spin | 3 | 1–`MAX_PLY` plies |
| `ReverseFutilityMargin` | `reverse_futility_margin` | spin | 100 | 0–1000 cp per ply |
| `FrontierFutility` | `frontier_futility_enabled` | check | true | true, false |
| `FrontierFutilityMargin` | `frontier_futility_margin` | spin | 200 | 0–1000 cp |
| `LateMovePruning` | `late_move_pruning_enabled` | check | true | true, false |
| `SingularExtensions` | `singular_extensions_enabled` | check | existing build default | true, false; compiled feature only |

Advertise and resolve UCI `SingularExtensions` only when `kSingularExtensionsCompiled` is true.
In shipping builds it follows the existing silent-ignore path for unadvertised options. JSON and
direct service configuration accept false and reject true when the feature is unavailable; the
false setting is a no-op when it is already false. Omission retains the default. Keep
`STRAT_SINGULAR_DEFAULT_ON` for this change: the experimental engine currently defaults on while
the test target defaults off. UCI access removes the need to rebuild to toggle it, but deleting
the macro would also change an existing build default or require replacing its policy elsewhere.
Update the macro's rationale to describe the retained default policy. Numeric singular parameters
remain unexposed.

These are proposed experiment bounds, not measured good settings. RFP's default depth band stays
three; users can sweep a wider band up to the engine's ply capacity. No evidence establishes eight
as a correctness or playing-strength boundary. At `MAX_PLY=256` and margin 1000, the depth product
is at most 256000. The shipping surface remains six controls; the experimental toggle demonstrates
availability handling through the same adapters. Any change to shipping defaults needs separate
strength evidence.

UCI advertises these options from the descriptors and uses their validators. Names are
case-sensitive, matching existing options; values are lowercase `true`/`false` or unsigned decimal
integers, with surrounding whitespace allowed. Reject missing values, overflow, trailing garbage
and out-of-range values without clamping. Invalid recognized tuning options produce an
`info string` diagnostic; unknown options and existing Hash/Threads behavior remain unchanged.

### D3: Apply only while idle; invalidate cached scores

Parse into a candidate copy, validate the whole candidate, and apply through one `AIPerplex`
tuning-update method. The tuning module provides the common validation/error contract for JSON
and service updates; adapters own protocol-specific diagnostics. UCI retains
`refuse_while_searching`; direct callers have the same externally serialized idle-only and
completion-handler restrictions as other configuration methods. This is not a concurrent setter.

A valid changed configuration clears the TT before the next search. Invalid requests and writes
of the current value leave both tuning and TT unchanged. Retain move-ordering history; this setter
does not start a new game. Controlled comparisons start with a fresh process or `ucinewgame`.
`StartNewGame()` continues to preserve tuning. Helpers use the service's immutable configuration
for the duration of each search; no per-node lookup, synchronization or dispatch is introduced.

### D4: Reuse the existing experiment path

`Run-EloMatch.ps1` already accepts `-CandidateOptions` and `-ReferenceOptions`; use, for example,
`option.ReverseFutility=false`. Names contain no spaces because the script splits option strings
on whitespace. Retain an explicit manifest beside the smoke-run artifacts containing binary
identity, both option lists, Threads/Hash, time control and opening selection. This first increment
requires no harness redesign; the manifest may be supplied with the experiment artifacts.

### D5: Shared validation for every tuning field

JSON requires actual Booleans for Boolean fields and representable integers for integer fields;
ratios accept JSON numbers but must be finite. Apply the same value-domain checks at service
construction and update, including fields currently unexposed. Invalid input rejects the candidate
without partial application. Diagnostics identify the field and violated constraint.

| Fields | Accepted domain |
|---|---|
| `min_nodes_threshold` | Full `int64_t` range; comparison only |
| `min_completion_ratio` | Any finite `double`; comparison only |
| `min_pv_ratio` | Finite, 0–1 |
| `score_draw_threshold` | Full `int` range; comparison only |
| `delta_pruning_margin` | 0–10000 cp; retained conservative arithmetic bound |
| `aspiration_initial_delta`, `aspiration_max_retries` | Positive delta, nonnegative retries; joint overflow constraint below |
| `lmr_min_depth`, `null_move_min_depth`, `singular_min_depth` | 1–`INT_MAX`; eligibility thresholds |
| `null_move_reduction` | 1–`INT_MAX`; subtracted from positive node depth minus one |
| `lmr_min_move_index` | Full `int` range; comparison only |
| `singular_tt_depth_margin` | 0–`INT_MAX`; subtracted from positive node depth |
| `singular_margin_factor` | 0–1000 cp per ply |
| RFP and frontier numeric settings | D2's ranges |
| All Boolean fields | false or true; singular true also requires compiled support |

Reject an aspiration pair unless `delta * 2^retries <= INT_MAX - GameValues::Search_Init`.
Check this by at most 31 checked doublings in the cold validator, rejecting before multiplication
would exceed the limit; do not shift or loop an unbounded user-supplied retry count. This replaces
the arbitrary independent caps of 10000 and 16. The PV ratio bound avoids an out-of-range
floating-to-integer conversion for positive int depths. Singular verification already clamps its
depth to at least one, so its eligibility threshold need not start at three.

Comparison-only values need no invented tuning ranges: negative thresholds can disable a test and
large thresholds can make it stricter. Margins used in sums/products retain conservative bounds;
nonnegativity alone does not prevent signed overflow. These bounds are not claimed to be maximal
safe domains or optimal playing values. In particular, `MAX_PLY` bounds recursion, not every
caller's requested remaining depth; do not justify the singular multiplier solely by that constant.
Relaxing the remaining conservative arithmetic bounds requires a separate derivation. Validate
domains even when a feature is disabled, so enabling it cannot activate invalid parameters.

### D6: Two PRs with one stable tuning-module contract

PR 1 delivers the catalogue, plain `SearchTuning`, shared validation, equality and JSON integration,
and removes the mirror/mapping. It includes the JSON bindings in D2 and the catalogue's UCI
name/encoding metadata, but no UCI code or runtime setter. Its cold API in namespace
`SearchTuningSchema`:

- `std::optional<TuningError> Validate(const SearchTuning&)`;
- `std::optional<TuningError> ParseJson(const nlohmann::json&, SearchTuning& in_out)`.

`TuningError` holds `Code code` (`UnknownSetting`, `InvalidType`, `OutOfRange`, `Unavailable`,
`InvalidCombination`) and owning strings `field` and `message`. Success is `std::nullopt`; parsers
validate a candidate and replace `in_out` only on success. JSON retains its unknown-key
compatibility rule. Keep JSON declarations in a separate adapter header from the value type.

PR 2 adds, together with their only caller in `UCIHandler`:

- `std::optional<TuningError> ParseUci(std::string_view name, std::string_view value, SearchTuning& in_out)`;
- `std::vector<std::string> UciOptionLines()` (complete `option name ...` lines, catalogue order).

UCI lookup uses only available/exposed entries, returning `UnknownSetting` for unavailable
singular. PR 2 also adds the idle-only service update/TT invalidation and performs the experiment
plumbing run. It relies on the value, equality and catalogue metadata from PR 1, not on macro
internals. Both PRs require Engine-tier gates, default search equivalence and nps
parity; PR 1 adds schema/JSON tests and PR 2 adds lifecycle/protocol and plumbing evidence.

## Assumptions I cannot verify from the code

- End-to-end fastchess delivery and persistence of the new options have not been exercised.
  Verify with a short plumbing match using distinct settings, retaining protocol evidence of the
  received options and the manifest. This is not evidence of playing strength.
- Moving declarations/bindings is intended to preserve shipping search speed, but code layout
  effects are unmeasured. Verify with the repository's shipping clang-cl benchmark workflow.

## Invariants

Defaults, JSON values within the documented domains, compile gates and default search behavior
remain unchanged. All entry points share validation. UCI and JSON bindings select the same members
and ranges. A failed/busy update cannot mutate
configuration or TT; a changed idle update invalidates TT scores; new-game reset preserves tuning.
Search sees a stable typed configuration throughout the main and helper threads. Every field has
one catalogue definition; extending an existing setting kind does not require adapter changes.
The configuration abstraction adds no per-node work or change to the tuning object's layout.

## Validation

This document is Docs tier; implementation is Engine tier and runs the required full gates.

- Keep independent assertions for existing JSON values and defaults; compare absent and empty
  tuning blocks. Test every bound and legacy/new binding with concrete values and malformed inputs,
  including atomic rejection, integer overflow and non-finite direct-service ratios. Retain the
  existing non-default round-trip cases. Do not derive expectations from descriptors under test.
- Test joint aspiration limits at and beyond the arithmetic boundary, including enormous retry
  counts; pin acceptance of comparison-only values beyond the removed caps. Exercise RFP depth
  values 4, 8 and `MAX_PLY`, plus rejection of zero and `MAX_PLY+1`; the default remains three.
- Add compile-time checks for unique exposed names and compatible type/encoding/default/range
  metadata. Exercise unavailable-feature rejection and mixed Boolean/integer/floating-point
  descriptors. The extension contract is demonstrated by PR 2 exposing seven options through
  catalogue metadata alone, with no per-option dispatch; no test-only catalogue is built.
  Verify the generated value preserves baseline member types, order, size and alignment.
- Test UCI advertisement, all seven option-to-member mappings, invalid/busy rejection, and settings
  persistence across `ucinewgame`. Seed a TT marker to prove changed updates clear it while invalid
  and same-value updates retain it. Check the next search consumes the chosen setting, including
  a multi-thread case. Retain existing Hash/Threads and search lifecycle coverage.
- Verify shipping advertises six controls and silently ignores UCI singular requests without state
  changes; JSON accepts false and rejects true. The compiled experimental engine advertises seven
  and honors both toggle values. Assert experimental-default-on
  and test-target-default-off separately. The test binary alone cannot prove shipping availability.
- At defaults, require identical best moves and node counts at `Threads=1` through
  `Compare-SearchEquivalence.ps1`; require nps parity with shipping clang-cl using the repository's
  paired benchmark workflow and uncertainty criteria. A reproducible slowdown blocks acceptance
  and must be removed, not traded for maintainability. Inconclusive measurements require more
  evidence rather than a parity claim. Follow `measure-strength` for instruments and uncertainty.
- Measure nps parity as a series of paired `Run-Bench.ps1` runs (baseline, candidate), discarding
  the first pair as warm-up. No Elo match.
- Complete D4's plumbing match with `Run-EloMatch.ps1 -Smoke` (20 games) and retain its manifest/protocol evidence. No additional
  statistical Elo match is needed for unchanged defaults; required Engine-tier self-play still
  runs. Promoting any non-default setting later requires its own strength measurement.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Catalogue schema, extension recipe, typed defaults and exposure (D1–D2) | `SearchTuning.def`, tuning-module comments and `Docs/EngineContracts.md` |
| Uniform validation and intentional rejection of old invalid values (D5) | Catalogue validators, `game_settings.json` guidance and `Docs/Changelog.md` |
| Cold tuning-module API and atomic parser results (D6) | Tuning adapter headers and contract tests |
| Binding/range correctness and performance parity | Independent config/UCI tests; benchmark evidence in PR and measurement records |
| Idle-only updates, TT invalidation and new-game persistence (D3) | `Docs/EngineContracts.md` configuration section and service API comments/tests |
| Reproducible option-based experiment (D4) | `Run-EloMatch.ps1` help/example and experiment manifest |
| Validation results and any approved design deviations | PR body; durable behavior changes in `Docs/Changelog.md` |

Author self-review: checked scope, defaults and existing paths against the baseline; the unmeasured
claims above remain explicitly subject to implementation validation. Claude's design
questions are answered in `unified-search-tuning.review.md`. Round 3 (owner-approved, edited by
Claude): UCI parsing moves to PR 2 with its caller; no test-only catalogue; nps by paired
`Run-Bench.ps1` series; plumbing by `-Smoke`. Approved for implementation by Claude.
