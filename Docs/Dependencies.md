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

1. Copy `build/windows-clang-cl/StratChessEvolved.exe` aside as the baseline, and note the fast
   tier's test-case and assertion counts.
2. Change the `GIT_TAG` in `CMakeLists.txt`. The first build afterwards checks out the new version
   in the shared `StratChessSupport/Deps` cache, which every worktree on the machine uses
   ([`Workflow.md` → Dependency cache](Workflow.md#dependency-cache)).
3. Change the CI cache keys. The `actions/cache` keys for the dependencies spell every version
   literally, across `build-and-test.yml`, `nightly.yml` and `strength.yml`. A key you miss keeps
   restoring the old sources, and CI passes against a version the branch no longer pins. Find them
   with `grep -rn "catch2-v" .github/workflows`.
4. Run `build.ps1 all`.
5. Validate according to what the dependency reaches (next section).
6. Add a `Docs/Changelog.md` entry that names the versions and the evidence. This is a Build-tier
   PR.

## What a bump must prove

**Catch2 links only `StratChessTests`.** Compare the baseline `StratChessEvolved.exe` byte for byte
with the one built afterwards. Release builds are reproducible, so the two must be identical. That
proves more than a search-equivalence or nps run could. Then run the fast tier and confirm that the test-case and assertion counts have not changed. A drop in either means
tests stopped registering.

**spdlog and nlohmann/json compile inside the engine's own translation units**, so a bump can change
the engine binary. Run `Compare-SearchEquivalence.ps1` and a bench pass (skill `measure-strength`),
and build on Linux/GCC: spdlog bundles `{fmt}`, and a `{fmt}` major-version jump is where the real
risk sits.

## Tripwires per dependency

Each consumption setting is explained in a comment at its place in `CMakeLists.txt`. These are the
ones a new version can break:

- **Catch2 unity batches.** A name collision between files in one unit is a compile error. Lower
  `UNITY_BUILD_BATCH_SIZE`.
- **Catch2 SYSTEM includes.** They are copied from the `Catch2` target. If a new version declares
  its include directories on another target, warnings in its headers break the `/WX` build.
- **Catch2 sanitizer and libstdc++-debug settings.** They are applied to `Catch2` and
  `Catch2WithMain` by name. The Linux sanitizer jobs show whether they still apply.
- **spdlog and nlohmann/json system-include options** (`SPDLOG_SYSTEM_INCLUDES`,
  `JSON_SystemInclude`). If an option is renamed, the old name does nothing without any message.
