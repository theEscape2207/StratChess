# Full Build + Test CI on GitHub Actions

## Context
The repo now has two lightweight CI workflows (branch-cleanup, FEN-check) but no
workflow that actually compiles the engine or runs the test suite. Every "tests pass"
claim on a PR is still self-reported by the same session that wrote the code. This plan
adds an independent build+test gate on PRs into `main`.

The hard constraint: this is a **Windows-only, C++20** project. Dependencies are NOT
vendored — they are resolved from sibling checkouts via `$(DepsRoot)` in
`Directory.Build.props`. So CI must both provision the toolchain and fetch/point at deps.

## Key facts (verified this session)
- **PlatformToolset `v145`** (VS 2026 / VS 18; local MSBuild is 18.7.8), `stdcpp20`, `WindowsTargetPlatformVersion 10.0`, **x64 only**.
- Deps + pinned versions: **spdlog v1.16.0**, **nlohmann/json ~v3.12.0** (tag base v3.11.3+), **Catch2 v3.13.0 amalgamated** (`catch_amalgamated.cpp` compiled straight into the test project).
- `$(DepsRoot)` defaults to the repo's parent dir and must CONTAIN `json/`, `spdlog/`, `Catch2/`. Overridable via `Directory.Build.user.props` (gitignored) → set `DepsRoot`.
- `build.ps1 all` builds both projects; `build.ps1 run-tests` runs the fast tier (`~[slow]`); the test exe lands at `StratChessTests\x64\Release\StratChessTests.exe`.
- The two existing workflows live in `.github/workflows/` (un-ignored via `.gitignore` negation rules).

## Toolset availability — VERIFIED, risk resolved
Original concern: hosted `windows-latest` historically shipped VS 2022 (**v143**) while the
project pins **v145**, so a hosted build could fail at toolset resolution (infra, not code).

**Verified 2026-07-01 against `actions/runner-images` manifests:**
- There is a GA image label **`windows-2025-vs2026`** = **Visual Studio Enterprise 2026 v18.7.11911.148**, installed at `C:\Program Files\Microsoft Visual Studio\18\Enterprise` — **matches the local VS 18 / MSBuild 18.7.8 toolchain**; default `VC.Tools.x86.x64` is the 18.7 (v145) toolset, older toolsets bundled for back-compat.
- `windows-latest`/`windows-2025` are migrating to this VS 2026 image as of June 2026 (issues #14017, #14004).
- **Conclusion:** pin the explicit `windows-2025-vs2026` label (stable, unambiguous) rather than the moving `windows-latest`. No self-hosted runner, no v143 retarget, no need to defer. Being a modern Azure VM, it also covers BMI2/AVX2 for future PEXT.

How to re-verify if labels shift: (1) runner-images per-image READMEs, (2) the runner-images issue tracker for label→image moves, (3) a one-shot probe workflow (`vswhere -property catalog_productDisplayVersion` + `MSBuild -version`), ~1 min quota.

## Benefits
- **Independent verification** — a build+test result not produced by the authoring session; catches "forgot to run tests", stale-binary, and env-specific passes.
- **Regression net on PRs** — extends the FEN check into real compile + Catch2 assertions before merge.
- **Documents the real build recipe** — the workflow becomes executable proof of exactly which deps/versions/toolset build the project from clean.

## Risks & costs
1. **Toolset mismatch (high, see above).** Mitigations: self-hosted runner on your machine (has v145), OR retarget CI to v143 (divergence risk: C++20/`/WX` behavior can differ between toolsets — a v143 pass wouldn't prove v145 builds), OR gate on VS 2026 landing on hosted images.
2. **Actions-minutes usage (low-moderate).** Windows runners bill quota at **2×** wall-clock. A full C++20 LTCG build + extended tests is minutes each. On the Free tier's 2,000 min/month with a **$0 spending limit**, worst case is workflows *stop running* until reset — **no money changes hands** unless you raise the limit. Self-hosted runners consume **zero** GitHub minutes.
3. **Dep-fetch fragility (low).** Cloning three repos at pinned tags each run adds latency and a network-failure surface; mitigated by shallow clones + actions/cache.
4. **Flaky self-play (low).** If we include self-play, its 60s-timeout/nondeterminism could cause spurious failures. Recommendation: **exclude self-play from CI**; keep it in the local `Validate-PrePR.ps1` only. CI runs build + Catch2 tests.

## Instruction-set / CPU-feature axis (relevant to the planned PEXT work)
Separate from the toolset (which is about *compiling*), CPU features are about *executing* the test binary.
- **Today: no exposure.** No `/arch:AVX2` on any project (default x64 = SSE2 baseline); zero PEXT/PDEP/BMI/`__popcnt`/`_tzcnt` intrinsics in the source. Current binary runs on any x64 CPU. CI as planned is unaffected.
- **When PEXT (Magic Bitboards) lands**, it introduces a **BMI2** dependency (PEXT's home — not AVX2, though both arrive together on Haswell+):
  - *Compiling* PEXT needs nothing from the runner CPU — MSVC emits `_pext_u64` regardless of build-host support.
  - *Running* a PEXT-exercising test requires the runner CPU to support BMI2 (absent a fallback).
- **Runner implications:** hosted `windows-latest` (modern Azure VMs, Haswell+) support BMI2/AVX2, so running PEXT tests works — caveat: AMD Zen1/Zen2 microcoded PEXT is ~18× slower, which only matters if benchmarking on CI (don't). **Self-hosted solves both the toolset AND the execute axis at once** (your machine has v145 and the CPU features).
- **Design mitigation (recommended for the PEXT item itself):** runtime CPU-feature detection with a classic-magic / ray-based fallback when BMI2 is absent (the Stockfish `USE_PEXT` pattern). Makes CI correctness CPU-agnostic and avoids breaking end users on older hardware.

## Recommended approach (decisions locked)
**Runner:** `windows-2025-vs2026` (verified v145). **Scope:** build + fast tests. **Cost:** proceed, $0 spending limit stays, `actions/cache` for deps. No self-play in CI (risk 4).

Single workflow `.github/workflows/build-and-test.yml` (already un-ignored by the `!/.github/workflows/*.yml` rule):

1. **Trigger/matrix:** `on: pull_request: branches: [main]`; `runs-on: windows-2025-vs2026`; config `Release`, platform `x64`.
2. **Checkout:** `actions/checkout@v4` into `${{ github.workspace }}` (`D:\a\StratChess\StratChess`).
3. **Provision deps** into a fixed deps dir (e.g. `${{ github.workspace }}\..\deps`), each shallow-cloned at a pinned tag:
   - **spdlog** `v1.16.0` → `deps/spdlog` (gives `spdlog/include/spdlog/…` ✓).
   - **nlohmann/json** `v3.12.0` → `deps/json` (matches local header macros MAJOR 3 / MINOR 12 / PATCH 0; gives `json/include/nlohmann/json.hpp` ✓).
   - **Catch2** `v3.13.0` → clone, then copy `extras/catch_amalgamated.{hpp,cpp}` into `deps/Catch2/` (the vcxproj references `$(DepsRoot)Catch2\catch_amalgamated.cpp` and include dir `$(DepsRoot)Catch2` — files must sit directly in `Catch2/`, matching the local layout). Pin exactly — assertion counts differ per Catch2 version.
   - Wrap the clone/copy in `actions/cache` keyed on the three pinned versions so steady-state runs skip network.
4. **Point the build at deps:** generate `Directory.Build.user.props` at repo root on the runner with `<DepsRoot>…\deps\</DepsRoot>` (trailing backslash; must CONTAIN `json/ spdlog/ Catch2/`). This is the documented, gitignored override mechanism — nothing committed.
5. **Build:** `.\build.ps1 all` — auto-discovers the v145 MSBuild via `vswhere` at the standard Installer path (version-independent); also bootstraps `core.hooksPath` (harmless on CI).
6. **Test:** run the fast tier and propagate exit code — `& StratChessTests\x64\Release\StratChessTests.exe ~[slow]; exit $LASTEXITCODE` (avoids the redundant test rebuild `build.ps1 run-tests` would do after step 5).

**Key correctness properties:** deps resolved at the exact pinned versions (assertion count must match the local fast tier, ~1546); job fails (red X) on any build error or test-assertion failure; no reliance on the moving `windows-latest` label.

## Open Question — RESOLVED by verification
The runner-strategy fork (self-hosted vs hosted-with-v143-fallback vs defer) was predicated on
uncertainty about v145 on hosted images. Verification confirmed a GA hosted image
(`windows-2025-vs2026`) with the exact v145 toolchain, so the recommended approach is
hosted CI pinned to that label. Remaining user decision is scope, not runner (see below).

## Verification (once implemented)
- Open a PR into `main`; confirm the workflow triggers and the build+test job goes green.
- Inspect the run log to confirm: correct toolset selected, deps resolved at pinned versions, Catch2 assertion count matches the local fast tier.
- If it fails on infra grounds (e.g. Catch2 `extras/` path wrong), fix and push follow-up commits to the same PR rather than opening a separate throwaway PR.
