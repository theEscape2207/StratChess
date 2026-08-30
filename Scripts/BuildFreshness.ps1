<#
.SYNOPSIS
    Shared artifact-freshness checks, dot-sourced by build.ps1 and
    Get-BuildArtifact.ps1 so a build's own verdict and a caller's read-time
    check agree on what "stale" means.
#>

Set-StrictMode -Version Latest

# Was this artifact produced after the newest source it is built from?
#
# Pure, so the verdict is testable without a build (see build.ps1 -SelfTest). It
# compares timestamps rather than consulting ninja: `ninja -n` cannot answer this
# here, because CONFIGURE_DEPENDS leaves a pending glob re-check on every
# invocation and the dry run stops at "Re-running CMake" without ever evaluating
# the downstream edges. It reports a genuinely stale binary as having no work to do.
#
# $Sources takes objects with Path and WriteTime, so callers decide what counts as
# a source and the comparison stays free of filesystem access.
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
