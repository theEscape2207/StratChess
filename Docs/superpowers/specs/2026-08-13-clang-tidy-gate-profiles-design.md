# Clang-Tidy Gate Profiles Design

**Status:** Approved in conversation on 2026-08-13

**Issues:** #175, #284

**Starting point:** merged PR #293 at `75d6835`, including blocker fix `aab72dc`

## Goal

Finish #284 in two reviewable pull requests: first remove the source and configuration noise left by
the backlog-clearing changes, then make a fast clang-tidy profile block PrePR/PR validation while
moving expensive, lower-yield analysis to failing-but-non-required Nightly jobs.

The design optimizes developer feedback time without discarding the useful findings seen in #285,
#289, and #292. `clang-analyzer-*` and `bugprone-exception-escape` remain automated, but they do not
sit in the merge-critical path.

## Standing Constraints

- LLVM major 22 remains pinned locally and in CI.
- Required PR lint uses a plain Release Clang compile database, not GCC or a sanitizer database.
- Windows/clang-cl analysis remains necessary for shipping-only code and MSVC standard-library
  behavior.
- A lint command must fail closed when its file list or compilation database is unusable.
- Clang-format remains blocking for changed `.cpp` and `.h` files.
- Header coverage remains whole-tree Nightly rather than expanding a common-header PR into a
  whole-tree required run.
- Source comments explain current invariants, not tool history or earlier implementations.
- Existing user changes in the main checkout are out of scope and must remain untouched.

## Delivery Boundary

### PR 1: source and configuration hygiene

PR 1 changes source structure, comments, and central clang-tidy configuration only. It does not
change CI failure semantics. This creates a clean, measurable baseline for PR 2.

It will:

1. Refactor `FENParser::ParseFEN()` into a small public `noexcept` boundary and a private
   non-`noexcept` implementation. The implementation contains the existing parser at its natural
   indentation; the boundary catches unexpected exceptions and returns the existing
   `"internal error parsing FEN"` result.
2. Rename `safe_warn()` to `log_warning_noexcept()`. The name records both the operation and its
   exception guarantee. Only logging is caught; metadata correction must continue after a logging
   failure.
3. Retain the throwing-sink regression test added by `aab72dc`, but shorten its comments to the
   present behavior being protected.
4. Correct the `PieceHelper::AsPiece()` range comment: valid type/color inputs produce named piece
   values 0 through 13; 15 remains the `NO_PIECE` enumerator.
5. Correct the transposition-table narrowing proof to use the actual mate-score bound (`Mate`) plus
   `MAX_PLY`, not the recognition threshold (`Mate_Threshold`).
6. Reduce `main()`'s exception-boundary comment to the current contract and remove old crash-code
   and lint-history discussion.
7. Replace the nine source-level `performance-enum-size` suppressions with
   `performance-enum-size.EnumIgnoreList` in the root `.clang-tidy` file. The ignore list names only
   the accepted enum types; source headers retain domain explanations only where the type itself
   needs one.
8. Remove inaccurate history comments around `eRowNames` and `eSquare`. `eRowNames` may state the
   present `NO_ROW == -1` constraint if needed; `eSquare` performance investigation remains in #292.

PR 1 validation records:

- Release and Debug build/test results;
- clang-format over changed sources;
- whole-tree clang-tidy on Linux and Windows with the existing broad profile;
- identical fixed-depth, `Threads=1` best moves and node counts before and after the cleanup.

### PR 2: enforcement and CI architecture

PR 2 changes lint execution, CI workflows, and documentation. It starts only after PR 1 is merged so
the required profile is enabled against a known-clean main branch.

It will:

1. Make the fast profile blocking in local PrePR validation and required PR CI.
2. Move `clang-analyzer-*` and `bugprone-exception-escape` into a separate deep profile.
3. Run deep analysis on Nightly Linux and Windows/clang-cl jobs, failing those Nightly jobs on new
   findings without making them required PR checks.
4. Normalize the compilation database so each source is analyzed once with the intended target's
   command.
5. Update `Docs/CI.md`, script help, workflow comments, and #284 evidence with exact scope, failure
   semantics, invocation counts, and timings.

## Profile Architecture

### Fast required profile

The root `.clang-tidy` is the fast, discoverable default. It enables the cleared high-signal
`bugprone-*` and `performance-*` families while excluding:

- `bugprone-throwing-static-initialization` (Catch2 registration artifact);
- `bugprone-easily-swappable-parameters` (low-value API noise);
- `bugprone-exception-escape` (expensive and prone to invasive catch-all changes);
- all `clang-analyzer-*` checks (deep-profile responsibility).

The fast profile applies to Engine and application sources. Test sources inherit the root profile
but their `StratChessTests/.clang-tidy` override disables:

- `bugprone-unchecked-optional-access`, because Catch2 `REQUIRE(opt.has_value())` is not recognized
  as a guard;
- `performance-*`, because test-harness allocation/copy advice has no shipping value.

All remaining fast-profile findings are errors. The local command and required CI command use the
same runner and configuration rather than maintaining separate check lists.

`performance-enum-size` stays enabled for Engine/application code. Accepted current exceptions live
in its central `EnumIgnoreList`, so a new enum still receives review without adding lint-policy
comments to source.

### Deep Nightly profile

`.clang-tidy-deep` contains only:

- `clang-analyzer-*`;
- `bugprone-exception-escape`.

The deep profile targets shipping Engine and application translation units, not Catch2 test
translation units. #289 demonstrated that analyzer output can expose useful latent/unused defects,
but the yield does not justify adding roughly five minutes to every PR. `bugprone-exception-escape`
is grouped here because Clang documents it as expensive on large sources and PR #293 demonstrated
that satisfying it mechanically can increase correctness risk and churn.

Nightly runs the deep profile on both:

- Linux Release/Clang, for portable Engine/application paths;
- Windows Release/clang-cl, for `_WIN32` code and the shipping standard-library exception surface.

Each deep job fails on findings. It remains outside the repository's required PR status check, so a
failure is visible and actionable without delaying developer feedback.

## Compilation Database Normalization

`New-TidyCompileDatabase.ps1` is the single normalization implementation used by local and CI lint
execution. It reads an input `compile_commands.json`, writes a separate normalized database, and
never mutates CMake's generated file.

Selection rules are deterministic:

1. Canonicalize each `file` path and group entries by source.
2. A unique entry is retained unchanged.
3. For a duplicated `StratEngine/*.cpp`, select the one whose output/command belongs to the
   `StratChessEvolved` shipping target, not the `StratChessTests` target with
   `STRAT_ENABLE_TEST_ACCESS`.
4. Application and test translation units retain their sole target command.
5. Any duplicate group without exactly one valid shipping-target candidate is an error. The helper
   reports the source and candidates and exits nonzero rather than guessing.
6. Report input entry count, unique-source count, selected-entry count, and duplicate count as a
   positive control.

The normalized database lives below the build directory, for example
`build/tidy-gate/compile_commands.json`. Paths and working directories remain those emitted by CMake,
so copying the JSON does not change include resolution.

The observed Windows database has 74 entries for 50 unique files; 24 Engine sources are represented
twice. The expected normalized result is therefore 50 commands. CI measurements must record the
actual Linux counts rather than assuming they are identical.

## Shared Lint Runner

`Run-Lint.ps1` remains the user-facing entry point and becomes the common orchestration path for
local validation and workflow jobs.

It gains an explicit profile selection:

- `-Profile Gate` selects the root `.clang-tidy`, normalizes the database, and fails on findings;
- `-Profile Deep` selects `.clang-tidy-deep`, normalizes the database, limits scope to shipping
  Engine/application translation units, and fails on findings.

It also gains `-Jobs <positive integer>` and runs one clang-tidy process per selected translation
unit through a bounded worker pool. Gate defaults to four workers; Deep defaults to two because
analyzer processes consume more memory. An explicit value overrides the profile default, and the
effective worker count never exceeds the selected translation-unit count. CI passes `-Jobs 4` for
Gate and `-Jobs 2` for Deep so its concurrency is reproducible rather than host-dependent.

Normalization completes before work is scheduled, so parallelism applies only to unique selected
translation units. Each worker captures its source's stdout, stderr, and exit code independently;
the runner prints results in deterministic source order after all workers finish. Any failed worker,
tidy finding, or missing result fails the aggregate command. This bounded pool is preferred over a
single multi-source invocation, which processes sources sequentially, and over unbounded per-source
fan-out, which can exhaust memory during deep analysis.

`-Check Format` remains independent of compile-database setup. `-Check Tidy` defaults to `Gate` once
PR 2 flips enforcement. `Validate-PrePR.ps1` invokes the gate tidy check as well as format and
propagates either failure.

For CI portability, the runner accepts explicit Clang tool paths and an explicit build/database
directory. Windows retains override → `PATH` → `vswhere` resolution. Linux workflows pass the
pinned `clang-format-22` and `clang-tidy-22` executables explicitly.

The runner prints:

- resolved tool/version;
- profile and configuration path;
- source count and normalized command count;
- requested/effective worker count and completed invocation count;
- elapsed tidy time;
- findings grouped by check.

Zero selected translation units is valid only when the requested changed-file scope contains no
`.cpp` files. A missing/malformed database, failed diff, diagnostic parse error, normalization
ambiguity, or tidy finding exits nonzero.

## CI Topology

### Required PR CI

`lint-linux` keeps changed-file format and tidy analysis in the same parallel job:

- clang-format checks changed `.cpp` and `.h` files;
- gate clang-tidy checks changed `.cpp` files using the normalized shipping database;
- gate clang-tidy uses four workers, capped by the changed translation-unit count;
- both steps are blocking;
- `build-and-test-result` continues to aggregate `lint-linux`, updating its success/error text to
  describe both checks as required.

No analyzer or exception-escape check runs here.

### Nightly

Nightly exposes three lint results independently:

1. `lint-tree`: whole-tree format plus fast gate profile on Linux with four tidy workers;
2. `lint-deep-linux`: deep profile on shipping Linux translation units with two tidy workers;
3. `lint-deep-windows`: deep profile on shipping Windows/clang-cl translation units with two tidy
   workers.

`nightly-result` depends on all three and turns red if any fails. Separate jobs preserve diagnosis
and prevent one platform/profile failure from suppressing another run.

## Testing Strategy

### Source cleanup tests

- Existing FEN parsing and metadata-validation tests must remain green.
- The throwing-sink test proves every correction completes when warning logging throws.
- No new behavior is introduced by the `ParseFENImpl()` extraction; public return values and output
  mutation remain unchanged.
- Fixed-depth engine equivalence protects the mechanical type/comment/refactor cleanup.

### Normalizer tests

Script-level tests build temporary JSON fixtures for:

- one unique command retained byte-for-byte at the object-field level;
- duplicated Engine commands selecting `StratChessEvolved` over `StratChessTests`;
- duplicate candidates with no shipping target failing closed;
- multiple shipping candidates failing closed;
- malformed JSON and missing input failing clearly;
- output counts matching the selected unique source count.

Tests use temporary directories and never overwrite a real build database.

### Workflow and profile verification

- Parse both workflow YAML files and PowerShell scripts with existing repository validation.
- Run the fast whole-tree profile on Linux and Windows with zero findings.
- Run the deep shipping profile on Linux and Windows with zero findings.
- Verify a synthetic known finding makes each profile command exit nonzero.
- Verify `-Jobs 1`, profile defaults, and explicit overrides analyze every selected normalized
  translation unit exactly once and never exceed their requested concurrency.
- Verify one failed worker fails the aggregate command and parallel diagnostics are emitted in
  deterministic source order.
- Compare raw and normalized invocation counts.
- Record elapsed timings for changed-file gate, whole-tree fast, and whole-tree deep runs, including
  a `-Jobs 1` baseline for each whole-tree profile.
- Run full Release and Debug tests after each PR's changes.

## Documentation and Issue Closure

`Docs/CI.md` will describe the final profile matrix, Engine/Test differences, compile-database
normalization, failure semantics, and measured cost. Script help and workflow comments must use the
same terms: **gate profile** and **deep profile**.

#284 closes only after:

- Linux and Windows gate profiles report zero findings;
- Linux and Windows deep profiles report zero findings;
- the required PR lint step fails on a gate finding;
- Nightly fails on a deep finding;
- equivalence evidence and final timing/invocation counts are posted;
- the two pull requests are merged.

## Out of Scope

- Enabling `modernize-*`, `misc-include-cleaner`, or a broad all-check profile.
- Header-to-translation-unit mapping in required PR CI.
- Performance changes proposed by #292.
- Making deep Nightly jobs required PR checks.
- Reworking the FEN API to `std::expected`; that remains a separate C++23/API change.
