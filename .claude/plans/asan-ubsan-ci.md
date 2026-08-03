# Add ASan/UBSan to CI (issue #179)

## Context

The engine has **never been run under a sanitizer**. It is dense with raw fixed-size arrays indexed
by computed values — the magic sliding-attack tables (64 x 4096 in `StratEngine/Magic.h`), PSTs,
the killer/history tables in `ThreadData.h`, the mailbox in `Board`, and the TT — and a Release build
passes out-of-bounds reads on all of them **silently**. The failure mode is not a crash; it is a
wrong evaluation that gets credited to whatever change is under test. The same applies to the
bitboard code's shifts, where a shift by >= 64 or a signed overflow is UB that works until a compiler
version changes its mind.

The prerequisite (#81 Phase 3 — CMake is the only build) landed in PR #177, so sanitizers are now a
compile-and-link flag pair rather than an MSBuild wiring exercise.

Intended outcome: an `ubuntu-24.04` CI leg that builds the test binary under
`-fsanitize=address,undefined` and runs the fast test tier, **already green when it lands** — the
backlog it would otherwise dump on the next unrelated PR is surveyed and fixed in this branch first.

## Scope decisions

| Decision | Choice | Why |
|---|---|---|
| Sequencing | Survey locally in WSL, fix findings, land the job enforcing | Avoids a job that is red from day one, and avoids a second PR |
| TSan | Out of scope; file a follow-up issue | The shared TT is racy-by-tolerance, so TSan is a triage-and-suppressions investigation, not a flag |
| Configuration | Debug only | `assert()` and the `#ifndef NDEBUG` tripwires stay live and are complementary to ASan; the fast tier is seconds even at -O0 + 3x |
| Trigger | Same condition as `build-linux` (`is_full`) | A `CMakeLists.txt` change is Build tier and must exercise the sanitizer config; one shared condition cannot drift |
| MSVC / clang-cl | Hard configure error if requested | Windows is not the home for this, and #84's lesson is that a silently-dropped flag is worse than a loud failure |

Not needed, confirmed by exploration: the issue suggests "add a short perft run if it stays cheap" —
`StratChessTests/PerftTests.cpp` already runs start-position depth 4 and Kiwipete depth 3 in the fast
tier, so move generation is already the densest thing the sanitizer leg will exercise. No new test.

## Design

### 1. `CMakeLists.txt` — two options, one branch

Two cache options, both empty/off by default so **the normal build is byte-for-byte unchanged**:

- `STRAT_SANITIZE` (STRING, default `""`) — passed straight through to `-fsanitize=`.
- `STRAT_SANITIZE_RECOVER` (BOOL, default `OFF`) — when OFF, adds `-fno-sanitize-recover=all`.

The recover knob exists because UBSan's default is *print and continue*, which exits 0 and would
leave CI green with findings on screen. `OFF` (the committed default) makes any finding fatal. `ON`
is the survey mode: paired with `ASAN_OPTIONS=halt_on_error=0` it collects every report in one pass
instead of stopping at the first. It is also what the next sanitizer to be added (TSan) will want.

Applied inside `strat_configure_target` (`CMakeLists.txt:97`), in the existing GNU/Clang `else()`
branch, to **both compile and link** options — `-fsanitize=` is required on the link line too:

```
-fsanitize=${STRAT_SANITIZE}
-fno-omit-frame-pointer        # readable stack traces
-D_GLIBCXX_ASSERTIONS          # see below
[-fno-sanitize-recover=all]    # unless STRAT_SANITIZE_RECOVER
```

`-D_GLIBCXX_ASSERTIONS` is included deliberately: ASan does **not** see past `std::vector::operator[]`
into a container's own heap block, so libstdc++'s own bounds assertions are what catch that class of
error. Free at this build's cost profile.

Guard clause ahead of the branch: if `STRAT_SANITIZE` is non-empty and the compiler is MSVC or has
the MSVC frontend variant, `message(FATAL_ERROR ...)` naming the Linux job. clang-cl accepts
`/fsanitize=address` but not the GNU spelling, and per #84 two of three clang-cl flag spellings fail
*silently* when wrong — an error is the only safe answer.

No preset, no `build.ps1` verb: this is Linux-only by design and belongs with the raw-CMake fallback
documented in `Docs/Workflow.md`.

### 2. `.github/workflows/build-and-test.yml` — one job

New `sanitize-linux` job, modelled on `build-linux` (same runner pin, same Ninja install, same
`cmake-deps-…` cache key — FetchContent sources are configuration-independent, as that job's own
comment already records):

```
needs: classify
if:   github.event_name == 'push' || needs.classify.outputs.is_full == 'true'
runs-on: ubuntu-24.04

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSTRAT_SANITIZE=address,undefined
cmake --build build --target StratChessTests --parallel
./build/StratChessTests '~[slow]'
```

No matrix — one leg, Debug. Add `sanitize-linux` to `build-and-test-result`'s `needs` and a `case`
arm handling `success` / `skipped` / everything-else, matching the shape of the two existing arms.

### 3. Docs

- `Docs/Workflow.md` §CI — a short paragraph: what the leg runs, why Debug, why Linux-only, and the
  `STRAT_SANITIZE` invocation for reproducing a CI finding locally (belongs alongside §Raw CMake
  invocation).
- `CLAUDE.md` — one clause in the CI paragraph. Do not expand it.
- `Docs/Changelog.md` — one line.

### 4. Follow-up issue: TSan

File after the PR: Lazy SMP's helper threads share a TT by design (`#91`/`#109`), TSan is the only
tool that finds a race before it corrupts anything, and it is Linux-only (no MSVC/clang-cl support).
Body should carry the open question from #179 verbatim — the shared-TT design is deliberately
racy-by-tolerance, so the real work is deciding which reports are signal before investing in
suppressions. Labels: `category:build-tooling`, `category:search`.

## Execution order

1. **Survey.** Export the tree to WSL and build under sanitizers with recovery on, so one pass
   reports everything rather than stopping at the first hit:

   ```bash
   # Windows side, from the worktree
   git archive --format=tar HEAD -o /c/Users/thees/AppData/Local/Temp/strat.tar
   # WSL side
   rm -rf ~/b && mkdir -p ~/b && tar -xf /mnt/c/Users/thees/AppData/Local/Temp/strat.tar -C ~/b
   cd ~/b && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
       -DSTRAT_SANITIZE=address,undefined -DSTRAT_SANITIZE_RECOVER=ON
   cmake --build build --target StratChessTests --parallel
   ASAN_OPTIONS=halt_on_error=0 UBSAN_OPTIONS=print_stacktrace=1 ./build/StratChessTests '~[slow]'
   ```

   Steps 1 and 2 of this order require the CMake change from the Design section to already be
   written — write it first, survey second. The three WSL approaches that fail (building over
   `/mnt/c`, cloning from a worktree path, cloning over `/mnt/c`) are recorded in memory; do not
   retry them. `sudo apt-get` inside WSL needs the user's password, so if `libasan`/`libubsan` turn
   out to be missing, ask rather than attempting it.

2. **Triage.** Classify each finding as: (a) real bug — fix here; (b) library/static noise (spdlog's
   registry, Catch2) — decide `ASAN_OPTIONS=detect_leaks=0` or an LSan suppression file, with the
   reason in a comment; (c) too large for this PR — file its own issue and record it in the PR body.
   Do not silence anything without naming why. The `new`/`malloc` grep over `StratEngine/` is clean
   (zero raw allocations), so LSan is unlikely to dominate, but this is a prediction, not a result.

3. **Fix** the (a) findings. If any turns out to be a real engine correctness bug rather than a
   latent one, it needs its own Elo consideration and should be called out separately — do not
   quietly bundle a behaviour change into a CI PR.

4. **Land** the CMake option, the CI job and the docs, with `STRAT_SANITIZE_RECOVER` at its `OFF`
   default so the job is enforcing.

5. **File the TSan follow-up issue.**

## Verification

- **Local Linux (WSL):** `./build/StratChessTests '~[slow]'` under
  `-DSTRAT_SANITIZE=address,undefined` exits 0 with no ASan/UBSan output. This is the substantive
  check — it is the same command the CI job runs, on the same GCC 13.3 / Ubuntu 24.04 the runner
  ships.
- **Default build unaffected:** `cmake --build build --verbose` on a normal configure shows **no**
  `-fsanitize`, no `-D_GLIBCXX_ASSERTIONS`. Because the default path adds nothing, no nps
  measurement is owed — `Run-Bench` here would be over-validation of a no-op.
- **MSVC-frontend guard fires:** configuring a Windows preset with `-DSTRAT_SANITIZE=address` fails
  at configure time with the intended message rather than producing an unsanitized binary.
- **`Validate-PrePR.ps1`** — this diff is Build tier (`CMakeLists.txt`, `.github/**`), so it runs
  everything; there is no judgement call.
- **`Get-ChangeTier.ps1 -SelfTest`** — unchanged behaviour expected, but it is cheap and the diff
  touches the files it classifies.
- **Apply the `windows-ci` label to the PR.** The diff touches `CMakeLists.txt`, and the Windows leg
  is the only job that compiles `strat_configure_target`'s clang-cl branch — which this change edits
  the neighbourhood of.

## Invariants after this change

- With `STRAT_SANITIZE` unset (every existing preset, every existing job), the produced binaries and
  compiler command lines are identical to before.
- A sanitizer finding fails the CI job — it never prints and exits 0.
- No sanitizer flag can reach a Windows build silently; requesting one is a hard configure error.
- Nothing is suppressed without a comment stating why.

## Outcome

The survey found **nothing**: 250 test cases / 3520 assertions, zero ASan findings, zero UBSan
findings, zero leaks with `detect_leaks=1`. The triage and fix steps above were therefore empty, and
the job landed enforcing rather than advisory as originally hedged. WSL needed no `apt-get` — GCC
13.3.0 ships `libasan.so.8` and `libubsan.so.1`.

A clean first run is weak evidence on its own, so the gate was checked against two deliberate faults
rather than trusted because it was quiet. `1 << 64` produced `runtime error: shift exponent 64 is too
large` and exit 1; a heap out-of-bounds write produced `AddressSanitizer: heap-buffer-overflow` and
exit 1. Both confirm that `-fno-sanitize-recover=all` turns a finding into a failed job, which is the
one property the whole job depends on and the one that fails silently if it is wrong.

The MSVC/clang-cl guard was confirmed to fire on both compilers, and the shipping clang-cl build's
`compile_commands.json` contains zero occurrences of `fsanitize`, `_GLIBCXX_ASSERTIONS` and
`fno-omit-frame-pointer` — the default path is untouched.

Filed rather than fixed here: **#184** (TSan over the shared TT) and **#185** (`classify` cannot
classify a push, so every merge to `main` runs full Linux validation regardless of tier — a
pre-existing condition this change made 50% more expensive by adding a third job to that trigger).
