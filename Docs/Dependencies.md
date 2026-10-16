# Dependency Handling

The engine has three external dependencies: spdlog, nlohmann/json and Catch2. **Adding a fourth needs
the project owner's approval** (`CLAUDE.md`). This file covers keeping the three current.

## Where they are pinned

`CMakeLists.txt` fetches each one with `FetchContent` at an exact `GIT_TAG`, deliberately without
`FIND_PACKAGE_ARGS`. A system or vcpkg copy at another version would otherwise satisfy the pin
silently. The comments around those declarations explain each consumption setting.

The **Dependency releases** workflow (`.github/workflows/dependency-releases.yml`) checks these
three pins and the strength lab's fastchess pin every Monday at 07:37 UTC. Dependabot
(`.github/dependabot.yml`) separately bumps workflow actions; it cannot read `FetchContent` pins.
To check for library releases by hand:

```bash
for r in gabime/spdlog nlohmann/json catchorg/Catch2; do
  gh release view -R $r --json tagName,publishedAt -q '"\(.tagName) \(.publishedAt)"' </dev/null
done
```

Read the release notes for every version you skip, not only the newest one.

## Automated release checks

The checker reads the three library pins from `CMakeLists.txt` and `FASTCHESS_VERSION` from
`.github/workflows/strength.yml`. It compares versions numerically across published GitHub
releases, ignoring drafts and library prereleases. fastchess's normal releases use `-alpha`
tags, so its established alpha channel is included along with final releases. Tags without a
release are not checked. This checks the committed fastchess pin, not the locally installed tool.

Newer releases create one **Dependency releases available** issue with `dependencies`,
`category:build-tooling`, `category:test` and `ready-for-human` labels. The lookup filters by
`dependencies` across open and closed issues, then checks a hidden body marker. Keep that label and
marker intact; the issue title and other labels may be changed. Later runs compare the dependency
rows, ignoring line endings, surrounding whitespace
and prose edits; they add no weekly comments or timestamps. When everything is current, no issue
is created, and an existing open report is updated
to say so. Close the issue when addressed or deferred. An unchanged closed report stays closed;
different findings with pending updates reopen the same issue. Put discussion in comments because
the generated body is replaced. Duplicate marked issues fail the check rather than creating more.

Every dependency lookup must succeed before the checker can write an issue. API errors, missing
pins and feeds without an eligible release fail the workflow, leaving the report alone. The run summary
contains the current report. Updates remain manual and follow the validation below; fastchess
updates follow [Measurements/README.md](../Measurements/README.md), including output-parser checks.

Run **Dependency releases → Run workflow** on `main`, or use:

```bash
gh workflow run dependency-releases.yml --ref main
```

For a read-only local preview, with Python 3 and authenticated `gh` on PATH:

```bash
python .github/scripts/check_dependency_releases.py --repo theEscape2207/StratChess
python .github/scripts/test_dependency_releases.py --self-test
```

The workflow uses only `contents: read` and `issues: write` from `GITHUB_TOKEN`; it needs no
additional secret or service. Publication runs only on this repository's `main` branch, including
manual dispatches. GitHub can delay schedules and disables scheduled workflows in public
repositories after 60 days without activity; re-enable it in Actions if that happens.

## Bumping a version

1. Build the current pin from the task branch before copying a baseline; an existing executable
   may be from another revision. For Catch2, use the capture commands below and record the fast
   tier's test-case and assertion counts.
2. Change the `GIT_TAG` in `CMakeLists.txt`. The first build afterwards checks out the new version
   in the shared `StratChessSupport/Deps` cache, which every worktree on the machine uses
   ([`Workflow.md` → Dependency cache](Workflow.md#dependency-cache)). Coordinate with any other
   local builds using that cache: different pins share the same source checkout. A new tag may
   need network even when the cache already exists.
3. Change the CI cache keys. The `actions/cache` keys for the dependencies spell every version
   literally, across `build-and-test.yml`, `nightly.yml` and `strength.yml`. Update both `cmake-deps-`
   and `win-deps-` keys. CMake normally updates a restored checkout to the requested tag, but
   caches are immutable: an unchanged key keeps restoring the old dependency tree. Find every
   occurrence with `rg -n 'catch2-v' .github/workflows` and check that the old tag is gone.
4. Run `build.ps1 all`.
5. Validate according to what the dependency reaches (next section).
6. Add a `Docs/Changelog.md` entry that names the versions and the evidence. This is a Build-tier
   PR. Use skill `open-pull-request`; `New-PullRequest.ps1` runs the Build-tier gates before
   pushing. Confirm the required `build-and-test-result` check is green before merge.

## What a bump must prove

**Catch2 links only `StratChessTests`.** Compare the baseline `StratChessEvolved.exe` byte for byte
with the one built afterwards. Release builds are reproducible, so the two must be identical. That
proves more than a search-equivalence or nps run could. Then run the fast tier and compare its
test-case and assertion counts. Investigate any difference: fewer test cases can mean lost
registration; assertion counts can also change with sections, generators or runtime-dependent
paths. Do not accept a green exit alone as evidence that the same tests ran.

**spdlog and nlohmann/json compile inside the engine's own translation units**, so a bump can change
the engine binary. Run `Compare-SearchEquivalence.ps1` and a bench pass (skill `measure-strength`),
and build on Linux/GCC: spdlog bundles `{fmt}`, and a `{fmt}` major-version jump is where the real
risk sits.

### Catch2 capture and verification

Run these PowerShell commands from the repository root, **before editing the pin**. Keep the
compiler, configuration, source revision and test filter the same on both sides. Stop if a build
or test command fails. `build/dependency-update/` is ignored and keeps the baseline in this
worktree.

```powershell
$repo = (Get-Location).Path
pwsh -ExecutionPolicy Bypass -File "$repo/build.ps1" all
if ($LASTEXITCODE -ne 0) { throw 'Baseline build failed' }
New-Item -ItemType Directory -Force build/dependency-update | Out-Null
Copy-Item build/windows-clang-cl/StratChessEvolved.exe build/dependency-update/engine-before.exe
pwsh -ExecutionPolicy Bypass -File "$repo/Scripts/Run-Tests.ps1" |
    Tee-Object build/dependency-update/tests-before.txt
if ($LASTEXITCODE -ne 0) { throw 'Baseline tests failed' }
```

After changing the pin and every dependency cache key, rebuild and verify the version actually
consumed. Read the source path from the build's cache rather than assuming a shared-cache location;
CI and direct preset builds use `build/_deps` instead. Replace `<new-tag>` with the intended tag.

```powershell
$expectedTag = '<new-tag>'
pwsh -ExecutionPolicy Bypass -File "$repo/build.ps1" all
if ($LASTEXITCODE -ne 0) { throw 'Updated build failed' }
$catchSource = ((Select-String -Path build/windows-clang-cl/CMakeCache.txt `
    -Pattern '^Catch2_SOURCE_DIR:STATIC=').Line -split '=', 2)[1]
$resolvedTag = git -C "$catchSource" describe --tags --exact-match
if ($LASTEXITCODE -ne 0 -or $resolvedTag -ne $expectedTag) { throw 'Unexpected Catch2 checkout' }
# The test runner's help must report the same version.
& ./build/windows-clang-cl/StratChessTests.exe --help
pwsh -ExecutionPolicy Bypass -File "$repo/Scripts/Run-Tests.ps1" |
    Tee-Object build/dependency-update/tests-after.txt
if ($LASTEXITCODE -ne 0) { throw 'Updated tests failed' }
$before = (Get-FileHash build/dependency-update/engine-before.exe -Algorithm SHA256).Hash
$after = (Get-FileHash build/windows-clang-cl/StratChessEvolved.exe -Algorithm SHA256).Hash
if ($before -ne $after) { throw 'Catch2-only update changed the engine binary' }
Select-String -Path build/dependency-update/tests-*.txt -Pattern 'All tests passed'
```

Record the resolved tag, test totals and identical engine hashes in the PR. A hash mismatch needs
investigation before claiming the engine is unchanged. Local Release checks cover unity batches
and warning handling; CI's Linux Debug, ASan/UBSan with libstdc++ debug mode, and Windows Release
and Debug builds cover the remaining compatibility settings. Nightly's extended and sanitizer
extended jobs provide the longer test runs after merge; they do not run on a pull request.

## Tripwires per dependency

Each consumption setting is explained in a comment at its place in `CMakeLists.txt`. These are the
ones a new version can break:

- **Catch2 unity batches.** A name collision between files in one unit is a compile error. Lower
  `UNITY_BUILD_BATCH_SIZE`.
- **Catch2 SYSTEM includes.** They are copied from the `Catch2` target. If a new version declares
  its include directories on another target, warnings in its headers break the `/WX` build.
- **Catch2 sanitizer and libstdc++-debug settings.** They are applied to `Catch2` and
  `Catch2WithMain` by name. The Linux sanitizer jobs show whether they still apply.
- **Catch2 reporter/listener callbacks.** `StratChessTests/StratChessTests.cpp` registers
  `QuietConsoleLogListener`. Check reporter API changes and deprecations; exceptions must not
  escape callbacks (reporter constructors may throw).
- **spdlog and nlohmann/json system-include options** (`SPDLOG_SYSTEM_INCLUDES`,
  `JSON_SystemInclude`). If an option is renamed, the old name does nothing without any message.
