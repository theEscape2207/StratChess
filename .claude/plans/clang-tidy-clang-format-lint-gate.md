# clang-tidy and clang-format lint gate — Design

**Issue:** #175

## Goal

The repository has no formatting configuration and no static-analysis configuration. Formatting is
consequently inconsistent — measurably so, see D1 — and a class of defect that compiler warnings
cannot reach goes undetected, including at least one candidate out-of-bounds read that the existing
sanitizer job can only catch if the faulting path happens to execute.

Both tools already ship with the installed VS toolchain and `CMAKE_EXPORT_COMPILE_COMMANDS` is
already `ON`, so there is **no new engine or source dependency**. CI does acquire one: pinning the
LLVM major (D5) adds apt.llvm.org as an external package source, and with it an availability
dependency that can fail a run for reasons unrelated to the code. That is an acceptable trade for a
reproducible check set, but it is a real dependency and is named here rather than waved away.

What is missing is a decision about *which* rules apply, and evidence about what enforcing them
would cost. This change supplies both: configuration files, a CI job that runs them, and a measured
backlog converted into follow-up issues.

## Scope

**This change will:**

- Add `.clang-format` and reformat the 93 non-archived sources in one commit, recorded in a new
  `.git-blame-ignore-revs`, with `blame.ignoreRevsFile` set by `build.ps1`'s first-run setup (D9).
- Add `.clang-tidy` enabling `bugprone-*`, `performance-*` and `clang-analyzer-*`, less two checks
  (D3).
- Add a `lint-linux` job to `build-and-test.yml`: clang-format **blocking**, clang-tidy
  **advisory**.
- Add a whole-tree clang-tidy pass to `nightly.yml`, advisory (D6).
- Pin the LLVM version used by CI (D5).
- Add `StratChessEvolved/Scripts/Run-Lint.ps1` as the local entry point, and call its format check
  from `Validate-PrePR.ps1` (D8).
- Update `Docs/CI.md`, add one line to `CLAUDE.md`, and add `Get-ChangeTier.ps1` rules plus
  self-test cases for the new dotfiles and the new script.

**This change will not:**

- Fix any clang-tidy finding. The ~120 findings in the enabled set are left visible and unfixed;
  this is why clang-tidy is advisory rather than blocking.
- Enable `misc-include-cleaner`, `misc-const-correctness` or `misc-use-anonymous-namespace`. Each
  gets its own issue (D4).
- Touch `StratEngine/Archived/`, which is excluded from the build.
- Run clang-tidy over the whole tree on a pull request.
- Change any engine behaviour. The reformat is required to be semantically inert and is validated
  as such.

## Decisions

### D1: One bulk reformat, blame-ignored — not format-on-touch

The issue asserts "the tree is internally consistent today; a reformat that churns every file would
bury real diffs", and proposes codifying the existing style to avoid churn. **That premise is
false.** Measured over the 93 non-archived sources:

| Property | Measurement |
|---|---|
| Indentation | 43 files tab-indented, 45 space-indented, 5 neither; **10 files mixed internally** (`StratChessEvolved.cpp` 55 tab / 266 space; `Sort.cpp` 93 tab / 10 space) |
| Braces | 1049 Allman openers vs 419 attached (~29% attached) |
| Line length | p50 33, p90 78, p99 105, max 166; 1451 lines >80, 285 >100, 52 >120 |
| Nearest stock style | WebKit — and it still rewrites ~77% of sampled lines. LLVM, Google, Chromium, Mozilla, Microsoft and GNU are all worse |

There is no single existing style to codify. Any `.clang-format` disagrees with roughly half the
tree on the indent character alone, so the churn is unavoidable; the only real choice is whether it
lands once as an isolated, blame-ignored commit or leaks into every future engine PR.

Rejected: **format-on-touch** (no bulk commit) — spreads the same churn across unrelated diffs,
which is precisely the outcome the issue wanted to avoid, just harder to see. Rejected: **check
changed lines only via `git-clang-format`** — bounds the diff forever but leaves the tree
permanently non-uniform, and flags lines a PR merely shifted.

### D2: K&R-derived layout, tabs, width 4 — specified completely

A partial style specification is not a decision: clang-format fills every unstated option from its
base style, and several of those defaults are behaviour-bearing. The config is therefore given in
full for every option that changes this tree's output. `BasedOnStyle: LLVM` supplies the rest.

```yaml
BasedOnStyle:  LLVM
Language:      Cpp
Standard:      c++20
UseTab:        ForIndentation
TabWidth:      4
IndentWidth:   4
ColumnLimit:   100
IndentCaseLabels: false
NamespaceIndentation: None
PointerAlignment:   Left      # LLVM default is Right; the tree writes `const Move* p`, `MoveList& m`
ReferenceAlignment: Pointer
AllowShortFunctionsOnASingleLine: Empty   # LLVM default All would collapse PieceHelper.h's accessors
SortIncludes:  Never          # see below
IncludeBlocks: Preserve
IndentPPDirectives: AfterHash # LLVM default None flattens the nested _MSC_VER branches
BreakBeforeBraces: Custom
BraceWrapping:
  AfterFunction:         true
  AfterClass:            false
  AfterStruct:           false
  AfterUnion:            false
  AfterEnum:             false
  AfterNamespace:        false
  AfterCaseLabel:        false
  AfterControlStatement: Never
  BeforeElse:            false
  BeforeCatch:           false
  BeforeWhile:           false
  BeforeLambdaBody:      false
  IndentBraces:          false
  SplitEmptyFunction:    false
  SplitEmptyRecord:      false
  SplitEmptyNamespace:   false
```

**`SortIncludes: Never` is the load-bearing line.** Include reordering is not whitespace — it changes
translation-unit semantics, and this tree has a `StdAfx.h` umbrella header that must precede the
headers relying on it. A bulk reformat that also reordered includes could not be defended as
semantically inert, and would collide head-on with the include-order question deferred in D4. If
include ordering is ever adopted it is its own change, with its own validation.

**Brace wrapping is `Custom`, not `Stroustrup`.** `Stroustrup` is documented as "like `Attach`, but
break before function definitions, catch, and else" — it also breaks `else` and `catch`, which
earlier drafts of this document wrongly denied. The explicit `BraceWrapping` block above encodes the
intended NL.17 layout directly (`struct Cable {` attached, `double foo(int x)` broken, `if (0 < x) {`
attached, `} else {` attached) and removes the ambiguity rather than relying on a preset whose name
suggests one thing and whose definition says another. `Linux` was rejected for breaking before class
and namespace, which NL.17 does not.

Tabs over spaces: preserves the convention of the original engine core (`Sort.cpp`,
`ABIterative.cpp`, `MoveGenerator.cpp`), and the file counts are near-even so neither choice is
"what the tree already does". `ForIndentation` rather than `Always` so continuation alignment uses
spaces — otherwise the layout silently degrades under any tab width but 4.

`ColumnLimit: 100` over 120: only 285 lines exceed 100 and 52 exceed 120, so the extra wrapping is
affordable and 100 is the stated preference. If tuning shows 100 forces materially worse wrapping in
the wide PST and bitboard tables, fall back to 120 and record why.

`IndentPPDirectives: AfterHash` is a late addition, and only 17 lines in 4 files depend on it —
`StdAfx.h`, `Compat.h`, `SquareHelper.h`, `PieceHelper.h`. LLVM's default (`None`) flattens
`#  define STRAT_FORCEINLINE ...` to column zero, erasing the visual nesting of exactly the
`_MSC_VER`/`_WIN32` compat branches that are hardest to read. `AfterHash` preserves the nesting and
renders it as a tab, consistent with `UseTab: ForIndentation`. Cost is 3 lines of extra churn.

**Measured churn against this complete config** (clang-format 22.1.8, all 93 non-archived sources):

| | |
|---|---:|
| Files needing reformat | **90 / 93** |
| Lines added / removed | +9,746 / −9,503 |
| Share of the tree rewritten | **48.1%** |
| Files with a changed include sequence | **0** |
| Files touched under `Archived/` | **0** |

This supersedes the earlier "WebKit rewrites ~77% of sampled lines" figure, which established only
that no preset fits and was never an estimate of this configuration's diff. Roughly half the tree
moves — consistent with D1's finding that the indent character alone is split near 50/50, and the
reason the reformat is one blame-ignored commit rather than 90 unrelated diffs.

The include result is the one that matters for safety: **no file's sequence of included headers
changes.** 36 include *lines* do appear in the diff, but every one is trailing-comment realignment
(tab-aligned comments becoming space-aligned) or the `Compat.h` preprocessor indent above — never a
reordering. `SortIncludes: Never` does what D2 needs it to.

### D3: Enable bugprone + performance + clang-analyzer; exclude two checks

Measured twice on the full tree, 48 TUs: on Windows against a clang-cl database (LLVM 22.1.3) and
on Linux against the D7 clang database (clang 22.1.8, WSL Ubuntu 24.04 matching the CI image). **The
Linux run is authoritative** — it is the configuration CI will use. Totals agree closely:

| Metric | Windows clang-cl 22.1.3 | Linux clang 22.1.8 |
|---|---:|---:|
| Raw diagnostic lines | 18,902 | 18,856 |
| Unique findings, broad probe | 4,734 | 4,684 |
| **Enabled set (this decision)** | 113 | **97** |
| `clang-diagnostic-error` | — | **0** |

The probe was `bugprone-*`, `performance-*`, `modernize-*`, `misc-*`, `readability-*`,
`clang-analyzer-*`, `cert-*`, `concurrency-*`. Linux counts below; the total is dominated by checks
that are wrong for this codebase:

| Check | Findings | Disposition |
|---|---:|---|
| `modernize-use-trailing-return-type` | 745 | Off — pure style; would rewrite every signature |
| `misc-include-cleaner` | 683 | Deferred, see D4 |
| `readability-identifier-length` | 503 | Off — `sq`, `to`, `bb` are the right names here |
| `readability-magic-numbers` | 448 | Off — PSTs and bitboard constants are magic numbers by construction |
| `misc-use-anonymous-namespace` | 349 | Deferred, see D4 |
| `readability-braces-around-statements` | 344 | Off — clang-format's remit |
| `misc-const-correctness` | 330 | Deferred, see D4 |
| `bugprone-throwing-static-initialization` | 320 | Off — **every one is in `StratChessTests/`**, one per Catch2 `TEST_CASE` static registration. A pure artifact of the test framework |

What remains is **97 findings**, small enough to fix in a single follow-up PR:

| Check | Linux | Windows |
|---|---:|---:|
| `bugprone-narrowing-conversions` | 32 | 32 |
| `performance-enum-size` | 20 | 20 |
| `bugprone-unchecked-optional-access` | 14 | 14 |
| `clang-analyzer-*` | 5 | 5 |
| `performance-inefficient-vector-operation` | 5 | 5 |
| `performance-avoid-endl` | 5 | 5 |
| `bugprone-implicit-widening-of-multiplication-result` | 3 | 3 |
| `bugprone-empty-catch` | 3 | 3 |
| `bugprone-branch-clone` | 3 | 3 |
| `performance-move-const-arg` | 2 | 2 |
| **`bugprone-exception-escape`** | **2** | **15** |
| **`bugprone-reserved-identifier`** | **0** | **3** |
| `performance-unnecessary-value-param` | 1 | 1 |
| `bugprone-random-generator-seed` (`Board.cpp:36`) | 1 | 1 |
| `bugprone-inc-dec-in-conditions` | 1 | 1 |
| **Total** | **97** | **113** |

Every check agrees across platforms except two, which together account for the whole 16-finding gap:

- **`bugprone-exception-escape`, 15 Windows / 2 Linux (−13).** Windows reports `Logger.cpp`,
  `BitBoardHelper.h`, `IPlayer.h`, `AIPerplex.cpp`, `main` and two test destructors; Linux only two
  sites in `FENParser.cpp`. The MSVC standard library's different `noexcept` surface is the plausible
  cause.
- **`bugprone-reserved-identifier`, 3 Windows / 0 Linux (−3).** These are `_WIN32_WINNT` and
  neighbours in `StdAfx.h`, inside `_WIN32` guards that Linux never compiles. Unlike the above this
  one is *correctly* absent: the code does not exist in a Linux build.

This matters for the follow-up fix PR: **16 findings are invisible to the Linux gate, and 13 of them
are real on the shipping clang-cl build.** A fix PR validated only against CI would leave those
untouched and believe itself complete.

By location the enabled set is 73 in `StratEngine`, 20 in `StratChessTests`, 2 in
`StratChessEvolved`, and 2 in vendored Catch2 — see the `_deps` note below.

`bugprone-easily-swappable-parameters` (14) is also excluded: adjacent same-typed parameters are
inherent to a move-generation API (`from`, `to`; `rank`, `file`) and the check has no actionable fix.

**`--header-filter` alone does not exclude vendored headers.** Two `clang-analyzer` findings in
`_deps/catch2-src/extras/catch_amalgamated.hpp` survived a filter scoped to the three source
directories. The `.clang-tidy` therefore needs an explicit `ExcludeHeaderFilterRegex` for `_deps`;
without it the gate reports findings nobody can fix.

Rejected: **`clang-analyzer` only** (5 findings, could block on day one) — leaves the 92 bugprone and
performance findings undetected, which is most of the value. Rejected: **also enabling
`misc-const-correctness` and `misc-use-anonymous-namespace`** (~680 findings) — both are real
improvements, but they touch `Eval.cpp` and `AIPerplex.cpp` and so drag in the specialised-reviewer
gate, making this PR a different and much larger thing.

### D4: `misc-include-cleaner` is deferred to its own issue

The issue names this check as the headline justification: it "directly finds #167 item 5 (ten
headers using fixed-width integer types without including `<cstdint>`)".

**That justification is stale.** PR #275 ("MSVC-era residue cleanup", merged 2026-08-11) closed it;
a sweep of the current tree finds zero headers using a fixed-width integer type without including
`<cstdint>`. The check's remaining 662 findings are something else entirely: it demands every TU
directly include `<algorithm>`, `<array>`, `<utility>` and so on, which contradicts the documented
`StdAfx.h` umbrella-header convention in `CLAUDE.md`.

That contradiction is a real architectural question — include-what-you-use has genuine benefits for
incremental build time — but it is a 662-edit change across 93 files plus a convention reversal, and
it is not issue #175. It gets its own issue. `misc-const-correctness` (330) and
`misc-use-anonymous-namespace` (349) are deferred the same way and for the same reason: each is
mechanical but large enough to deserve its own review.

Rejected: **enable it advisory-only.** A check that emits 662 warnings on every run is noise people
learn to scroll past — the exact failure mode the issue warns about for whole-tree jobs.

### D5: clang-format blocking, clang-tidy advisory, pinned LLVM

The two tools are at different readiness. After the bulk reformat the tree is format-clean *by
construction*, so the format check can block from day one at zero cost. clang-tidy has ~120
outstanding findings, so blocking would make the gate red on arrival — and the issue is explicit:
"Do not enable checks and `-Werror`-equivalent failure in the same change."

CI installs a **pinned LLVM major version** rather than using ubuntu-24.04's apt default. Two reasons,
and the first is not optional: the check *inventory* differs between clang-tidy majors, so an
unpinned runner can silently gain or lose checks on an image update. Second, once clang-tidy becomes
blocking, a version difference between the developer's toolchain and CI produces "clean locally, red
in CI" with no visible cause. Pin to the major that ships with the VS toolchain developers actually
have, and state the version in `Docs/CI.md`.

Rejected: **use the distro clang-tidy.** Cheaper by one install step, and buys a moving check set.

**Wiring, so that "blocking" is actually true.** `build-and-test-result` is the sole required check
and currently `needs: [classify, build-linux, sanitize-linux, tsan-linux, build-and-test]`
(`build-and-test.yml:340`). Adding `lint-linux` to the workflow without adding it to that list would
leave a failed formatter outside the required check — the gate would be advertised as blocking and be
nothing of the kind. So `lint-linux` joins both the `needs:` list and the `case` ladder in the report
step, on the same skipped-is-success terms as every other leg.

Within the job the two tools get opposite step semantics: the clang-format step fails the job on any
mismatch, while the clang-tidy step is forced to success (its findings are printed and summarised,
never propagated). This is deliberately done at the *step* level rather than with a job-level
`continue-on-error`, which would also swallow a format failure.

**File selection, per tool:**

| Tool | Scope | Rationale |
|---|---|---|
| clang-format | every changed `.cpp` **and** `.h` | Formatting is per-file and needs no compile database, so headers cost nothing to include — and excluding them would leave the "tree is format-clean" invariant false |
| clang-tidy | changed `.cpp` only | A header is not a translation unit; headers are reached transitively via `HeaderFilterRegex`, and comprehensively by the nightly pass |

**Changed-file discovery fails closed.** The job needs `fetch-depth: 0` for the same reason
`classify` does — a default checkout cannot resolve `origin/main...HEAD` — and uses the same
`github.event.before` base on a push to main. If the diff command fails, the job **fails** rather
than linting an empty list; a formatter that silently checked zero files while reporting success is
the same green-because-broken failure as D7's.

### D6: PR job lints changed files; nightly lints the whole tree

A PR-time whole-tree run is the one shape capable of becoming the critical path. The current full
tier is 260 s, set by `build-linux (Release)`; the whole-tree probe took ~15-20 min locally. So the
PR job lints only the `.cpp` files the PR touches — typically 1-5, comfortably under 260 s as a
seventh parallel job, adding runner minutes and no PR feedback time. `tsan-linux` is the worked
precedent.

That leaves a real gap: **a header is not a translation unit**, so a PR touching only `.h` files
lints nothing. Rather than complicate the PR job with header-to-TU mapping — which `defines.h` or
`StdAfx.h` would blow up into a whole-tree run anyway — the whole-tree pass goes to `nightly.yml`,
where wall clock does not matter. Headers are covered within a day, and the nightly output doubles
as the backlog trend line.

The PR job configures a **Release, non-sanitized** database. The sanitizer and stdlib-debug
configurations change which macros are live and would produce findings that do not apply to a
shipping build. It is configured with **clang**, not GCC — see D7.

### D7: The lint database is configured with clang, not the default GCC

The lint job runs `cmake -DCMAKE_CXX_COMPILER=clang++-<pinned>` rather than reusing the plain Linux
Release configure.

This is not a preference. `strat_configure_target` branches on compiler id and emits
`-fconstexpr-ops-limit=100000000` for GNU against `-fconstexpr-steps=100000000` for Clang
(`CMakeLists.txt:198-201`). clang-tidy is Clang LibTooling: it consumes the database's argument list
through the clang driver, which rejects the GNU spelling outright —

```
$ clang -fconstexpr-ops-limit=100000000 -c -
clang: error: unknown argument: '-fconstexpr-ops-limit=100000000'
```

— and surfaces it as a `clang-diagnostic-error` that no `Checks:` line can suppress. Against a
GCC-configured database this fails on **every** translation unit. Combined with the advisory
`continue-on-error`, the job would report success having analysed nothing: a gate that is green
precisely because it is broken. That is the worst available failure mode and the reason this is a
decision rather than an implementation detail.

A clang-configured database is also the better match on the merits. The shipping compiler is
**clang-cl**, so a clang-based Linux database is closer to what actually ships than a GCC one, and it
guarantees the flags in the database are the flags the analyser can consume. `-include
<compat_header>` (`CMakeLists.txt:197`) is compiler-neutral and survives either way.

Rejected: **strip or translate GNU-only arguments** from the database post-configure. It works, but
it means maintaining a flag-translation table in CI that silently rots the next time
`strat_configure_target` gains a branch — the same class of trap that made two of the three clang-cl
flag spellings fail silently in #84.

**Consequence for D3's numbers:** the survey must be re-run against this exact database and the
pinned LLVM major before implementation, and D3 reconciled against it. See assumption 2.

### D8: A local entry point, wired into the pre-PR gate

`Run-Lint.ps1` wraps both tools: it resolves `compile_commands.json` from the build directory the way
`Get-BuildArtifact.ps1` resolves binaries, and takes the same changed-files-by-default / `-All`
shape the CI job uses. Without it, running either tool locally means hand-assembling a `clang-tidy
-p build/<preset>` invocation, which is exactly what this repository's `Scripts/` convention exists
to prevent.

Its **format** check is called from `Validate-PrePR.ps1` on Build and Engine tiers. A blocking CI
check with no local counterpart would be the only gate in this repository that can only be
discovered after pushing, which contradicts how `Validate-PrePR.ps1` is meant to mirror CI. The
clang-tidy half is deliberately **not** wired in: it is advisory in CI, so making it fail a local
gate would be stricter locally than remotely.

Consequence for classification: because `Validate-PrePR.ps1` invokes it, `Run-Lint.ps1` is **Build**
tier, not Tooling — the same no-self-exemption rule that puts `New-PullRequest.ps1` in Build. A bug
in it could suppress a check and then decline to validate the change that suppressed it.

**Tool resolution is an explicit contract, because neither tool is on `PATH`.** Verified on this
machine: `which clang-tidy clang-format` finds nothing. They live at
`<VS>/VC/Tools/Llvm/x64/bin/{clang-tidy,clang-format}.exe`, and `build.ps1` importing the VS
environment does **not** persist into the parent shell that later runs `Validate-PrePR.ps1`. A script
that just invokes `clang-format` would therefore fail on a clean shell — or worse, find some
unrelated copy. Resolution order:

1. explicit `-ClangFormat` / `-ClangTidy` parameters, for an override;
2. a matching command already on `PATH`;
3. `vswhere` discovery of the VS LLVM directory on Windows, the same mechanism `build.ps1` already
   uses — never a hard-coded VS path.

Failing all three, exit with a diagnostic naming what was looked for and where, rather than
skipping silently.

The script **prints and validates the resolved major version** against the pinned CI major, warning
on a mismatch. The whole pinning argument in D5 rests on local output matching CI output; a version
skew that is invisible locally converts that argument into a false one.

### D9: `.git-blame-ignore-revs` is also configured locally

GitHub's blame view honours a root `.git-blame-ignore-revs` automatically, but local `git blame` does
not: it needs `--ignore-revs-file` per invocation, or `blame.ignoreRevsFile` set once. Since the
reformat's whole justification is that blame stays useful, the promise has to hold in the terminal
too, not only in the web UI.

`build.ps1` already sets `core.hooksPath` to `.githooks` on first run precisely so a fresh clone or
worktree is configured without anyone remembering to do it. `blame.ignoreRevsFile` goes in the same
place, for the same reason, and `Docs/Workflow.md` records the one-time command for anyone who wants
it without running the build.

## Assumptions I cannot verify from the code

1. ~~**apt.llvm.org publishes LLVM 22 packages for ubuntu-24.04 (noble).**~~ **VERIFIED** by
   installing them on WSL Ubuntu 24.04.4 (the CI image): `clang-22`, `clang-tidy-22` and
   `clang-format-22` all install and run, at **22.1.8**. Note the patch-level drift from the VS 18
   toolchain's 22.1.3 — the majors match, which is what D5's pinning argument actually depends on,
   but `Docs/CI.md` should state both so the difference is not rediscovered as a surprise.

2. ~~**The Linux finding set differs from the measured Windows one.**~~ **VERIFIED and reconciled**:
   the survey was re-run under WSL against a clang-22-configured Release database. `0`
   `clang-diagnostic-error` diagnostics, 48 TUs analysed, so the database is sound (D7's failure
   mode is excluded). Totals: 4,684 unique findings broad / **97 in the enabled set**, against 4,734
   / 113 on Windows. D3 now carries the Linux numbers.

   **One residual difference is carried forward rather than closed**: 16 findings are Windows-only.
   Three (`bugprone-reserved-identifier`) are correctly absent on Linux — the guarded code is not
   compiled there. The other 13 (`bugprone-exception-escape`) are real on the shipping clang-cl build
   and invisible to the Linux gate. Recorded in D3; a caveat for the follow-up fix PR, not a blocker
   for this one, since clang-tidy is advisory here either way.

3. **The reformat is semantically inert.** True for formatting in the general case, but macro
   continuations and multi-line string literals are where it stops being true. *Verification*: the
   equivalence check in the Validation section. Not yet done.

4. **`PieceHelper::Value()` is reachable with `NO_PIECE`.** `clang-analyzer-security.ArrayBound`
   reports an out-of-bounds read on `g_iPieceValues[piece >> 1]` at `PieceHelper.h:92`. Now confirmed
   to reproduce on **both** platforms, so it is not a clang-cl artifact — but the analyzer only
   claims the path exists, and whether any caller actually passes `NO_PIECE` remains unestablished.
   *Verification*: inspect callers, and if reachable, add a test that trips it under ASan.
   Deliberately **not** done here — it is a possible engine defect, not lint configuration, and gets
   its own issue.

## Invariants

- The engine binary built after the reformat visits **identical node counts** and returns
  **identical best moves** at fixed depth with `Threads=1`, compared to the binary built before it.
- The full test suite passes unchanged, in both Debug and Release.
- **No file's sequence of included headers changes.** Asserted on the *sequence*, not on whether
  include lines appear in the diff — 36 do, all of them trailing-comment realignment. Include order
  is the one formatting change that carries semantics here. Measured: 0 files reordered.
- `build-and-test-result` stays the required check and keeps its name, and **`lint-linux` is in its
  `needs` list** — otherwise "blocking" is a claim the wiring does not support. The lint job must be
  able to turn it red on a format mismatch and must never turn it red on a clang-tidy finding.
- **The clang-tidy step analyses a non-zero number of TUs when the PR changes any `.cpp`.** A run
  reporting zero findings must be distinguishable from a run that analysed nothing (D7).
- A Docs- or Tooling-tier PR still skips the lint job, as it skips every other build job.
- `Get-ChangeTier.ps1 -SelfTest` passes, including the new cases.

## Validation

Build tier (`.github/**` and root dotfiles). `Validate-PrePR.ps1` scopes itself; no judgement call.

The reformat commit carries the real risk, and its evidence is the equivalence check `CLAUDE.md`
already defines for comparing two builds: **identical node counts and identical best moves at fixed
depth, `Threads=1`**, via `Run-Bench.ps1` against a pre-reformat binary. Two builds of semantically
identical source must visit an identical tree; if they do not, the reformat changed meaning. This is
an equivalence check, not a speed measurement — nps is irrelevant here.

Two cheap assertions accompany it, both aimed at the ways a formatting change stops being inert:
`git diff` on the reformat commit contains no moved `#include` line, and the diff touches no file
under `Archived/`.

The database itself needs a positive control, since D7's failure mode is silence: the clang-tidy step
prints the count of TUs analysed, and a run that analysed zero while the PR changed a `.cpp` is a
failure regardless of findings.

Plus the full suite under both configurations, and `Get-ChangeTier.ps1 -SelfTest`.

**No Elo match.** There is no semantic change, so there is nothing an Elo match could resolve that
the node-count equivalence check does not settle more cheaply and more precisely.

## Follow-up issues to file

All five filed.

| Issue | Size | Why separate |
|---|---:|---|
| #281 — include-what-you-use vs the `StdAfx.h` umbrella convention | 683 findings | Architectural decision plus a `CLAUDE.md` convention reversal |
| #282 — enable `misc-const-correctness` | 330 | Mechanical but touches `Eval.cpp`/`AIPerplex.cpp` — reviewer gate |
| #283 — enable `misc-use-anonymous-namespace` | 349 | Same |
| #284 — clear the enabled-set backlog, then make clang-tidy blocking | 97 | The issue's own staging: configure first, enforce second |
| #285 — possible OOB read in `PieceHelper::Value()` | 1 | An engine defect, not lint configuration |

#284 carries the platform-gap warning from D3: a fix PR validated only against CI will believe itself
complete with 13 real `bugprone-exception-escape` findings outstanding on the shipping build.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| The tree was ~50/50 tabs/spaces before the reformat; why the bulk commit was unavoidable (D1) | `Docs/Changelog.md` |
| Reformat commit SHA, so `git blame` skips it | `.git-blame-ignore-revs` |
| Why `bugprone-throwing-static-initialization` is off (Catch2 registration artifact) | comment in `.clang-tidy` |
| Why `misc-include-cleaner` is off, and that #167 item 5 was already closed by #275 | comment in `.clang-tidy`, and the deferral issue |
| Why clang-format blocks and clang-tidy does not; why LLVM is pinned (D5) | comment in `build-and-test.yml`, and `Docs/CI.md` |
| Why the PR job lints changed files and the nightly lints the tree (D6) | comment in `build-and-test.yml` |
| Why `SortIncludes: Never`, and that include order is deliberately out of scope (D2) | comment in `.clang-format` |
| Why the lint database is clang-configured, with the `-fconstexpr-ops-limit` incompatibility that forces it (D7) | comment in `build-and-test.yml`, and `Docs/CI.md` |
| Why local `git blame` needs `blame.ignoreRevsFile` and where it is set (D9) | comment in `build.ps1`, and `Docs/Workflow.md` |
| Tool-resolution order for `Run-Lint.ps1` (D8) | its comment-based help |
| Pinned LLVM version, and any delta from the local VS toolchain | `Docs/CI.md` |
| Measured backlog by check | the follow-up issues, and the PR body |
| How to run the lint locally | `CLAUDE.md` Scripts table, one line for `Run-Lint.ps1` |
| Why `Run-Lint.ps1` is Build tier rather than Tooling (D8) | comment beside its rule in `Get-ChangeTier.ps1` |

**Approved decisions that changed during implementation:** none yet — fill in before opening the PR.
