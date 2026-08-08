<#
.SYNOPSIS
    Validate move generation against perftcheck's 142,953-position corpus.

.DESCRIPTION
    perftcheck (https://grandchesstree.com/perftcheck, Apache-2.0) drives a UCI
    engine with `go perft <depth>` and compares its divide output against a
    Stockfish/TGCT oracle. This script resolves the binaries, runs it, and then
    classifies the failures -- which is the part that cannot be skipped.

    A clean run does NOT report zero failures. The corpus contains positions the
    FEN parser correctly rejects (pawns on rank 1/8, nine pawns, more than 32
    pieces); the engine resets to the start position and answers for that instead,
    which the oracle scores as a mismatch. Those failures carry a fingerprint --
    an `actual` node count equal to the start position's perft for that depth --
    and this script buckets them mechanically. Anything without the fingerprint is
    a genuine move-generation finding, and the exit code says so.

.WHEN TO USE
    After any change to MoveGenerator, Board's make/unmake, or the FEN parser, and
    when a periodic exhaustive check of move generation is wanted. The committed
    `perft test` suite (131 positions, 655 checks, ~30 s) is the fast gate and runs
    in CI; this is the breadth instrument and runs locally on demand.

.HOW TO INVOKE (from bash, cmd, or PowerShell)
    Full corpus including edge cases -- the exhaustive pass, ~25 minutes:
    cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-PerftCheck.ps1"
    Bounded sanity run, well under a minute:
    cmd.exe /c "pwsh ... Run-PerftCheck.ps1 -Limit 5000"
    Re-read a past run without repeating it:
    cmd.exe /c "pwsh ... Run-PerftCheck.ps1 -ClassifyReport <path to report.json>"

.NOTES
    Must be invoked with -File, not dot-sourced. $PSScriptRoot is $null under dot-source.
    Build the engine first -- this script does not build it, and the binary must
    carry the UCI `go perft` command and the reset-on-rejected-FEN behaviour.
    Cost per case is superlinear (the corpus is ordered roughly simplest-first), so
    a -Limit run's rate does NOT extrapolate: 1.09 ms/case over the first 5,000
    against 3.29 ms/case over the next 15,000. Measure at the limit you intend.
    perftcheck.exe (~84 MB) is not committed. It lives in EngineTesting\ beside the
    main checkout with fastchess and the reference builds, shared by every worktree.
#>

param(
    # Engine under test. Default: this repo's shipping (clang-cl Release) build,
    # resolved by Get-BuildArtifact.ps1 rather than hardcoded -- a literal path
    # can silently name another checkout's stale binary.
    [string]$Engine = '',
    # perftcheck binary. Default: EngineTesting\perftcheck.exe beside the main checkout.
    [string]$PerftCheckExe = '',
    # First N matching cases only. 0 runs the whole corpus. A case is a
    # position-depth pair, not a position.
    [int]$Limit = 0,
    # Depth range. perftcheck's own defaults, and what every recorded run used.
    [int]$DepthMin = 1,
    [int]$DepthCap = 4,
    # Skip the positions tagged edge_case_engine_disagreement. They are excluded by
    # perftcheck's default and included here: they are the pathological slice, so
    # they are where a move-generation gap would surface. A failure among them is
    # not automatically a defect -- production engines disagree on them too.
    [switch]$SkipEdgeCases,
    # Report path. Default: a timestamped file in EngineTesting\, never in the repo.
    [string]$Report = '',
    # Per-case timeout. A case that hits it is a performance cliff, not a wrong
    # answer, and is counted separately.
    [int]$TimeoutSeconds = 30,
    # Stop at the first failure. Useful when chasing one position, useless for a
    # survey -- the rejected-FEN failures below would end the run immediately.
    [switch]$FailFast,
    # Classify an existing report and exit, running nothing. A run costs 25 minutes
    # and re-reading one costs nothing, so a past report in EngineTesting\ can be
    # re-examined without the engine or the tool being present.
    [string]$ClassifyReport = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$GameDir  = Split-Path $PSScriptRoot -Parent
$RepoRoot = Split-Path $GameDir -Parent

# --- Locate EngineTesting\ ---------------------------------------------------
# Resolved through git rather than by probing for a marker file: a worktree's
# .git is a file, and --git-common-dir always points at the main repository's
# .git however deeply the worktree is nested.
$mainRoot      = (git -C $RepoRoot rev-parse --path-format=absolute --git-common-dir) -replace '[\\/]\.git[\\/]?$', ''
$DepsRoot      = Split-Path $mainRoot -Parent
$EngineTesting = Join-Path $DepsRoot 'EngineTesting'

# --- Run ---------------------------------------------------------------------
# Sets $script:reportFile rather than returning it: a native command's stdout
# joins the function's output stream, so capturing a return value here would
# swallow the live progress counter and hand back the tool's console lines.
# Everything below classifies that file and never cares which branch set it.
function Invoke-PerftCheckRun {
    $toolExe = if ($PerftCheckExe -ne '') { $PerftCheckExe } else { Join-Path $EngineTesting 'perftcheck.exe' }
    if (-not (Test-Path $toolExe)) {
        Write-Host "MISSING perftcheck: $toolExe" -ForegroundColor Red
        Write-Host 'Download the ~84 MB self-contained binary (Apache-2.0) and drop it there:' -ForegroundColor Yellow
        Write-Host '  https://github.com/Timmoth/grandchesstree/releases/download/perftcheck-0.3.0/perftcheck-win-x64.exe' -ForegroundColor Yellow
        exit 1
    }

    $engineExe = if ($Engine -ne '') { $Engine } else { & (Join-Path $PSScriptRoot 'Get-BuildArtifact.ps1') }
    if (-not (Test-Path $engineExe)) {
        Write-Host "MISSING engine exe: $engineExe" -ForegroundColor Red
        Write-Host 'Build it first: .\build.ps1 main' -ForegroundColor Yellow
        exit 1
    }

    $reportPath = if ($Report -ne '') { $Report }
                  else { Join-Path $EngineTesting "perft-report-$(Get-Date -Format 'yyyyMMdd-HHmmss').json" }

    # Not $args: that is an automatic variable, and writing to it is a trap for
    # whoever edits this next.
    $toolArgs = @(
        '--engine', $engineExe
        '--depth-min', $DepthMin
        '--depth-cap', $DepthCap
        '--timeout', $TimeoutSeconds
        '--report', $reportPath
    )
    if (-not $SkipEdgeCases) { $toolArgs += '--include-edge-cases' }
    if ($Limit -gt 0)        { $toolArgs += @('--limit', $Limit) }
    if ($FailFast)           { $toolArgs += '--fail-fast' }

    $scopeLabel = if ($Limit -gt 0) { "first $Limit cases" } else { 'the full corpus (~25 min)' }
    $edgeLabel  = if ($SkipEdgeCases) { '' } else { ', edge cases included' }
    Write-Host "==> perftcheck over $scopeLabel, depths $DepthMin-$DepthCap$edgeLabel" -ForegroundColor Cyan
    Write-Host "    engine: $engineExe" -ForegroundColor DarkGray
    Write-Host "    report: $reportPath" -ForegroundColor DarkGray

    # Run from the game directory so any engine-side log lands in the usual place.
    # --quiet is never passed: the live "N pass M fail" counter is the only
    # progress signal a run of this length has.
    Push-Location $GameDir
    try {
        & $toolExe @toolArgs
        $toolExit = $LASTEXITCODE
    } finally {
        Pop-Location
    }

    if (-not (Test-Path $reportPath)) {
        Write-Host "`nperftcheck wrote no report (exit $toolExit) -- nothing to classify." -ForegroundColor Red
        exit 1
    }
    $script:reportFile = $reportPath
}

if ($ClassifyReport -ne '') {
    if (-not (Test-Path $ClassifyReport)) {
        Write-Host "MISSING report: $ClassifyReport" -ForegroundColor Red
        exit 1
    }
    Write-Host "==> Classifying an existing report, running nothing: $ClassifyReport" -ForegroundColor Cyan
    $reportFile = $ClassifyReport
} else {
    Invoke-PerftCheckRun
}

# --- Classify the failures ---------------------------------------------------
# Perft of the start position by depth. A rejected FEN leaves the engine on the
# start position, so a failure whose actual count matches this is the parser
# refusing an illegal position rather than a move-generation fault. The converse
# does not hold: a position whose true count happens to equal the start
# position's passes the check outright, which is why the fingerprint classifies
# failures and never certifies passes.
$startposPerft = @{ 1 = 20; 2 = 400; 3 = 8902; 4 = 197281; 5 = 4865609; 6 = 119060324; 7 = 3195901860 }

function Test-IsRejectedFen($failure) {
    $depth = [int]$failure.depth
    return $startposPerft.ContainsKey($depth) -and [long]$failure.actual -eq [long]$startposPerft[$depth]
}

$parsed   = Get-Content $reportFile -Raw | ConvertFrom-Json
$totals   = $parsed.totals
$failures = @($parsed.failures)

$rejected = @($failures | Where-Object { Test-IsRejectedFen $_ })
$genuine  = @($failures | Where-Object { -not (Test-IsRejectedFen $_) })

$distinctRejected = @($rejected | ForEach-Object { $_.fen } | Sort-Object -Unique).Count
$distinctGenuine  = @($genuine  | ForEach-Object { $_.fen } | Sort-Object -Unique).Count
$genuineColour    = if ($genuine.Count -gt 0) { 'Red' } else { 'Green' }

Write-Host ''
Write-Host "==> Result ($([math]::Round($parsed.durationSeconds / 60, 1)) min)" -ForegroundColor Cyan
Write-Host "    checks        : $($totals.cases)"
Write-Host "    passed        : $($totals.passed)"
Write-Host "    failed        : $($totals.failed)"
Write-Host "    timeout/error : $($totals.timeout) / $($totals.error)"
Write-Host "    rejected FENs : $($rejected.Count) failures over $distinctRejected positions (engine answered for the start position)"
Write-Host "    unexplained   : $($genuine.Count) failures over $distinctGenuine positions" -ForegroundColor $genuineColour

foreach ($f in $genuine) {
    Write-Host "      d$($f.depth) expected $($f.expected), got $($f.actual)  $($f.fen)" -ForegroundColor Red
}

if ($totals.timeout -gt 0 -or $totals.error -gt 0) {
    Write-Host "`nTimeouts or harness errors occurred -- a performance cliff or a broken invocation," -ForegroundColor Red
    Write-Host 'not a wrong answer. Investigate before reading the pass count as a result.' -ForegroundColor Red
    exit 1
}

if ($genuine.Count -gt 0) {
    Write-Host "`nFAIL: move generation disagrees with the oracle on positions the parser accepted." -ForegroundColor Red
    Write-Host 'Reproduce one by hand over UCI: position fen <FEN> / go perft <depth>.' -ForegroundColor Yellow
    Write-Host "Narrow it with: perftcheck.exe --engine <exe> --filter '<fen substring>' --drill-down" -ForegroundColor Yellow
    exit 1
}

$passDetail = if ($failures.Count -eq 0) { 'no failures at all' }
              else { 'every failure is a rejected FEN' }
Write-Host "`nPASS: $passDetail. Move generation matches the oracle on every position it was given." -ForegroundColor Green
exit 0
