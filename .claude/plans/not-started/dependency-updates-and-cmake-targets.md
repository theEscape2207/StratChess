# Dependency Updates and CMake Imported Targets — Design

**Issues:** #366 (spdlog 1.17.0), #367 (Catch2 3.15.3), #166 (consume dependencies as CMake packages)

## Goal

`CMakeLists.txt` pins spdlog at `v1.16.0` and Catch2 at `v3.13.0`, both a few releases behind, and
consumes all three dependencies as bare include directories rather than as CMake targets. Two
tracker issues ask for the version bumps and a third asks for the target migration; all three
rewrite the same twenty lines of the Dependencies block, so they share one rationale even though
they land as two separate PRs.

The include-only consumption is not a bug — it is correct and documented — but it makes the build
depend on each dependency's *internal directory layout* (`extras/`, `include/`) and re-state per
consuming target what an imported target would carry for free. Catch2 in particular is consumed
through its amalgamated distribution: one large translation unit rebuilt from scratch on every clean
build, and a single monolithic header included by all 34 test files.

## Scope

**PR 1 (#366, #367) will:**

- Bump `spdlog` to `v1.17.0` and `catch2` to `v3.15.3` in `CMakeLists.txt`.
- Update the twelve `cmake-deps-…` cache keys that spell the versions literally
  (`build-and-test.yml` ×4, `nightly.yml` ×7, `strength.yml` ×1).

**PR 2 (#166) will:**

- Drop `SOURCE_SUBDIR do-not-configure` from all three `FetchContent_Declare` calls and consume
  `spdlog::spdlog_header_only`, `nlohmann_json::nlohmann_json` and `Catch2::Catch2WithMain`.
- Force the dependencies' own example/test/benchmark options OFF before `MakeAvailable` so no extra
  targets are configured.
- Rewrite `#include <catch_amalgamated.hpp>` in 34 files (33 `.cpp` plus `UCITestFixture.h`) to the
  modular Catch2 headers, and drop `catch_amalgamated.cpp` from `target_sources`.
- Propagate `STRAT_STDLIB_DEBUG` and `STRAT_SANITIZE` onto the fetched Catch2 targets — see D4.

**Neither will:**

- Bump `nlohmann_json`. `v3.12.0` is the current upstream release; there is nothing to do.
- Add `FIND_PACKAGE_ARGS` (D5).
- Change the cache-key scheme (D3).
- Change any engine behaviour. Both PRs are expected to produce byte-identical search output; that
  expectation is the gate, not a hope (see Validation).

The will-not list is what stops #166 from turning into a build-system rewrite. It stays challengeable
where an exclusion turns out to be required for correctness.

## Decisions

### D1: Bumps first, migration second — two PRs, in that order

The two changes fail in different ways, and separating them keeps the diagnosis unambiguous. If
`sanitize-linux` goes red after a combined PR, the candidates are "the new Catch2 broke something"
and "configuring Catch2 as a subproject broke the ABI", and those are investigated very differently.

Bumps first rather than migration first because the bumps are almost free on the current scheme:
Catch2 `v3.15.3` still ships `extras/catch_amalgamated.{hpp,cpp}` (verified against the upstream tag),
so every existing `#include` line stays valid and PR 1 is a five-line source change. Rejected:
migrating first so the 34-file include rewrite happens against a version already known good. That
argument is real but weaker — the rewrite is mechanical and version-independent, whereas the bump's
one genuine risk (D6) is not.

### D2: Issue #367's reporter and discovery tasks do not apply here

#367 asks to validate the JSON reporter's `inf`/`NaN` handling and the JUnit output, and lists the
4–5× `catch_discover_tests` speedup as motivation. Neither reaches this repository: no `--reporter`
appears in any script under `StratChessEvolved/Scripts/` or in any workflow, and `CMakeLists.txt`
does not use `catch_discover_tests` — `StratChessTests` is invoked directly with a tag filter.

Recorded here so those checkboxes are closed as non-applicable rather than ticked without evidence.
The real payload of #367 is the accumulated compile fixes and the C++26 `std::optional` overload fix.

### D3: Leave the twelve literal cache keys alone

The version strings are hardcoded into twelve `actions/cache` keys. Deriving the key from
`hashFiles('CMakeLists.txt')` would make it one edit forever and impossible to forget, at the cost of
invalidating the dependency cache on every unrelated `CMakeLists.txt` edit across twelve jobs.

Left alone because a stale key is cheap and not incorrect: `FetchContent` re-clones when the on-disk
tag does not match `GIT_TAG`, so a missed key costs a clone, never a wrong build. Twelve mechanical
edits inside a version-bump PR are within its scope; a CI caching refactor is not. If this recurs,
`hashFiles` is the follow-up.

### D4: Push our compile flags onto the fetched Catch2 targets

This is the substantive part of #166 and the reason the issue is not the mechanical change it reads
as. Today all three dependencies are populate-only sources compiled *into our own targets with our
own flags*, so the standard-library ABI is uniform by construction. `CMakeLists.txt` carries an
explicit comment warning against changing that.

`spdlog::spdlog_header_only` and `nlohmann_json::nlohmann_json` are `INTERFACE` targets — header-only,
no ABI surface, and their headers still compile inside our translation units under our flags. They
are safe.

`Catch2::Catch2WithMain` is different: it is a compiled static library that passes `std::string` and
`std::vector` across the library boundary. Configured as an ordinary subproject it would be built
*without* `_GLIBCXX_DEBUG` and *without* the sanitizer flags, then linked against test translation
units that have both. The required `sanitize-linux` job builds with
`-DSTRAT_SANITIZE=address,undefined -DSTRAT_STDLIB_DEBUG=ON`, so this is not a hypothetical
configuration — it is a required check on every PR.

So PR 2 applies `_GLIBCXX_DEBUG`/`_GLIBCXX_DEBUG_PEDANTIC` and the `-fsanitize=` compile and link
options to the fetched Catch2 targets after `MakeAvailable`, under the same conditions
`strat_configure_target` uses. `PRIVATE` is the correct visibility: Catch2's own TUs need the define,
and our TUs already get it from `strat_configure_target`.

Rejected: dropping `STRAT_STDLIB_DEBUG` from the sanitize job to sidestep the problem. That trades a
correctness gate — checked iterators and container preconditions, which `_GLIBCXX_ASSERTIONS` does
not cover — for build hygiene. Wrong direction.

The existing tripwire comment is **rewritten to describe the new mechanism, not deleted**. The hazard
it names does not go away; it becomes something the build handles explicitly.

### D5: No `FIND_PACKAGE_ARGS`

Scope item 4 of #166 proposes `FIND_PACKAGE_ARGS` so a preinstalled or vcpkg-provided copy can
satisfy the dependency and CI can skip the clone. Declined on two grounds: a system copy at a
different version would silently satisfy the pin — and Catch2 in particular is pinned exactly because
behaviour differs across versions — and `build/_deps` is already cached in every job that builds, so
there is no clone cost left to save.

Recorded as a decision rather than an omission so #166 can close without this hanging open.

### D6: The `.vcxproj` constraint in #166 is stale

#166 declares the `.vcxproj` files out of scope and requires the amalgamated Catch2 files to remain
available for them. Those files no longer exist — `git ls-files '*.vcxproj' '*.sln'` returns nothing;
CMake is the only build system. The constraint is void and PR 2 can remove the amalgamated
consumption outright.

## Assumptions I cannot verify from the code

- **spdlog 1.17.0's bundled `{fmt}` 12.1.0 keeps the `formatter::parse`/`format` signatures that
  `StratEngine/Utils/Formatters.h` uses.** That header includes `spdlog/fmt/bundled/base.h` directly
  and specializes `fmt::formatter<eColor>` and `fmt::formatter<eSquare>` against the bundled fmt,
  which crosses a major version in this bump. Not verified. Settled only by compiling — and it would
  fail loudly at compile time on every toolchain, which is the good kind of failure. This is the one
  real risk in PR 1.

- **Catch2 3.15.x keeps `Catch::EventListenerBase`, `Catch::TestRunInfo`, `Catch::TestCaseInfo` and
  `Catch::TestCaseStats` source-compatible.** `StratChessTests/SuitePolicyTests.cpp` registers a
  listener against all four. Upstream treats 3.14/3.15 as maintenance releases and these are
  documented extension points, but that is taken from the release notes, not checked. Settled by
  compiling.

- **Setting the dependencies' option variables before `MakeAvailable` is sufficient to keep their
  extra targets out of the build.** Standard practice and documented by all three projects, but not
  verified against these specific tags. Settled by inspecting the configured target list after
  PR 2's first successful configure.

## Invariants

- Search output is unchanged. Every per-iteration `info` line and the `bestmove` at fixed depth,
  `Threads=1`, must match `origin/main` exactly — for both PRs.
- The fast-tier test count is unchanged and every test still runs. A Catch2 include rewrite that
  silently drops a file's registration would present as a smaller suite passing.
- Every translation unit in a link agrees on the standard-library container ABI. This is what D4
  exists to preserve, and it holds for the sanitize and stdlib-debug configurations specifically.
- The dependency set stays pinned and reproducible: no configuration may satisfy a dependency from
  outside `build/_deps`.

## Validation

**PR 1 — Tooling tier.** It touches no engine source, but it changes what the engine compiles
against, so the Tooling tier is a floor rather than the whole answer:

- `Compare-SearchEquivalence.ps1 -After <worktree exe> -BaselineRef origin/main` — the gate. Identical
  per-iteration `info` lines and `bestmove` at fixed depth, `Threads=1`.
- `Run-Bench.ps1` before/after on clang-cl Release. A bundled-fmt major bump touches the logging path,
  and nps is the only thing that would show it. Compare nps, not node counts.
- A Linux/GCC build under WSL, since fmt 12's compatibility story may differ by toolchain — see the
  build-timing section below, which produces this build anyway.

**PR 2 — Engine tier.** It recompiles every translation unit.

- `Compare-SearchEquivalence.ps1 -BaselineRef origin/main` — same gate, same reasoning. The migration
  changes how headers arrive, not what they say.
- A local WSL build with `-DSTRAT_SANITIZE=address,undefined -DSTRAT_STDLIB_DEBUG=ON` **before**
  pushing. This is the configuration D4 exists for, and finding an ABI mismatch in CI rather than
  locally wastes a full required-check cycle. The failure mode is a crash or corruption inside
  Catch2's own string handling, not a compile error.
- Test count compared against `origin/main`, not just "tests pass".

**No Elo match for either PR.** No search or evaluation term moves. Search equivalence is a stronger
claim than any affordable SPRT could make: an SPRT bounds a difference, equivalence rules one out.

### Build and test timing, local and WSL

Measured across both PRs, on the expectation that the result is **neutral**. PR 2 removes the
amalgamated single-TU compile and lets each test TU include only what it uses, which should help
clean-build time; against that, configuring three dependency subprojects adds configure time. The
expectation is that these roughly cancel. Measure rather than assume — and record the number even
when it is a wash, because "we checked and it was neutral" is a durable result and "we assumed"
is not.

Four data points per platform — `origin/main`, after PR 1, after PR 2 — capturing:

- **Clean configure + build wall time**, `StratChessTests` target, Release, warm dependency cache
  (`build/_deps` already populated, so the clone is not being timed).
- **Test suite wall time**, fast tier (`~[slow]`).

Platforms: Windows clang-cl via `build.ps1`, and Linux/GCC under WSL. The WSL build must run on
native ext4 — building over `/mnt/c` fails in `FetchContent`, and the working recipe is
`git archive` from the worktree, extracted into `~/`. Each extracted tree plus build dirs is ~800 MB;
clean up afterwards.

Build throughput is explicitly not this project's bottleneck (#81), so any number here is
**informational** — it does not gate either PR. It is recorded to close the question, not to justify
the change.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Why Catch2's fetched targets carry `_GLIBCXX_DEBUG` and `-fsanitize=` (D4) | rewritten tripwire comment in `CMakeLists.txt`, replacing the one that forbids the change |
| Why there is no `FIND_PACKAGE_ARGS` (D5) | source comment in the Dependencies block; closing note on #166 |
| #367's reporter/discovery tasks are non-applicable (D2) | closing note on #367 |
| Measured before/after build and test timings, both platforms | PR bodies; `Docs/Changelog.md` if non-neutral |
| Version bumps | `Docs/Changelog.md` |

The twelve-literal-cache-key observation (D3) is durable only if it recurs; if PR 1's twelve edits
feel like friction in practice, it becomes a `hashFiles` follow-up issue rather than a note here.

Nothing else in this document needs to outlive the two PRs. Once both merge and the table above is
discharged, delete it.
