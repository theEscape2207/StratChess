<#
.SYNOPSIS
    Build wrapper for StratChessEvolved.

.DESCRIPTION
    Drives the CMake presets in CMakePresets.json and exposes simple build verbs.
    Defaults to Release with clang-cl, which is the shipping configuration.

    Imports the Visual Studio developer environment itself, so this works from a
    plain PowerShell, a git hook or an agent session — clang-cl and cl need
    INCLUDE/LIB set, and nothing outside a Developer Prompt has them.

.PARAMETER Verb
    main            Build the engine executable
    tests           Build the test executable
    all             Build both (default)
    run-tests       Build tests then run fast tier only (excludes [slow])
    extended-tests  Build tests then run all tiers including [slow]

.PARAMETER Tag
    Optional Catch2 tag filter for run-tests, e.g. "[formatter]" or "[perft]".

.PARAMETER Config
    Build configuration: Release (default) or Debug.

.PARAMETER Compiler
    clang-cl (default, what ships) or msvc. MSVC is supported for development and
    debugging; do not measure with it, since nps and Elo are only comparable
    between binaries from the same compiler.

.PARAMETER SelfTest
    Run build-wrapper regression tests without configuring or building. Covers the
    processor-architecture fallback and the artifact-freshness verdict.

.NOTES
    After building, each artifact is checked against the newest source the build
    consumes. The target just built is fatal if still older; an artifact this verb did
    not rebuild is a warning naming the newer source and its consumers -- 'run-tests'
    builds only the test binary, so the tactical suite, UCI eval and Run-Bench would
    otherwise read a stale engine silently.

.EXAMPLE
    .\build.ps1
    .\build.ps1 tests
    .\build.ps1 run-tests
    .\build.ps1 run-tests "[formatter]"
    .\build.ps1 extended-tests
    .\build.ps1 all -Config Debug
    .\build.ps1 main -Compiler msvc
    .\build.ps1 -SelfTest
#>
param(
    [Parameter(Position=0)]
    [ValidateSet('main', 'tests', 'all', 'run-tests', 'extended-tests')]
    [string]$Verb = 'all',

    [Parameter(Position=1)]
    [string]$Tag = '',

    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',

    [ValidateSet('clang-cl', 'msvc')]
    [string]$Compiler = 'clang-cl',

    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = $PSScriptRoot

$Preset = "windows-$Compiler"
if ($Config -eq 'Debug') { $Preset += '-debug' }

$BuildDir = Join-Path $RepoRoot "build\$Preset"
$TestExe  = Join-Path $BuildDir 'StratChessTests.exe'

function Resolve-ProcessorArchitecture {
    param(
        [AllowEmptyString()][string]$CurrentArchitecture,
        [AllowEmptyString()][string]$VsTargetArchitecture
    )

    if (-not [string]::IsNullOrWhiteSpace($CurrentArchitecture)) {
        return $CurrentArchitecture
    }

    if ($VsTargetArchitecture -ieq 'x64') {
        return 'AMD64'
    }

    throw "PROCESSOR_ARCHITECTURE is missing and vcvars64 reported target '$VsTargetArchitecture'. A fresh CMake configure cannot identify x86-64."
}

# Was this artifact produced after the newest source it is built from?
#
# Pure, so the verdict is testable without a build (see Invoke-SelfTest). It compares
# timestamps rather than consulting ninja: `ninja -n` cannot answer this here, because
# CONFIGURE_DEPENDS leaves a pending glob re-check on every invocation and the dry run
# stops at "Re-running CMake" without ever evaluating the downstream edges. It reports
# a genuinely stale binary as having no work to do.
#
# $Sources takes objects with Path and WriteTime, so callers decide what counts as a
# source and the comparison stays free of filesystem access.
function Test-ArtifactFreshness {
    param(
        [object]$ArtifactWriteTime,
        [object[]]$Sources
    )

    if ($null -eq $ArtifactWriteTime) {
        return [pscustomobject]@{ Fresh = $false; Reason = 'missing'; NewestSourcePath = $null }
    }
    if (-not $Sources -or $Sources.Count -eq 0) {
        # Nothing to compare against. Reported rather than assumed: an empty source set
        # means the watched globs matched nothing, which is a bug in the caller, not a
        # clean bill of health.
        return [pscustomobject]@{ Fresh = $true; Reason = 'no-sources'; NewestSourcePath = $null }
    }

    # Strictly newer, so a tie counts as stale. That matches ninja, which rebuilds when a
    # source's mtime exactly equals the object it produces (measured, not assumed), and it
    # errs the cheap way: a false 'stale' costs one no-op rebuild, while a false 'fresh'
    # costs a wrong measurement, which is the whole reason this check exists. Ties are not
    # only a theoretical concern on filesystems with coarse timestamp granularity.
    $newest = $Sources | Sort-Object -Property WriteTime -Descending | Select-Object -First 1
    if ([DateTime]$ArtifactWriteTime -gt [DateTime]$newest.WriteTime) {
        return [pscustomobject]@{ Fresh = $true; Reason = 'fresh'; NewestSourcePath = $newest.Path }
    }
    return [pscustomobject]@{ Fresh = $false; Reason = 'stale'; NewestSourcePath = $newest.Path }
}

# Every file the named artifact is actually built from. Docs, Scripts and .claude are
# deliberately outside the set so editing a design document never reports a binary as
# stale, and StratEngine/Archived is excluded because CMake never builds it.
#
# The two artifacts get different sets, matching CMakeLists.txt: both compile
# StratEngine, but StratChessTests.exe adds StratChessTests/ and StratChessEvolved.exe
# adds StratChessEvolved/. One shared set instead reports the main binary as stale
# forever after a test-only file is added — a file it does not depend on and no rebuild
# can make it newer than.
function Get-BuildRelevantSources {
    param(
        [string]$Root,
        [ValidateSet('Main', 'Tests')][string]$Artifact
    )

    $roots = @(
        (Join-Path $Root 'StratEngine'),
        (Join-Path $Root ($Artifact -eq 'Tests' ? 'StratChessTests' : 'StratChessEvolved'))
    ) | Where-Object { Test-Path $_ }

    $sources = @()
    if ($roots) {
        $sources += Get-ChildItem -Path $roots -Recurse -File -Include '*.cpp', '*.h', '*.hpp' |
            Where-Object { $_.FullName -notmatch '\\StratEngine\\Archived\\' }
    }
    foreach ($name in @('CMakeLists.txt', 'CMakePresets.json')) {
        $path = Join-Path $Root $name
        if (Test-Path $path) { $sources += Get-Item $path }
    }

    return $sources | ForEach-Object { [pscustomobject]@{ Path = $_.FullName; WriteTime = $_.LastWriteTime } }
}

# Reports on one artifact. $Fatal for the target that was just built -- a stale
# artifact there means the build did not produce what it claimed, which must not pass
# silently. A warning for artifacts a verb deliberately did not rebuild.
function Assert-ArtifactFresh {
    param(
        [string]$ArtifactPath,
        [object[]]$Sources,
        [string]$Consumers = '',
        [switch]$Fatal
    )

    $writeTime = (Test-Path $ArtifactPath) ? (Get-Item $ArtifactPath).LastWriteTime : $null
    $verdict = Test-ArtifactFreshness -ArtifactWriteTime $writeTime -Sources $Sources

    if ($verdict.Reason -eq 'no-sources') {
        Write-Warning "Freshness check found no sources to compare against - the watched globs matched nothing."
        return
    }
    if ($verdict.Fresh) { return }
    if ($verdict.Reason -eq 'missing' -and -not $Fatal) { return }

    $name = Split-Path $ArtifactPath -Leaf
    $newer = Split-Path $verdict.NewestSourcePath -Leaf
    if ($Fatal) {
        Write-Error "$name is older than $newer even after building it. The build reported success without producing a current binary."
        exit 1
    }

    Write-Host ""
    Write-Warning "$name is STALE - $newer is newer, and this verb did not rebuild it."
    if ($Consumers) {
        Write-Host "         $Consumers will read the old binary." -ForegroundColor Yellow
    }
    Write-Host "         Rebuild it with: .\build.ps1 all" -ForegroundColor Yellow
}

function Invoke-SelfTest {
    $cases = @(
        @{ Name = 'existing architecture is preserved'; Current = 'AMD64'; Target = 'x64'; Expected = 'AMD64' }
        @{ Name = 'missing architecture uses VS x64 target'; Current = ''; Target = 'x64'; Expected = 'AMD64' }
        @{ Name = 'missing architecture without VS target fails'; Current = ''; Target = ''; ExpectError = $true }
        @{ Name = 'missing architecture with unsupported target fails'; Current = ''; Target = 'arm64'; ExpectError = $true }
    )
    # Freshness verdicts. The stale case is the falsification: it must fail while
    # naming the file that made it stale, or the check would be untrustworthy in
    # exactly the situation it exists for.
    $t0 = [DateTime]'2026-01-01T12:00:00'
    $freshnessCases = @(
        @{ Name = 'artifact newer than every source is fresh'
           Artifact = $t0.AddSeconds(1)
           Sources  = @([pscustomobject]@{ Path = 'a.cpp'; WriteTime = $t0 })
           Fresh    = $true }
        @{ Name = 'artifact older than a source is stale and names it'
           Artifact = $t0
           Sources  = @([pscustomobject]@{ Path = 'old.cpp'; WriteTime = $t0.AddSeconds(-10) },
                        [pscustomobject]@{ Path = 'edited.h'; WriteTime = $t0.AddSeconds(1) })
           Fresh    = $false
           Newest   = 'edited.h' }
        @{ Name = 'artifact exactly as old as its newest source is stale'
           Artifact = $t0
           Sources  = @([pscustomobject]@{ Path = 'a.cpp'; WriteTime = $t0 })
           Fresh    = $false
           Reason   = 'stale' }
        @{ Name = 'missing artifact is not fresh'
           Artifact = $null
           Sources  = @([pscustomobject]@{ Path = 'a.cpp'; WriteTime = $t0 })
           Fresh    = $false
           Reason   = 'missing' }
        @{ Name = 'empty source set is reported, not assumed clean'
           Artifact = $t0
           Sources  = @()
           Fresh    = $true
           Reason   = 'no-sources' }
    )

    # Source-set partitioning, against the real tree. Falsifies the bug the split exists
    # for: with one shared set, a test-only file is "newer than" the main binary that no
    # rebuild of it can ever answer, so build.ps1 fails fatally from then on.
    # Compared as full paths under $RepoRoot, never as a substring of one: a worktree of
    # this repository lives under a directory called StratChessEvolved itself, so every
    # source path contains that name.
    $partitionCases = @(
        @{ Name = "the main binary's sources exclude StratChessTests"
           Artifact = 'Main'; Excluded = 'StratChessTests'; Included = 'StratEngine' }
        @{ Name = "the test binary's sources exclude StratChessEvolved"
           Artifact = 'Tests'; Excluded = 'StratChessEvolved'; Included = 'StratEngine' }
    )

    $failures = 0
    foreach ($case in $partitionCases) {
        $set = Get-BuildRelevantSources -Root $RepoRoot -Artifact $case.Artifact
        $excludedDir = (Join-Path $RepoRoot $case.Excluded) + [IO.Path]::DirectorySeparatorChar
        $includedDir = (Join-Path $RepoRoot $case.Included) + [IO.Path]::DirectorySeparatorChar
        $leaked = @($set | Where-Object { $_.Path.StartsWith($excludedDir, [StringComparison]::OrdinalIgnoreCase) })
        $covered = @($set | Where-Object { $_.Path.StartsWith($includedDir, [StringComparison]::OrdinalIgnoreCase) })
        $passed = ($leaked.Count -eq 0) -and ($covered.Count -gt 0)
        $detail = "$($leaked.Count) file(s) under '$excludedDir', $($covered.Count) under '$includedDir'"

        if ($passed) {
            Write-Host "PASS: $($case.Name)" -ForegroundColor Green
        } else {
            Write-Host "FAIL: $($case.Name) ($detail)" -ForegroundColor Red
            $failures++
        }
    }

    foreach ($case in $freshnessCases) {
        $verdict = Test-ArtifactFreshness -ArtifactWriteTime $case.Artifact -Sources $case.Sources
        $passed = $verdict.Fresh -eq $case.Fresh
        $detail = "expected Fresh=$($case.Fresh), got $($verdict.Fresh)"
        if ($passed -and $case.ContainsKey('Reason')) {
            $passed = $verdict.Reason -eq $case.Reason
            $detail = "expected Reason='$($case.Reason)', got '$($verdict.Reason)'"
        }
        if ($passed -and $case.ContainsKey('Newest')) {
            $passed = $verdict.NewestSourcePath -eq $case.Newest
            $detail = "expected NewestSourcePath='$($case.Newest)', got '$($verdict.NewestSourcePath)'"
        }

        if ($passed) {
            Write-Host "PASS: $($case.Name)" -ForegroundColor Green
        } else {
            Write-Host "FAIL: $($case.Name) ($detail)" -ForegroundColor Red
            $failures++
        }
    }

    foreach ($case in $cases) {
        $expectsError = $case.ContainsKey('ExpectError')
        $passed = $false
        $detail = 'expected an exception'
        try {
            $actual = Resolve-ProcessorArchitecture -CurrentArchitecture $case.Current -VsTargetArchitecture $case.Target
            if (-not $expectsError) {
                $passed = $actual -eq $case.Expected
                $detail = "expected '$($case.Expected)', got '$actual'"
            }
        } catch {
            $passed = $expectsError
            $detail = $_.Exception.Message
        }

        if ($passed) {
            Write-Host "PASS: $($case.Name)" -ForegroundColor Green
        } else {
            Write-Host "FAIL: $($case.Name) ($detail)" -ForegroundColor Red
            $failures++
        }
    }

    $total = $cases.Count + $freshnessCases.Count + $partitionCases.Count
    if ($failures) { Write-Host "$failures self-test case(s) FAILED." -ForegroundColor Red }
    else { Write-Host "$total self-test cases passed." -ForegroundColor Green }
    return $failures -eq 0
}

if ($SelfTest) {
    if (Invoke-SelfTest) { exit 0 }
    exit 1
}

# ---------------------------------------------------------------------------
# One-time bootstrap: point this checkout's hooks at the tracked .githooks/
# directory so new clones/worktrees get FEN + fast-test enforcement without
# a manual setup step. Idempotent — only writes when the value differs.
# ---------------------------------------------------------------------------
$currentHooksPath = git config --get core.hooksPath 2>$null
if ($currentHooksPath -ne '.githooks') {
    git config core.hooksPath .githooks
    Write-Host "Configured core.hooksPath = .githooks" -ForegroundColor DarkGray
}

# GitHub honours .git-blame-ignore-revs automatically; local `git blame` does not
# unless blame.ignoreRevsFile is set, so the reformat commit stays hidden from
# blame here too.
$currentBlameIgnoreRevs = git config --get blame.ignoreRevsFile 2>$null
if ($currentBlameIgnoreRevs -ne '.git-blame-ignore-revs') {
    git config blame.ignoreRevsFile .git-blame-ignore-revs
    Write-Host "Configured blame.ignoreRevsFile = .git-blame-ignore-revs" -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# Developer environment
#
# vcvars64.bat is the only supported way to get the compiler's INCLUDE/LIB and
# the Windows SDK onto the environment. It also puts cmake, ninja, clang-cl,
# lld-link and llvm-rc on PATH, so nothing below needs an absolute tool path.
# ---------------------------------------------------------------------------
function Import-VsDevEnvironment {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        Write-Error "vswhere.exe not found at: $vswhere`nIs Visual Studio installed?"
        exit 1
    }

    $vsRoot = (& $vswhere -latest -property installationPath) | Select-Object -First 1
    if (-not $vsRoot) {
        Write-Error "No Visual Studio installation found via vswhere."
        exit 1
    }

    $vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) {
        Write-Error "vcvars64.bat not found at: $vcvars`nIs the C++ workload installed?"
        exit 1
    }

    cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }

    # Some agent shells omit this standard Windows variable, and vcvars64 does
    # not restore it. Fresh CMake trees need it for CMAKE_SYSTEM_PROCESSOR.
    $env:PROCESSOR_ARCHITECTURE = Resolve-ProcessorArchitecture `
        -CurrentArchitecture $env:PROCESSOR_ARCHITECTURE `
        -VsTargetArchitecture $env:VSCMD_ARG_TGT_ARCH

    foreach ($tool in @('cmake', 'ninja')) {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
            Write-Error "$tool not on PATH after vcvars64. Install the 'C++ CMake tools for Windows' VS component."
            exit 1
        }
    }

    # Check the compiler by name here rather than letting CMake fail later: a
    # missing clang-cl otherwise surfaces as a configure error about a broken
    # compiler, which does not say which VS component to install.
    $compilerExe = if ($Compiler -eq 'msvc') { 'cl' } else { 'clang-cl' }
    if (-not (Get-Command $compilerExe -ErrorAction SilentlyContinue)) {
        $hint = if ($Compiler -eq 'msvc') {
            "Install the MSVC v14x build tools VS component."
        } else {
            "Install the 'C++ Clang tools for Windows' VS component, or build with -Compiler msvc."
        }
        Write-Error "$compilerExe not on PATH after vcvars64. $hint"
        exit 1
    }
}

# FetchContent clones spdlog, nlohmann/json and Catch2 on a build tree's first
# configure, so without this every new worktree re-clones all three over the
# network. Point them at one cache beside the main checkout instead: only the
# first worktree on a machine pays for the clone, and the rest need no network.
#
# Overridden via -D rather than in CMakePresets.json because a preset's
# cacheVariables cannot be redefined by CMakeUserPresets.json (duplicate preset
# names are an error), and inheriting under a new name would also change
# binaryDir, which Get-BuildArtifact.ps1 depends on.
#
# Skipped in CI: the workflow caches build/_deps by that exact path, and a
# runner is discarded after every job so there is nothing to share.
function Get-SharedDepsCache {
    if ($env:GITHUB_ACTIONS -eq 'true') { return $null }

    $commonDir = & git -C $RepoRoot rev-parse --path-format=absolute --git-common-dir 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $commonDir) { return $null }

    $mainCheckout = $commonDir -replace '[\\/]\.git[\\/]?$', ''
    return (Join-Path (Split-Path $mainCheckout -Parent) 'StratChessDeps') -replace '\\', '/'
}

function Invoke-CMakeBuild {
    param([string[]]$Targets)

    # Configure only when there is no cache: Ninja re-runs CMake by itself when
    # CMakeLists.txt or CMakePresets.json change, so configuring every time only
    # adds latency.
    if (-not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) {
        Write-Host "`n==> Configuring $Preset" -ForegroundColor Cyan
        # --log-level=WARNING drops the ~20 lines of compiler-ABI/pthread probing
        # CMake emits on every reconfigure; warnings and errors still surface.
        $configureArgs = @('--preset', $Preset, '--log-level=WARNING')
        $depsCache = Get-SharedDepsCache
        if ($depsCache) { $configureArgs += @('-D', "FETCHCONTENT_BASE_DIR=$depsCache") }

        & cmake @configureArgs
        if ($LASTEXITCODE -ne 0) {
            Write-Error "Configure failed (exit $LASTEXITCODE): $Preset"
            exit $LASTEXITCODE
        }
    }

    $label = if ($Targets) { $Targets -join ', ' } else { 'all targets' }
    Write-Host "`n==> Building $label ($Preset)" -ForegroundColor Cyan

    $buildArgs = @('--build', '--preset', $Preset)
    foreach ($t in $Targets) { $buildArgs += @('--target', $t) }

    # Ninja's \r-based progress bar becomes runs of blank lines once captured
    # non-interactively; dropping empty lines keeps real progress and errors
    # visible without the padding. Native command output still streams through
    # the pipeline, and $LASTEXITCODE still reflects cmake's own exit code.
    & cmake @buildArgs | Where-Object { $_ -ne '' }
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Build failed (exit $LASTEXITCODE): $label"
        exit $LASTEXITCODE
    }
}

Import-VsDevEnvironment
Write-Host "Preset: $Preset | Config: $Config | Compiler: $Compiler" -ForegroundColor DarkGray

# ---------------------------------------------------------------------------
# Verbs
#
# Ninja builds every requested target in one invocation and parallelises across
# them, so 'all' needs no job orchestration of its own.
# ---------------------------------------------------------------------------
$MainExe = Join-Path $BuildDir 'StratChessEvolved.exe'

# Which artifacts a verb leaves untouched, and who then reads them. A stale binary has
# produced wrong measurements more than once: the verb builds one target and the next
# step measures the other, and nothing says so.
$engineConsumers = 'The tactical suite, UCI eval and Run-Bench'

switch ($Verb) {
    'main' {
        Invoke-CMakeBuild -Targets @('StratChessEvolved')
        Assert-ArtifactFresh -ArtifactPath $MainExe -Sources (Get-BuildRelevantSources -Root $RepoRoot -Artifact Main) -Fatal
        Assert-ArtifactFresh -ArtifactPath $TestExe -Sources (Get-BuildRelevantSources -Root $RepoRoot -Artifact Tests) -Consumers 'Run-Tests.ps1 and Validate-PreCommit.ps1'
    }
    'tests' {
        Invoke-CMakeBuild -Targets @('StratChessTests')
        Assert-ArtifactFresh -ArtifactPath $TestExe -Sources (Get-BuildRelevantSources -Root $RepoRoot -Artifact Tests) -Fatal
        Assert-ArtifactFresh -ArtifactPath $MainExe -Sources (Get-BuildRelevantSources -Root $RepoRoot -Artifact Main) -Consumers $engineConsumers
    }
    'all' {
        Invoke-CMakeBuild -Targets @()
        Assert-ArtifactFresh -ArtifactPath $MainExe -Sources (Get-BuildRelevantSources -Root $RepoRoot -Artifact Main) -Fatal
        Assert-ArtifactFresh -ArtifactPath $TestExe -Sources (Get-BuildRelevantSources -Root $RepoRoot -Artifact Tests) -Fatal
    }
    'run-tests' {
        Invoke-CMakeBuild -Targets @('StratChessTests')
        Assert-ArtifactFresh -ArtifactPath $TestExe -Sources (Get-BuildRelevantSources -Root $RepoRoot -Artifact Tests) -Fatal
        Assert-ArtifactFresh -ArtifactPath $MainExe -Sources (Get-BuildRelevantSources -Root $RepoRoot -Artifact Main) -Consumers $engineConsumers
        Write-Host ""
        # Default: exclude [slow] tests; pass an explicit tag to override.
        $effectiveTag = $Tag ? $Tag : '~[slow]'
        Write-Host "==> Running tests ($effectiveTag)" -ForegroundColor Cyan
        & $TestExe $effectiveTag
        exit $LASTEXITCODE
    }
    'extended-tests' {
        Invoke-CMakeBuild -Targets @('StratChessTests')
        Assert-ArtifactFresh -ArtifactPath $TestExe -Sources (Get-BuildRelevantSources -Root $RepoRoot -Artifact Tests) -Fatal
        Assert-ArtifactFresh -ArtifactPath $MainExe -Sources (Get-BuildRelevantSources -Root $RepoRoot -Artifact Main) -Consumers $engineConsumers
        Write-Host ""
        if ($Tag) {
            Write-Warning "extended-tests ignores the Tag parameter ('$Tag'). Use run-tests to filter by tag."
        }
        Write-Host "==> Running all tests including [slow]" -ForegroundColor Cyan
        & $TestExe
        exit $LASTEXITCODE
    }
}
