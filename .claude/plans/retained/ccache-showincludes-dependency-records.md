# clang-cl `/showIncludes` dependency records under ccache — Design

**Issue:** #519

## Goal

On Windows the shipping toolchain is clang-cl + Ninja, and `build.ps1` puts `ccache` in front of the
compiler. Since CMake [`ed0f48e`](https://github.com/Kitware/CMake/commit/ed0f48e79bd7cffe1fcc18cc6b8539219f14f6d6)
(13 February 2026) CMake asks clang-cl for GCC-style depfiles (`-clang:-MD -clang:-MF<path>`) instead
of `/showIncludes`, so that `#embed` dependencies can be reported. ccache 4.14 does not decode the
`-clang:` prefix ([ccache #1775](https://github.com/ccache/ccache/issues/1775); draft
[PR #1780](https://github.com/ccache/ccache/pull/1780) is incomplete), so it stores no dependency
file: a cache **hit** replays the object and leaves Ninja with an **empty** dependency record for it.
A later header edit then does not rebuild that object. `Assert-ArtifactFresh` in `build.ps1` catches
the resulting stale executable and exits 1, but the failure is *stuck* — nothing will rebuild the
object — and the only cure is deleting the build directory. Driving `ninja` or `cmake --build`
directly skips that check entirely and hands over a wrong binary silently.

Restoring `/showIncludes` for exactly this configuration returns the project to CMake's pre-`ed0f48e`
behaviour, where dependency information travels in the compiler's stdout that ccache already replays,
and keeps the ~30–40 s warm clean-build saving from #515/#517.

## Scope

**This change will:**

- Set `CMAKE_DEPFILE_FLAGS_CXX` / `CMAKE_CXX_DEPFILE_FORMAT` back to the `/showIncludes` + `msvc`
  pair, guarded to Ninja + clang-cl + the ccache launcher this build configures.
- Add a build-graph gate that fails the build unless the *generated* clang-cl C++ rules for the
  project's own targets have `deps = msvc`, `/showIncludes` in the command, and no `depfile =`.
- Correct `Docs/Workflow.md` → Compiler cache, which currently documents the defect as live, and
  record the winget PATH trap that silently disables the cache.

**This change will not:**

- Touch `build.ps1`, its launcher lifecycle, or `Assert-ArtifactFresh` — the freshness assertion stays
  as defence in depth, not as the fix.
- Reconcile the `/showIncludes` prefix between CMake and ccache, add an `#embed` guard, add a
  `#pragma message` fixture, or add PowerShell self-test fixtures. See D4.
- Enable cross-worktree cache sharing. `base_dir` stays rejected (#510/#511); caching remains
  same-worktree only.
- Switch caches (sccache), pin an older CMake, or wrap the compiler in a custom depfile shim.
- Set the C-language equivalents. The project enables `CXX` only.

## Decisions

### D1: Restore `/showIncludes` rather than wait, wrap, or switch

`/showIncludes` + `deps = msvc` is the mechanism CMake itself used for clang-cl until February 2026,
and Ninja's msvc dependency path is ccache's supported replay path: the include notes are part of the
cached stdout. Rejected: waiting for ccache (no released fix, and the draft PR is incomplete);
pinning CMake (freezes the whole toolchain for one flag); a depfile wrapper or sccache (new machinery
and a new cache to validate, for a two-line setting). The cost of D1 is that `#embed` dependencies
would go untracked — see D5.

### D2: Guard on the frontend variant and the launcher, never on the pre-state

`CMAKE_CXX_DEPFILE_FORMAT` is **empty**, not `"gcc"`, on the defaulted GCC-style path — confirmed by
configure-time probe. A guard that reads the variable and expects `gcc` before overriding never
fires. The guard therefore keys off `CMAKE_GENERATOR` (Ninja), `CMAKE_CXX_COMPILER_ID` (Clang),
`CMAKE_CXX_COMPILER_FRONTEND_VARIANT` (MSVC) and the launcher being the `ccache` string this build
configures — all facts about the *configuration*, not about what CMake happened to default to. The
post-state is asserted separately (D3).

Narrowing to the ccache launcher is deliberate: without a launcher, CMake's GCC-style depfiles work
correctly and are the better-supported path. The override exists to work around a cache, so it applies
only when the cache is present.

### D3: Gate on the generated rule shape, inside the build graph

The override drives undocumented CMake internals: the variable names, the rule-generation logic and
the defaults can all change in a CMake upgrade, and a *silent* revert to GCC depfiles reproduces
exactly the stale-object defect this fixes. Checking the CMake variables at configure time would only
restate what we just set. The gate therefore parses the generated `rules.ninja` and requires, for the
project's own clang-cl C++ compile rules: `deps = msvc`, `/showIncludes` in the command, and no
`depfile =` binding.

It runs as a custom target that the two executables depend on, so it is ordered before their object
compiles in the generated graph — a bare `ninja` or `cmake --build` cannot bypass it, which matters
precisely because those are the invocations `Assert-ArtifactFresh` never sees. Rejected: a
`Validate-*.ps1` check (bypassed by the invocations that need it most) and a configure-time
`message(FATAL_ERROR)` (checks the input, not the output).

### D4: No prefix reconciliation, and no fixtures for failures we have not observed

Two facts, both confirmed by probe, retire the prefix work that an earlier draft of the contract
carried: `msvc_deps_prefix` is **already emitted** by CMake into `rules.ninja` on both dependency
paths under its own `# localized /showIncludes string` comment, so the override only inherits it; and
a literal `STREQUAL` between `CMAKE_CXX_CL_SHOWINCLUDES_PREFIX` (`Note: including file: `, trailing
space) and ccache's `msvc_dep_prefix` (`Note: including file:`, no trailing space) **false-fails on a
correctly configured English-locale machine**. Any such comparison would have to be
trailing-whitespace tolerant — which is a reason not to build it for a mismatch nobody has
reproduced. In ccache's default direct mode a successful stdout replay already supplies the include
notes Ninja consumes. Likewise no `#pragma message` fixture: it does not faithfully reproduce
[Ninja #2138](https://github.com/ninja-build/ninja/issues/2138)'s reported triggers.

### D5: `#embed` is already unreachable, so its dependency tracking is not a regression here

`ed0f48e`'s motivation was `#embed`. Under this project's contract — `clang-cl /nologo /std:c++20
/W4 /WX` — a valid `#embed` initializer fails with `#embed is a Clang extension
[-Werror,-Wc23-extensions]`. The language and warning settings already forbid it, so nothing in the
tree can depend on the GCC-depfile path. Adopting a language mode where `#embed` is supported is a
revisit condition, not a silent upgrade.

## Assumptions I cannot verify from the code

- **ccache replays the `/showIncludes` notes on a hit.** Verified by reproduction on two machines
  (CMake 4.3.1-msvc1, clang-cl 22.1.3, ccache 4.14, Ninja): with the override, `ninja -t deps` is
  non-empty on both the miss and the hit; without it, the hit yields 0 dependencies. Re-verified on
  the full build during implementation (see Validation).
- **Ninja's stdout/stderr merging does not corrupt include-note parsing under this project's flags.**
  Not directly falsified; [Ninja #2138](https://github.com/ninja-build/ninja/issues/2138) is open.
  Its reported triggers are colour diagnostics interleaving with include notes; this build is `/WX`,
  so a diagnostic ends the compile rather than interleaving with it. Residual risk, retained below.
- **CMake keeps populating `CMAKE_CXX_CL_SHOWINCLUDES_PREFIX` for clang-cl** even though it no longer
  uses the msvc depfile format by default. Verified by configure-time probe; the D3 gate is what
  detects a future change of heart.

## Invariants

- After a ccache hit, no compiled object in the build has an empty Ninja dependency record.
- Editing a header after a hit recompiles its dependants and changes the executable.
- A cached object, library and executable are byte-identical to their uncached counterparts (this is
  what `/Brepro` in `CMakeLists.txt` already makes checkable on Windows).
- The override and the gate are inert for every configuration outside Ninja + clang-cl + ccache —
  Linux, MSVC, and a launcher-less clang-cl tree generate exactly what they do today.
- `Assert-ArtifactFresh` and the compiler-launcher lifecycle in `build.ps1` are unchanged.

## Validation

Build tier, plus a dependency-integrity sequence that no existing gate covers. No Elo match: this
changes no compiled code, and byte-identity of the produced binaries is asserted rather than assumed.

- The reported defect sequence: baseline → edit a header → revert → make the same edit again; the
  final step must recompile and change the executable.
- After a warm build, `ninja -t deps` over the whole tree: 100% of compiled objects have a non-empty
  record, and both `Eval.cpp.obj` rows retain their dependencies.
- All three entry points exercised — `build.ps1`, bare `ninja`, bare `cmake --build` — since the last
  two bypass the freshness assertion and are what D3's placement is for.
- Release **and** Debug clang-cl presets.
- Byte-comparison of cached vs. uncached objects, libraries and executables.
- Warm clean builds repeated enough to confirm they stay well under the ~44 s uncached baseline. The
  4.85 s single confirmation run in #519 is **not** a target; the honest claim is preserving
  #515/#517's ~30–40 s saving on a warm clean same-worktree build.
- Negative check on the gate: force the pre-change rule shape and confirm the build fails.

## Value boundary and revisit conditions

**Value boundary.** Same-worktree caching only. `$in` and `$INCLUDES` are absolute and hashed, so a
new worktree's first build is a full cold compile; the beneficiaries are clean rebuilds, reverts,
branch switches, bisects and build-A/build-B/build-A comparisons. No-op builds were already cheap.
ccache 4.14's `/showIncludes` path rewriting is tied to its MSVC compiler type while clang-cl is
classified separately, so cross-worktree sharing would still replay absolute paths identifying the
populating tree — it needs its own integrity gate and likely upstream support (#510/#511).

**Remove or revisit this override when any of these holds:**

1. An available ccache release records dependencies for `-clang:-MD -clang:-MF` command lines
   (ccache #1775 / PR #1780) — then CMake's default path works and the override and its gate go.
2. The project adopts a language mode where `#embed` is supported, or drops the `-Wc23-extensions`
   error that currently forbids it — GCC-style depfiles are then load-bearing (D5).
3. Evidence appears that Ninja #2138's stream-merging defect affects this configuration — a truncated
   or malformed include-note parse under `/showIncludes` would make the residual risk real.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Why `/showIncludes` is restored, and the removal condition | source comment on the override block in `CMakeLists.txt` (live invariant + removal condition, no issue refs) |
| Why the gate reads the generated rule rather than the CMake variables | source comment in the gate script |
| Defect is fixed; the "live defect (#519)" paragraph is now wrong | `Docs/Workflow.md` → Compiler cache |
| winget ccache lands in a version-pinned directory with no PATH shim, so the cache is silently inert | `Docs/Workflow.md` → Compiler cache |
| Value boundary, residual Ninja #2138 risk, the three revisit conditions | this document (retained) and the PR body |
| Reproduction table, measurements, upstream links | #519 and the PR body |
