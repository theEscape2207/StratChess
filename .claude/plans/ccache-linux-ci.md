# ccache on the Linux CI builds — Design

**Issue:** #92

## Goal

A Build- or Engine-tier run recompiles every translation unit from scratch in four Linux
configurations, and three of those jobs sit within ~70 s of each other at the top of the run
(measured on run `32669041340`: `build-linux (Release)` 299 s, `sanitize-linux` 306 s,
`build-linux (Debug)` 237 s). Roughly half the TU count of a `StratChessTests` build is Catch2 —
~60 modular TUs against 62 of ours — and Catch2 is pinned by `GIT_TAG`, so nothing we commit can
change it, yet it is rebuilt in every configuration on every run. The only currency this buys back
is PR feedback time; the repository is public, so runner minutes are free and are not a
justification for anything here.

This is `priority:longterm` work. It carries no risk to the shipping binary, no effect on playing
strength, and its value is entirely a wall-clock number that has to be measured rather than
asserted — see D5.

## Scope

**This change will:**

- Put `ccache` in front of the compiler on the four Linux build jobs of `build-and-test.yml`:
  `build-linux` (Release and Debug), `sanitize-linux`, `tsan-linux`.
- Install `ccache` from its upstream static release tarball, not from apt (D1).
- Keep one `actions/cache` entry per configuration, never shared (D3).
- Prove the cached path produces a byte-identical binary before it is trusted (Validation).
- State a kill criterion and act on it if the measurement misses (D5).

**This change will not:**

- Touch the Windows clang-cl legs. clang-cl plus ccache is the least-trodden combination here, and
  the Windows legs already run in parallel with the Linux ones — caching them moves the run's wall
  clock only if they become critical, which they are not.
- Touch `lint-linux`. It configures a compile database and never compiles.
- Touch `nightly.yml`. Nothing there is on the PR feedback path.
- Decide anything about #281. See "Relationship to #281".

## Decisions

### D1: install ccache from the upstream static tarball, not apt

`ccache` is **not** preinstalled on the `ubuntu-24.04` runner image (checked against the
runner-images manifest: Ninja 1.13.2 and CMake 3.31.6 are listed, ccache is not). The obvious
approach — `hendrikmuhs/ccache-action` — installs it with apt, and `build-and-test.yml` carries an
explicit standing decision against exactly that:

> Every Linux job below pins ubuntu-24.04, whose image ships Ninja and CMake on PATH — `-G Ninja`
> needs no install step. Do not add one: it puts an Ubuntu apt mirror on the critical path of every
> job, and a stalled mirror hangs `apt-get update` indefinitely.

Adding four apt touches per run to save wall clock is self-defeating in the tail case the standing
decision exists to prevent. `lint-linux` does accept one bounded apt step for LLVM, so the decision
is not absolute — but that step buys a toolchain that has no other distribution channel, and this
one does.

**Chosen:** download `ccache-4.13.6-linux-x86_64-musl-static.tar.xz` from the ccache GitHub release,
verify its SHA-256 against a pinned literal, extract the single static binary onto `PATH`, and cache
it with `actions/cache` keyed on the version. github.com is already on the critical path of every
job (checkout, and the FetchContent clone on a cold cache), so this adds no new dependency — only
another request to a host the run cannot proceed without anyway.

**Rejected:** `hendrikmuhs/ccache-action` (apt, per above); building ccache from source (minutes,
absurd for a build accelerator); vendoring the binary in the repository (a 6 MB binary in a source
tree, updated by hand).

**Version pinned to 4.13.6, not the newer 4.14.** 4.14 was released 2026-08-23, one day before this
document. A required status check is not the place to be an early adopter of a compiler cache;
4.13.6 has been out since 2026-05-04. Bumping it later is a one-line change plus a new hash.

### D2: `CMAKE_CXX_COMPILER_LAUNCHER`, not a `CC`/`CXX` wrapper

`-DCMAKE_CXX_COMPILER_LAUNCHER=ccache` on each Linux configure line. It is the supported CMake
mechanism, works with the Ninja generator these jobs already use, and leaves `CMAKE_CXX_COMPILER`
reading as the real compiler — which matters because `lint-linux` consumes a compile database and a
wrapper in the compiler field would put `ccache` where clang-tidy's driver expects a compiler.
`lint-linux` configures its own database and is out of scope here, but the failure mode is one
sentence away and not worth inviting.

C is not launched: the project has no C sources.

### D3: one cache entry per configuration, never shared

Keys: `ccache-linux-release`, `ccache-linux-debug`, `ccache-linux-asan-ubsan-stdlibdebug`,
`ccache-linux-tsan`.

Sharing would still be **correct** — the sanitizer flags and `-D_GLIBCXX_DEBUG` are part of the
compiler command line and therefore part of ccache's own hash, so a Release object can never be
served to a TSan build. The split exists for hit rate and eviction behaviour, not for safety. This
is the opposite of the reasoning on the existing FetchContent entries, whose key is deliberately
*not* matrix-scoped because those sources are identical across configurations; the note in the
workflow explaining that must not be read as applying here.

Each entry's key carries the run id, with a `restore-keys` prefix falling back to the newest earlier
entry for that configuration — `actions/cache` entries are immutable, so a cache that grows across
runs has no other shape. `CCACHE_MAXSIZE` is set to 400 MB per configuration, so the four total
about 1.6 GB against the repository-wide 10 GB budget. See D5 for why that cap is load-bearing.

### D4: the LTO link stays uncached

`build-linux (Release)` sets `INTERPROCEDURAL_OPTIMIZATION_RELEASE` on the `StratChessEvolved`
target. ccache caches compilation, not the LTO link, so that link remains on the critical path in
full and caps what this change can do to the Release job specifically. This is a stated limit, not
a problem to solve.

### D5: a kill criterion, fixed before the measurement

The scheme in D3 writes a **new** cache entry per job per run — unavoidable, since `actions/cache`
entries are immutable — against a repository-wide 10 GB LRU budget that already holds the
FetchContent entries. The failure mode that matters is not a low hit rate — it is ccache churn evicting the
FetchContent entry, whose miss costs a fresh dependency clone of about a minute. That turns this
change net negative while every job still reports green.

So: **if the median run wall clock does not drop by at least 20 s, or the FetchContent entry starts
missing, revert.** The revert is the inverse of the landing diff, with no other consequences.

A `gh cache list` inspection one week after landing is part of the work, not a follow-up someone
might do. The number it produces is the only evidence that eviction is not happening.

## Assumptions I cannot verify from the code

- **ccache's hashing is correct for these configurations** — specifically that `-fsanitize=`,
  `-D_GLIBCXX_DEBUG` and the constexpr-limit flags all enter the hash, so a cached object cannot
  cross configurations. Taken on trust from ccache's documented behaviour, and the per-configuration
  keys in D3 mean a hashing bug would have to defeat both mechanisms at once. *Verified by:* the
  cold-vs-warm binary comparison in Validation, which would catch a wrong-object hit as a hash
  mismatch.
- **GCC's LTO link is bit-reproducible** given identical inputs on the same runner image. Not
  verified. If the Release `StratChessEvolved` binary differs cold vs warm, the fallback is to
  compare the `StratChessTests` binary (no IPO) plus the object files under `build/`, and to record
  in the PR body that the Release link was compared at object level rather than binary level.
- **`ccache-4.13.6-linux-x86_64-musl-static.tar.xz` runs on the `ubuntu-24.04` image.** A static musl
  build has no libc dependency, so this is close to certain, but it is untested here. *Verified by:*
  the first CI run — a broken binary fails the configure or build step immediately and loudly.

## Invariants

- Every Linux job produces the same binary with a warm cache as with a cold one.
- No Linux job gains a dependency on an Ubuntu apt mirror (D1).
- A cache miss is a slowdown, never a failure: every job must still pass with the cache empty, which
  is what the first run after landing demonstrates for free.
- `build-and-test-result` semantics are untouched — no job added, none removed, no condition changed.

## Validation

**Build tier** — the diff is `.github/workflows/build-and-test.yml` plus documentation, and the
tier classifier treats a workflow edit as Build. No Elo match: the change cannot affect the shipping
binary, search behaviour or evaluation. `Run-Bench` is likewise irrelevant — ccache changes how the
binary is produced, not what it is, which is precisely what the binary comparison below asserts.

**The gate: byte-identical binary, cold versus warm.** On the landing PR, run the workflow twice on
the same commit. The first run populates the cache; the second hits it. A temporary step printing
`sha256sum` of `build/StratChessEvolved` and `build/StratChessTests` in each Linux job supplies the
comparison, and the two sets of hashes go in the PR body. The step comes out before merge — it is
evidence for landing the change, not a permanent check. Fallback if the Release binary differs, per
the assumptions above: compare object files and say so explicitly.

**The number.** Measured with #372's method, which is the only one shown to reproduce here: re-run
merge commits on `main` one at a time, two samples each, reading `sanitize-linux` and
`build-linux (Release)`. Do **not** batch `gh run rerun` — the runs share a concurrency group and
each queued rerun cancels the previous one while reporting success (#372's method note). Do not
quote #92's "~90 s": it was costed against `build-linux (Release)` as sole critical path, which
#372 showed it is not.

## Relationship to #281

#281 asks whether to adopt include-what-you-use or keep the `StdAfx.h` umbrella. Its only surviving
argument for IWYU is incremental build time. CI has no incremental state today; **this change is
what gives it one**, so it is the change that makes header fanout start costing PR feedback time
instead of being theoretical.

The effect runs both ways, and neither way is decisive:

- ccache partly *substitutes* for IWYU. An untouched TU is a cache hit however fat its include set,
  because the 21 STL headers `StdAfx.h` pulls in never change between our commits.
- It also sharpens the cost of headers that *do* change: 11 of the last 12 code-touching merges on
  `main` touched at least one header.
- Most of that fanout is not the umbrella's doing. `StdAfx.h` includes exactly two project headers,
  `Compat.h` and `defines.h`; a change to either invalidates everything with or without IWYU, and
  IWYU would not fix it.

Neither issue blocks the other, and this change does not pre-commit the umbrella decision. It does
produce the per-PR hit-rate data that #281's "confirm build times before/after" definition of done
otherwise has no way to obtain, which is a reason to do this one first.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| ccache is not on the `ubuntu-24.04` image, so the apt standing decision applies — and how the tarball install sidesteps it (D1) | comment in `build-and-test.yml`, beside the install step |
| Per-configuration keys are for hit rate, not correctness; the FetchContent "not matrix-scoped" note does not apply here (D3) | comment in `build-and-test.yml`, beside the first ccache cache step |
| The LTO link is not cached and caps the Release saving (D4) | comment in `build-and-test.yml` |
| Kill criterion and the eviction risk to the FetchContent entry (D5) | this document until landed, then `Docs/CI.md` |
| What each Linux job actually caches, and that Windows deliberately does not | `Docs/CI.md` |
| Measured before/after wall clock, and the cold-vs-warm hashes | `Docs/Changelog.md` and the PR body |
| The #281 interaction | a comment on #281, so the decision there inherits it |

Delete this file once the row destinations above exist and the one-week `gh cache list` check has
been recorded on #92.

## Execution notes

Not durable; delete with this section when the work lands.

The work splits cleanly by how much judgement each part needs, and most of it needs little:

| Part | Model |
|---|---|
| The workflow diff — install step, four launcher flags, four cache steps, comments from the Harvest table | Lower tier (Haiku/Sonnet). Mechanical once D1–D4 are fixed; the closed list is in this document. |
| `Docs/CI.md` and `Docs/Changelog.md` edits | Lower tier, same PR. |
| Cold-vs-warm hash comparison, reading the two runs | Lower tier to collect, controller to judge — a mismatch needs the D4/LTO reasoning to interpret. |
| The wall-clock measurement and the kill-criterion call | Controller. #372's method has a trap that reports success while cancelling runs, and the verdict is a judgement about noise bands, not a threshold check. |
| The one-week `gh cache list` eviction check | Lower tier to collect; one line of judgement to close. |

One PR. Splitting the workflow edit from the docs would put a Build-tier change and a Docs-tier
change through two full validation cycles for a diff of well under a hundred lines.
