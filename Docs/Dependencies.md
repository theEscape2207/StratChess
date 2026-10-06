# Dependency Handling

The engine has three external dependencies: spdlog, nlohmann/json and Catch2. **Adding a fourth needs
the project owner's approval** (`CLAUDE.md`). This file covers keeping the three current.

## Where they are pinned

`CMakeLists.txt` fetches each one with `FetchContent` at an exact `GIT_TAG`, deliberately without
`FIND_PACKAGE_ARGS`. A system or vcpkg copy at another version would otherwise satisfy the pin
silently. The comments around those declarations explain each consumption setting.

**Nothing automates this.** Dependabot (`.github/dependabot.yml`) bumps only the workflow actions,
because it cannot read `FetchContent` pins. To check for new releases by hand:

```bash
for r in gabime/spdlog nlohmann/json catchorg/Catch2; do
  gh release view -R $r --json tagName,publishedAt -q '"\(.tagName) \(.publishedAt)"' </dev/null
done
```

Read the release notes for every version you skip, not only the newest one.

## Bumping a version

1. Change the `GIT_TAG` in `CMakeLists.txt`.
2. Change the CI cache keys. The `actions/cache` keys for the dependencies spell every version
   literally, across `build-and-test.yml`, `nightly.yml` and `strength.yml`. A key you miss keeps
   restoring the old sources, and CI passes against a version the branch no longer pins. Find them
   with `grep -rn "catch2-v" .github/workflows`.
3. Delete both executables in `build/windows-clang-cl/`, then run `build.ps1 all`. `build.ps1`
   checks freshness by mtime, so a `CMakeLists.txt` edit makes an exe that Ninja correctly left
   alone look stale. The build then fails with no compiler error.
4. Validate according to what the dependency reaches (next section).
5. Add a `Docs/Changelog.md` entry that names the versions and the evidence. This is a Build-tier
   PR.

## What a bump must prove

**Catch2 links only `StratChessTests`.** Keep a copy of `StratChessEvolved.exe` from before the bump
and compare it byte for byte with the one built afterwards. Release builds are reproducible, so the
two must be identical. That proves more than a search-equivalence or nps run could. Then run the fast
tier and confirm that the test-case and assertion counts have not changed. A drop in either means
tests stopped registering.

**spdlog and nlohmann/json compile inside the engine's own translation units**, so a bump can change
the engine binary. Run `Compare-SearchEquivalence.ps1` and a bench pass (skill `measure-strength`),
and build on Linux/GCC: spdlog bundles `{fmt}`, and a `{fmt}` major-version jump is where the real
risk sits.

## Tripwires per dependency

- **Catch2 is compiled as unity units of 16 files.** A new version can define the same file-local
  name in two files of one unit, which is a hard compile error. Lower `UNITY_BUILD_BATCH_SIZE`.
- **Catch2's warnings are silenced by hand.** Its include directories are copied onto
  `INTERFACE_SYSTEM_INCLUDE_DIRECTORIES` because it has no system-include option. If a new version
  adds that option, use it instead. If it moves its include directories, warnings in its headers
  break the `/WX` build.
- **spdlog and nlohmann/json are silenced through their own options** (`SPDLOG_SYSTEM_INCLUDES`,
  `JSON_SystemInclude`). If an option is renamed, the old name does nothing without any message, and
  the next warning in their headers fails the build.
