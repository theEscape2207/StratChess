<#
.SYNOPSIS
    Build the tree twice and assert the project's objects and executables come out
    byte-identical.

.DESCRIPTION
    #381 established a byte-identical cold-vs-warm comparison for Debug and left it a
    procedure; nothing in Scripts/ implemented it, and no equivalent existed for
    Release at all. This is that comparison, for the configuration that ships.

    Release could not inherit Debug's basis. /Brepro has two halves, and on the engine
    target only one of them does anything: under ThinLTO the compiler writes LLVM
    bitcode with a content hash rather than a COFF object, so there is no
    TimeDateStamp for the compile-side /Brepro to zero, and identity there rests on
    frontend determinism instead. The linker-side /Brepro still settles the PE header.
    The compile flag must not be removed on that finding: the ~89 non-LTO compile
    edges (the test target, the dependencies) do emit real COFF and do need it.

    Measured on 2026-09-19 over both modes: 105 of 108 artifacts byte-identical. The
    three that differ are CMake's own configure probes -- CompilerIdCXX.exe,
    ShowIncludes/main.obj and the _CMakeLTOTest binaries -- none of which go through
    strat_configure_target, so none ever sees /Brepro. They are out of scope below,
    named rather than pattern-excluded so that a new unreproducible artifact is a
    failure rather than a silent exemption.

    Scope is this project's two targets: the objects under CMakeFiles/<target>.dir/
    and both executables. Dependency artifacts are excluded deliberately -- Catch2
    builds a real static library, it does not go through strat_configure_target and so
    never receives /Brepro, and /Brepro does not strip a TimeDateStamp from a .lib
    header anyway. It is also built outside the preset's build directory and is not
    rebuilt by an ordinary edit. Gating on it would mean fixing a dependency's build
    for no benefit to what ships.

.PARAMETER Mode
    Determinism -- two cold builds with the compiler cache disabled. Asserts the
                   toolchain itself is deterministic. This is the mode that answers
                   "is the Release image reproducible".
    Cache       -- three builds: an uncached reference, a cold build that populates a
                   private cache, and a third served from it. The reference is what
                   makes the mode mean anything. Comparing the cold cached build with
                   the warm one compares a cache entry against the copy it was made
                   from, which holds however wrong the cache is; against an uncached
                   reference the comparison asserts the property Docs/CI.md's ccache
                   gate rests on -- that building through the cache ships what the
                   compiler alone produces. Skipped when no cache is installed.

.PARAMETER Config
    Build configuration. Defaults to Release: the configuration that ships, and the
    one that had no basis.

.PARAMETER SelfTest
    Run synthetic scope and comparison tests and exit. Pure: no build, no toolchain.

.HOW TO INVOKE
    pwsh -File Scripts/Test-ReleaseReproducibility.ps1
    pwsh -File Scripts/Test-ReleaseReproducibility.ps1 -Mode Cache
    pwsh -File Scripts/Test-ReleaseReproducibility.ps1 -SelfTest

    Two full builds, three in -Mode Cache, so budget minutes rather than seconds. Run
    it when a change touches the build configuration or the toolchain; nothing runs it
    automatically.
#>

[CmdletBinding(DefaultParameterSetName = 'Run')]
param(
    [Parameter(ParameterSetName = 'Run')]
    [ValidateSet('Determinism', 'Cache')]
    [string]$Mode = 'Determinism',

    [Parameter(ParameterSetName = 'Run')]
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',

    [Parameter(Mandatory, ParameterSetName = 'SelfTest')]
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Everything this project builds itself, and nothing else. Directory prefixes rather
# than a wildcard over the tree: CMake's configure probes live beside these and are
# reproducible in neither configuration.
$script:ScopedObjectPrefix = @(
    'CMakeFiles/StratChessEvolved.dir/'
    'CMakeFiles/StratChessTests.dir/'
)
$script:ScopedFile = @(
    'StratChessEvolved.exe'
    'StratChessTests.exe'
)

function Select-ScopedArtifact {
    <#
      .SYNOPSIS
        The subset of build-relative paths this gate asserts over.
    #>
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Path
    )

    $kept = [System.Collections.Generic.List[string]]::new()
    foreach ($candidate in $Path) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        $normalised = $candidate -replace '\\', '/'

        if ($script:ScopedFile -contains $normalised) {
            $kept.Add($normalised)
            continue
        }
        foreach ($prefix in $script:ScopedObjectPrefix) {
            if ($normalised.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                $kept.Add($normalised)
                break
            }
        }
    }

    return $kept
}

function Compare-ArtifactHash {
    <#
      .SYNOPSIS
        Verdict over two path->hash maps: what differs, what only one build produced.
      .DESCRIPTION
        An artifact present in one build and absent from the other is a difference,
        not a skip. A comparison that silently intersected the two sets would report
        a clean run for a build that stopped emitting something.
    #>
    param(
        [Parameter(Mandatory)][hashtable]$First,
        [Parameter(Mandatory)][hashtable]$Second
    )

    $differing = [System.Collections.Generic.List[string]]::new()
    $missing = [System.Collections.Generic.List[string]]::new()

    foreach ($key in $First.Keys) {
        if (-not $Second.ContainsKey($key)) { $missing.Add("$key (absent from the second build)"); continue }
        if ($First[$key] -ne $Second[$key]) { $differing.Add($key) }
    }
    foreach ($key in $Second.Keys) {
        if (-not $First.ContainsKey($key)) { $missing.Add("$key (absent from the first build)") }
    }

    $compared = $First.Count
    # An empty comparison is a failure. It is what a renamed target, a wrong build
    # directory or a scope filter that stopped matching all look like, and each of
    # those would otherwise report success.
    $ok = ($compared -gt 0) -and ($differing.Count -eq 0) -and ($missing.Count -eq 0)

    return [pscustomobject]@{
        Compared  = $compared
        Differing = @($differing)
        Missing   = @($missing)
        Ok        = [bool]$ok
    }
}

function Get-ArtifactHash {
    <#
      .SYNOPSIS
        Build-relative path -> SHA-256, over the scoped artifacts of one build tree.
    #>
    param(
        [Parameter(Mandatory)][string]$BuildDir
    )

    $table = @{}
    $files = @(Get-ChildItem -LiteralPath $BuildDir -Recurse -File |
        Where-Object { $_.Extension -in '.obj', '.exe' })

    foreach ($file in $files) {
        $relative = [System.IO.Path]::GetRelativePath($BuildDir, $file.FullName) -replace '\\', '/'
        if (@(Select-ScopedArtifact -Path @($relative)).Count -eq 0) { continue }
        $table[$relative] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }

    return $table
}

function Get-CacheHitCount {
    <#
      .SYNOPSIS
        Cache hits recorded in a `ccache --print-stats` dump (tab-separated key/value).
      .DESCRIPTION
        -Mode Cache is worthless unless the second build actually read the cache: a
        launcher that silently did not engage turns it into a slower rerun of
        -Mode Determinism that still reports PASS. Counting the hits is what tells
        the two apart.
    #>
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][AllowEmptyString()][string[]]$StatsLine
    )

    $hits = 0
    foreach ($line in $StatsLine) {
        $match = [regex]::Match($line, '^(?<key>direct_cache_hit|preprocessed_cache_hit)\s+(?<value>\d+)\s*$')
        if ($match.Success) { $hits += [int]$match.Groups['value'].Value }
    }

    return $hits
}

function Get-BuildPlan {
    <#
      .SYNOPSIS
        The ordered builds a mode runs, and which two of them are compared.
      .DESCRIPTION
        A function rather than two inline labels so the shape of each mode is
        assertable without a toolchain. The one property worth protecting is Cache
        mode's reference build: with the cache enabled for it, the comparison holds
        whatever the cache returns, because the warm build is served the bytes the
        cold one stored.
    #>
    param(
        [Parameter(Mandatory)][ValidateSet('Determinism', 'Cache')][string]$Mode
    )

    if ($Mode -eq 'Cache') {
        return @(
            [pscustomobject]@{ Label = 'Build 1 of 3 (compiler cache disabled, the reference)'; CacheEnabled = $false; Compare = $true }
            [pscustomobject]@{ Label = 'Build 2 of 3 (cold cache, populating it)'; CacheEnabled = $true; Compare = $false }
            [pscustomobject]@{ Label = 'Build 3 of 3 (warm cache, served from it)'; CacheEnabled = $true; Compare = $true }
        )
    }

    return @(
        [pscustomobject]@{ Label = 'Build 1 of 2 (compiler cache disabled)'; CacheEnabled = $false; Compare = $true }
        [pscustomobject]@{ Label = 'Build 2 of 2 (compiler cache disabled)'; CacheEnabled = $false; Compare = $true }
    )
}

function Invoke-SelfTest {
    $script:selfTestFailures = 0
    Write-Host '==> Self-test' -ForegroundColor Cyan

    function Assert-Equal {
        param(
            [Parameter(Mandatory)][string]$Name,
            [Parameter(Mandatory)][AllowEmptyString()][string]$Expected,
            [Parameter(Mandatory)][AllowEmptyString()][string]$Actual
        )

        if ($Expected -eq $Actual) {
            Write-Host "  PASS  $Name" -ForegroundColor Green
        }
        else {
            Write-Host "  FAIL  $Name (expected '$Expected', got '$Actual')" -ForegroundColor Red
            $script:selfTestFailures++
        }
    }

    # --- scope ---------------------------------------------------------------
    $candidates = @(
        'CMakeFiles/StratChessEvolved.dir/StratEngine/AIPerplex.cpp.obj'
        'CMakeFiles/StratChessTests.dir/StratChessTests/SearchServiceTests.cpp.obj'
        'StratChessEvolved.exe'
        'StratChessTests.exe'
        # The three CMake probes the 2026-09-19 measurement found irreproducible.
        'CMakeFiles/4.3.1-msvc1/CompilerIdCXX/CMakeCXXCompilerId.exe'
        'CMakeFiles/ShowIncludes/main.obj'
        'CMakeFiles/_CMakeLTOTest-CXX/bin/boo.exe'
        # A dependency artifact, deliberately out of scope.
        '_deps/catch2-build/src/Catch2.lib'
    )
    $scoped = @(Select-ScopedArtifact -Path $candidates)
    Assert-Equal -Name 'scope keeps both targets and both executables, nothing else' `
        -Expected '4' -Actual "$($scoped.Count)"
    Assert-Equal -Name 'scope excludes the CMake configure probes' `
        -Expected 'False' -Actual "$($scoped -contains 'CMakeFiles/ShowIncludes/main.obj')"

    $backslashed = @(Select-ScopedArtifact -Path @('CMakeFiles\StratChessEvolved.dir\StratEngine\Eval.cpp.obj'))
    Assert-Equal -Name 'a Windows-separator path is in scope too' -Expected '1' -Actual "$($backslashed.Count)"

    Assert-Equal -Name 'an empty path list yields an empty scope' `
        -Expected '0' -Actual "$(@(Select-ScopedArtifact -Path @()).Count)"

    # --- comparison ----------------------------------------------------------
    $base = @{ 'StratChessEvolved.exe' = 'AAA'; 'CMakeFiles/StratChessEvolved.dir/a.obj' = 'BBB' }

    $same = Compare-ArtifactHash -First $base -Second @{ 'StratChessEvolved.exe' = 'AAA'; 'CMakeFiles/StratChessEvolved.dir/a.obj' = 'BBB' }
    Assert-Equal -Name 'two identical builds pass' -Expected 'True' -Actual "$($same.Ok)"
    Assert-Equal -Name 'the compared count is reported' -Expected '2' -Actual "$($same.Compared)"

    # The situation the gate exists for.
    $changed = Compare-ArtifactHash -First $base -Second @{ 'StratChessEvolved.exe' = 'ZZZ'; 'CMakeFiles/StratChessEvolved.dir/a.obj' = 'BBB' }
    Assert-Equal -Name 'FALSIFY: one differing hash fails' -Expected 'False' -Actual "$($changed.Ok)"
    Assert-Equal -Name 'FALSIFY: the differing artifact is named' `
        -Expected 'StratChessEvolved.exe' -Actual "$($changed.Differing -join ',')"

    $dropped = Compare-ArtifactHash -First $base -Second @{ 'StratChessEvolved.exe' = 'AAA' }
    Assert-Equal -Name 'FALSIFY: an artifact the second build did not produce fails' `
        -Expected 'False' -Actual "$($dropped.Ok)"

    $added = Compare-ArtifactHash -First @{ 'StratChessEvolved.exe' = 'AAA' } -Second $base
    Assert-Equal -Name 'FALSIFY: an artifact only the second build produced fails' `
        -Expected 'False' -Actual "$($added.Ok)"

    $empty = Compare-ArtifactHash -First @{} -Second @{}
    Assert-Equal -Name 'FALSIFY: comparing nothing fails, never passes vacuously' `
        -Expected 'False' -Actual "$($empty.Ok)"

    # --- cache warmth --------------------------------------------------------
    $warmStats = @(
        'cache_miss	3'
        'direct_cache_hit	97'
        'preprocessed_cache_hit	5'
        'remote_storage_hit	0'
    )
    Assert-Equal -Name 'both hit kinds count towards warmth' `
        -Expected '102' -Actual "$(Get-CacheHitCount -StatsLine $warmStats)"

    # The situation the warmth check exists for: a cache that was never consulted.
    $coldStats = @('cache_miss	114', 'direct_cache_hit	0', 'preprocessed_cache_hit	0')
    Assert-Equal -Name 'FALSIFY: a cache that served nothing reports zero hits' `
        -Expected '0' -Actual "$(Get-CacheHitCount -StatsLine $coldStats)"

    Assert-Equal -Name 'a stats dump with no hit fields reports zero hits' `
        -Expected '0' -Actual "$(Get-CacheHitCount -StatsLine @('cleanups_performed	0', ''))"

    # --- build plan ----------------------------------------------------------
    $determinism = @(Get-BuildPlan -Mode Determinism)
    Assert-Equal -Name 'Determinism runs two builds' -Expected '2' -Actual "$($determinism.Count)"
    Assert-Equal -Name 'Determinism disables the cache for both' `
        -Expected 'False,False' -Actual "$(($determinism | ForEach-Object { $_.CacheEnabled }) -join ',')"
    Assert-Equal -Name 'Determinism compares both builds' `
        -Expected '2' -Actual "$(@($determinism | Where-Object { $_.Compare }).Count)"

    $cache = @(Get-BuildPlan -Mode Cache)
    Assert-Equal -Name 'Cache runs three builds' -Expected '3' -Actual "$($cache.Count)"
    # The finding this mode was rebuilt around: a cached reference compares a cache
    # entry with the copy it was made from, which passes whatever the cache returns.
    Assert-Equal -Name 'FALSIFY: Cache mode reference build is uncached' `
        -Expected 'False' -Actual "$($cache[0].CacheEnabled)"
    Assert-Equal -Name 'Cache mode populates, then serves, from the cache' `
        -Expected 'True,True' -Actual "$(($cache[1..2] | ForEach-Object { $_.CacheEnabled }) -join ',')"
    Assert-Equal -Name 'Cache mode compares the uncached reference against the warm build' `
        -Expected 'True,False,True' -Actual "$(($cache | ForEach-Object { $_.Compare }) -join ',')"
    Assert-Equal -Name 'every mode compares exactly two builds' `
        -Expected '2' -Actual "$(@($cache | Where-Object { $_.Compare }).Count)"

    $failures = $script:selfTestFailures
    if ($failures -gt 0) {
        Write-Host "$failures self-test case(s) FAILED." -ForegroundColor Red
        return $false
    }
    Write-Host 'Self-test PASSED.' -ForegroundColor Green
    return $true
}

if ($SelfTest) {
    if (Invoke-SelfTest) { exit 0 }
    exit 1
}

$repoRoot = Split-Path $PSScriptRoot -Parent
$buildScript = Join-Path $repoRoot 'build.ps1'
$preset = if ($Config -eq 'Debug') { 'windows-clang-cl-debug' } else { 'windows-clang-cl' }
$buildDir = Join-Path $repoRoot "build\$preset"

# The cache is private to this run in both modes: Cache needs a genuinely cold one to
# start from, and Determinism must not be served by an existing one.
$cacheDir = Join-Path ([System.IO.Path]::GetTempPath()) "strat-repro-cache-$([guid]::NewGuid().ToString('N'))"

$useCache = $false
if ($Mode -eq 'Cache') {
    if (-not (Get-Command ccache -CommandType Application -ErrorAction SilentlyContinue)) {
        Write-Host 'SKIP: -Mode Cache needs ccache on PATH, and it is not installed.' -ForegroundColor Yellow
        Write-Host 'Docs/Workflow.md -> Compiler cache covers installing it.' -ForegroundColor Yellow
        exit 0
    }
    $useCache = $true
}

function Invoke-ScopedBuild {
    <#
      .SYNOPSIS
        Wipe the build directory, build both targets, and hash the scoped artifacts.
      .DESCRIPTION
        The same directory every time, deliberately: /Z7 embeds each object's path in
        its debug$S record, so two differently-named build directories differ by
        construction and would report a failure no toolchain could fix.
    #>
    param(
        [Parameter(Mandatory)][string]$Label,
        [Parameter(Mandatory)][bool]$CacheEnabled
    )

    Write-Host "`n==> $Label" -ForegroundColor Cyan
    if (Test-Path -LiteralPath $buildDir) {
        Remove-Item -LiteralPath $buildDir -Recurse -Force
    }

    $previousDisable = $env:CCACHE_DISABLE
    $previousDir = $env:CCACHE_DIR
    try {
        # Removed rather than set empty: ccache reads CCACHE_DISABLE as "set at all",
        # so an empty value would disable the cache in the mode that needs it on.
        if ($CacheEnabled) {
            Remove-Item Env:\CCACHE_DISABLE -ErrorAction SilentlyContinue
        }
        else {
            $env:CCACHE_DISABLE = '1'
        }
        $env:CCACHE_DIR = $cacheDir
        # Piped to Out-Host: a native command's stdout inside a function is part of
        # the function's return value, and this one returns a hash table.
        & $buildScript all -Config $Config | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "$Label failed (build.ps1 exited $LASTEXITCODE)."
        }
    }
    finally {
        # Same asymmetry on the way out: restoring an unset variable as '' would leave
        # the caller's shell with the cache disabled.
        if ([string]::IsNullOrEmpty($previousDisable)) {
            Remove-Item Env:\CCACHE_DISABLE -ErrorAction SilentlyContinue
        }
        else {
            $env:CCACHE_DISABLE = $previousDisable
        }
        if ([string]::IsNullOrEmpty($previousDir)) {
            Remove-Item Env:\CCACHE_DIR -ErrorAction SilentlyContinue
        }
        else {
            $env:CCACHE_DIR = $previousDir
        }
    }

    $table = Get-ArtifactHash -BuildDir $buildDir
    Write-Host "  $($table.Count) scoped artifact(s) hashed." -ForegroundColor DarkGray
    return $table
}

Write-Host "==> Release reproducibility ($Mode, $Config)" -ForegroundColor Cyan
Write-Host "  Build directory: $buildDir" -ForegroundColor DarkGray
$plan = @(Get-BuildPlan -Mode $Mode)
Write-Host "  $($plan.Count) full builds; this takes minutes." -ForegroundColor DarkGray

try {
    $comparable = [System.Collections.Generic.List[hashtable]]::new()
    foreach ($step in $plan) {
        $hash = Invoke-ScopedBuild -Label $step.Label -CacheEnabled $step.CacheEnabled
        if ($step.Compare) { $comparable.Add($hash) }
    }
    $firstHash = $comparable[0]
    $secondHash = $comparable[1]

    # Read before the cache directory is removed below.
    $cacheHits = 0
    if ($useCache) {
        $previousCacheDir = $env:CCACHE_DIR
        try {
            $env:CCACHE_DIR = $cacheDir
            $cacheHits = Get-CacheHitCount -StatsLine @(& ccache --print-stats)
        }
        finally {
            if ([string]::IsNullOrEmpty($previousCacheDir)) {
                Remove-Item Env:\CCACHE_DIR -ErrorAction SilentlyContinue
            }
            else {
                $env:CCACHE_DIR = $previousCacheDir
            }
        }
    }
}
finally {
    if (Test-Path -LiteralPath $cacheDir) {
        Remove-Item -LiteralPath $cacheDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$verdict = Compare-ArtifactHash -First $firstHash -Second $secondHash

Write-Host "`n--- Reproducibility summary ---" -ForegroundColor Cyan
Write-Host ("  {0,-22} {1}" -f 'Mode', $Mode) -ForegroundColor DarkGray
Write-Host ("  {0,-22} {1}" -f 'Artifacts compared', $verdict.Compared) -ForegroundColor DarkGray
Write-Host ("  {0,-22} {1}" -f 'Differing', $verdict.Differing.Count) -ForegroundColor DarkGray
Write-Host ("  {0,-22} {1}" -f 'Present in one build', $verdict.Missing.Count) -ForegroundColor DarkGray

if ($useCache) {
    Write-Host ("  {0,-22} {1}" -f 'Cache hits (warm)', $cacheHits) -ForegroundColor DarkGray
}

foreach ($item in @($verdict.Differing) + @($verdict.Missing)) {
    Write-Host "  DIFF  $item" -ForegroundColor Red
}

# Before the verdict, because a cache that served nothing makes the comparison above
# answer the Determinism question instead -- with a PASS that reads as this one.
if ($useCache -and $cacheHits -eq 0) {
    Write-Host ''
    Write-Host 'FAIL: the warm build read nothing from the compiler cache, so this run did' -ForegroundColor Red
    Write-Host 'not compare a cached build against an uncached one. Check that build.ps1 still' -ForegroundColor Yellow
    Write-Host 'picks ccache up from PATH (Docs/Workflow.md -> Compiler cache).' -ForegroundColor Yellow
    exit 1
}

if (-not $verdict.Ok) {
    Write-Host ''
    if ($verdict.Compared -eq 0) {
        Write-Host 'FAIL: nothing was compared. The scope filter matched no artifact, which means' -ForegroundColor Red
        Write-Host 'the build directory or a target name moved -- not that the build is reproducible.' -ForegroundColor Yellow
    }
    else {
        Write-Host 'FAIL: the Release build is not reproducible.' -ForegroundColor Red
        Write-Host 'The engine target links with ThinLTO, where the compile-side /Brepro is inert and' -ForegroundColor Yellow
        Write-Host 'identity rests on frontend determinism; the linker-side /Brepro settles the PE.' -ForegroundColor Yellow
        Write-Host 'A newly-embedded timestamp, path or __DATE__ is the thing to look for (#513).' -ForegroundColor Yellow
    }
    exit 1
}

Write-Host ''
Write-Host "PASS: $($verdict.Compared) artifact(s) byte-identical across the compared builds." -ForegroundColor Green
exit 0
