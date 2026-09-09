# ccache launcher lifecycle — Design

**Issue:** #515

## Goal

A full local build of the two clang-cl presets takes ~45 s, of which 82% is compilation of sources
that did not change between worktrees, branches or `build/` wipes. ccache turns the repeat of that
work into a cache read: measured 44.7 s → 11.7 s warm, a 3.8x saving, for +1.1% on a cold build.

The cost of switching it on is not the wiring — CMake already emits `${LAUNCHER}` into every compile
edge, so one cache variable is the whole mechanism. The cost is the *lifecycle*: a build tree records
the launcher decision at configure time and never revisits it, so a tree configured before ccache was
installed silently never caches, and a tree configured with it hard-fails on the first edge once
ccache leaves PATH. `build.ps1` configures only when `CMakeCache.txt` is absent, so neither state
repairs itself. `Docs/CI.md` forbids a `build.ps1` parameter for this, so the transition must be
detected, not declared.

## Scope

**This change will:**

- Add `Get-SharedCompilerCache`, modelled on `Get-SharedDepsCache`: `$null` under `GITHUB_ACTIONS`
  or when ccache is unresolvable, otherwise a `StratChessCcache` directory beside the main checkout.
- Configure `CMAKE_CXX_COMPILER_LAUNCHER=ccache` on a fresh tree, and reconcile an existing tree
  whose recorded decision no longer matches the machine.
- Set `CCACHE_DIR`, `CCACHE_MAXSIZE=1G` and `CCACHE_COMPILERCHECK=content` for the build.
- Set `CCACHE_DISABLE=1` around the configure call only.

**This change will not:**

- Wire `windows-msvc`. Conservatism plus 78.5 MB of cache per generation, not a correctness
  requirement — #511's MSVC mangling hazard is cross-worktree only, and this change is single-tree.
  #516 can lift the exclusion without re-arguing safety.
- Set `base_dir` or `hash_dir`, so cross-worktree hits remain impossible (#510, #511, #516).
- Add a `build.ps1` parameter, or touch CI.

## Decisions

### D1: The launcher is recorded as the bare string `ccache`, never a resolved path

CMake stores the launcher verbatim — `CMAKE_CXX_COMPILER_LAUNCHER:STRING=ccache`, `LAUNCHER = ccache`
in `build.ninja` — and resolves it through PATH on every compile edge. Keeping it a bare word is what
makes the lifecycle a two-state question ("does this tree use a launcher, does this machine have
one") instead of a path-staleness question.

`Get-SharedDepsCache`, the function this one is modelled on, returns a resolved absolute path, so
copying it faithfully is the way to get this wrong. The ccache install here is a *versioned* WinGet
package directory with no `Links` shim, so a 4.14 → 4.15 upgrade moves the executable: a recorded
absolute path would leave every configured tree hard-failing at its first compile edge, with a stored
path that no longer exists. The bare string absorbs that upgrade silently.

Rejected: resolving the path for a clearer error when ccache is missing. The clearer error is not
worth converting a routine upgrade into a broken tree.

### D2: A mismatched tree is repaired in place, not deleted or reported

The four lifecycle states, given a tree that already has a `CMakeCache.txt`:

|                        | PATH has ccache | PATH lacks ccache        |
|------------------------|-----------------|--------------------------|
| **cache says launcher**| steady state    | hard failure at first edge|
| **cache says none**    | silent no-op    | steady state              |

Both off-diagonal cells are repaired by `cmake -D` (or `-U`) on the existing tree, which is cheap and
needs no wipe. Ninja then rebuilds everything once, because `LAUNCHER` is part of the command line —
an 11.7 s event with a warm cache, 45 s without. The repair prints a line saying what it did and why.

Rejected: deleting the tree (throws away link artifacts and the `_deps` configure for no gain), and
reporting without repairing (leaves the "silent no-op" cell exactly as broken as it is today, which
is the cell this repository is in right now).

### D3: `compiler_check = content`, at a measured +12–21%

Warm builds, three per setting: `mtime` 11.7 s, `string:clang-22.1.3` 12.7 s, `content` 13.1–14.2 s.
`content` hashes the compiler binary on all 121 invocations and ccache has no inode cache on Windows,
so nothing amortises it.

Taken anyway: 1.4 s against a 33 s saving, and `mtime` can serve objects from a compiler that was
rebuilt or replaced in place, which is the one failure a compiler cache must not have. `string:` is
the cheaper middle option but its value is hand-maintained — bump clang, forget the string, get stale
objects. If 12% ever matters, the fix is `string:` *derived* from `clang-cl --version`, not `mtime`.

An invalid value is worse than any of these: ccache treats an unrecognised `compiler_check` as a
*command to run*, fails it once per compile, exits 0 and caches nothing — 42.5 s instead of 11.7 s,
with no error anywhere the build shows. The value is a literal in one place for that reason.

### D4: `CCACHE_DISABLE=1` around configure only

CMake's `TryCompile-<random>` probe directories are named freshly per configure, so their ~14 cache
entries can never be hit again. Disabling the cache for the configure call stops them at source
rather than sizing the cap around them. Verified: +0 cache files with the variable set, +2 without.

## Assumptions I cannot verify from the code

- **ccache resolves through PATH at build time on every edge.** Verified: `build.ninja` contains
  `LAUNCHER = ccache` with no path, and builds succeed with only the WinGet package directory on
  PATH.
- **`ccache` on PATH is 4.14 or newer.** Not verified at runtime; earlier versions predate the
  clang-cl support this depends on. Not guarded — an older ccache degrades to misses, not to wrong
  output, and this is a single-developer machine.
- **A cache-served object is byte-identical to a freshly compiled one.** Verified: 109 of 112
  artifacts identical, the three exceptions being CMake's own configure probes — an LTO test, the
  compiler-id binary and the `/showIncludes` probe. Those are rebuilt from scratch on every configure
  and are not reproducible; D4 keeps them out of the cache, not out of the tree, so the correctness
  gate names them explicitly rather than comparing them.

## Invariants

- A build with ccache absent behaves exactly as it does today.
- `GITHUB_ACTIONS` builds are untouched — no launcher, no ccache environment.
- The launcher recorded in `CMakeCache.txt` is the bare string `ccache` (D1).
- `windows-msvc` trees never gain a launcher.
- The cache directory lives beside the main checkout, shared by every worktree.

## Validation

Tooling tier. Two gates, because they fail differently:

- **Performance:** a second full build of `windows-clang-cl` is ≤ 20 s against a ~45 s first build.
  This is what catches silent non-caching (D3's invalid-value failure produces a green build).
- **Correctness:** SHA-256 over every `.obj`/`.lib`/`.exe` of a cache-served build matches a build
  configured with no launcher. This is what catches a wrong hit, which no timing can see.

Plus `build.ps1 -SelfTest` for the four lifecycle states as a pure decision function, and
`Validate-PrePR.ps1`.

No Elo match: the compiler, its flags and the sources are unchanged, and the byte-identity gate is a
stronger statement than any match could make.

Run on the finished branch: 44.1 s reference, 9.4 s repair-and-rebuild, **12.5 s** warm on a wiped
tree, 112 of 112 artifacts identical, both repairs reported, `Validate-PrePR.ps1` PASSED at Build
tier. The warm figure is 0.8 s above the design-time 11.7 s, which was measured before
`compiler_check = content` was adopted (D3).

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Bare string, never a resolved path (D1) | comment on `Get-CompilerLauncherAction`; `Docs/Workflow.md` → Compiler cache |
| The four lifecycle states and the in-place repair (D2) | same comment, plus the four `-SelfTest` cases |
| `compiler_check` cost and the invalid-value trap (D3) | comment on the environment block; `Docs/Workflow.md`; `Docs/Changelog.md` |
| The measured before/after and the cache convention | `Docs/Workflow.md` → Compiler cache; `Docs/Changelog.md`; `CLAUDE.md` one-liner |
| `CCACHE_DISABLE` around configure (D4) | comment on `Invoke-CMakeConfigure`; `Docs/Changelog.md` |
| #510/#511 are upside, not prerequisites | `Docs/Changelog.md`, and #509's children table |

Harvest is complete, nothing in the tree cites this file, and it holds no spec or ADR role — the
work has landed. It is deleted in this PR once design review is done.
