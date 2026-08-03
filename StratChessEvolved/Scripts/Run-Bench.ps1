<#
.SYNOPSIS
    Measure search speed (nodes per second) at fixed depth over a position set.

.DESCRIPTION
    Drives the engine over UCI and reports nodes, time, nps and the best move for
    each position, plus an aggregate.

    Intended for comparing two BUILDS of the same source — a different compiler,
    optimisation setting, or an optimisation change. Run it once per binary and
    compare the tables.

    Why nps and not node counts: at a fixed depth the node count is a property of
    the search, not of the machine code, so it is the wrong thing to compare
    between builds. It is still reported, because at Threads=1 it is the
    EQUIVALENCE check — two builds of identical source must visit identical nodes
    and return identical best moves. A node-count difference means code
    generation changed search behaviour, and any nps comparison is then
    meaningless. See Docs/EloLog.md and issue #161 for why node counts alone have
    misled this project before.

    Each position runs in a FRESH ENGINE PROCESS so no transposition-table state
    carries between them. Process startup is excluded from the measurement: the
    reported time is the engine's own, not wall clock.

.PARAMETER Exe
    Path to StratChessEvolved.exe. Required.

.PARAMETER Depth
    Fixed search depth. Default 12, calibrated on the current dev machine to keep
    every position above MinTimeMs while holding a full pass to under ten seconds.
    Raise it on faster hardware — the script warns per position when a search
    finishes too quickly to time reliably.

.PARAMETER Threads
    UCI Threads option. Default 1. Leave it at 1 for any comparison: Lazy SMP is
    non-deterministic, so node counts stop being reproducible and the equivalence
    check is lost.

.PARAMETER Positions
    Optional file of FENs, one per line; blank lines and lines starting with '#'
    are ignored. Defaults to a built-in set spanning opening, middlegame and
    endgame. Every FEN needs its side-to-move field or the engine silently plays
    the wrong side (bug #46).

.PARAMETER Csv
    Optional path to also write results as CSV, for diffing two runs.

.PARAMETER MinTimeMs
    Warn when a position completes faster than this, since short searches make
    nps unreliable. Default 200.

.EXAMPLE
    .\Run-Bench.ps1 -Exe (.\StratChessEvolved\Scripts\Get-BuildArtifact.ps1)

.EXAMPLE
    # Comparing two builds. Both must come from the same compiler unless the
    # compiler IS what is being compared -- otherwise the toolchain difference
    # swamps whatever change is under test.
    .\Run-Bench.ps1 -Exe .\build\windows-clang-cl\StratChessEvolved.exe -Depth 10 -Csv before.csv
    .\Run-Bench.ps1 -Exe .\build\windows-msvc\StratChessEvolved.exe     -Depth 10 -Csv after.csv
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Exe,

    [ValidateRange(1, 30)]
    [int]$Depth = 12,

    [ValidateRange(1, 32)]
    [int]$Threads = 1,

    [string]$Positions = '',

    [string]$Csv = '',

    [int]$MinTimeMs = 200
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Built-in set: opening, middlegame and endgame, chosen so no position is trivial
# at the default depth. Every FEN carries its side-to-move field (bug #46).
$DefaultPositions = @(
    @{ Name = 'startpos';    Fen = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1' }
    @{ Name = 'kiwipete';    Fen = 'r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1' }
    @{ Name = 'rook-endgm';  Fen = '2r3k1/1p3pp1/p3p2p/8/2PR4/1P3P2/P4KPP/8 w - - 0 1' }
    @{ Name = 'tactical-4';  Fen = 'r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1' }
    @{ Name = 'tactical-5';  Fen = 'rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8' }
    @{ Name = 'open-mid';    Fen = 'r1bqkb1r/pp3ppp/2n1pn2/2pp4/3P1B2/2PBPN2/PP3PPP/RN1QK2R w KQkq - 0 7' }
    @{ Name = 'closed-mid';  Fen = 'r1bq1rk1/pp2ppbp/2np1np1/8/2PNP3/2N1B3/PP2BPPP/R2QK2R w KQ - 0 9' }
    @{ Name = 'piece-endgm'; Fen = '2r3k1/pp3pp1/4p2p/3n4/3P4/P1NBP3/1P3PPP/2R3K1 w - - 0 1' }
)

# Note on position choice: sparse pawn endgames are deliberately absent. They are
# nearly solved by the time the search reaches these depths, so they finish in
# tens of milliseconds and contribute almost nothing to either side of the
# aggregate nps ratio — they measure timer resolution rather than the engine.

function Resolve-Positions {
    param([string]$Path)

    if (-not $Path) { return $DefaultPositions }

    if (-not (Test-Path $Path)) {
        throw "Positions file not found: $Path"
    }

    $i = 0
    $list = foreach ($line in Get-Content $Path) {
        $fen = $line.Trim()
        if (-not $fen -or $fen.StartsWith('#')) { continue }
        $i++
        @{ Name = "pos-$i"; Fen = $fen }
    }

    if (-not $list) { throw "No FENs found in $Path" }
    return $list
}

function Invoke-Search {
    <#
        Runs ONE position in a fresh engine process and returns its reported
        nodes, time and best move. A fresh process guarantees no transposition
        table state carries over from the previous position.
    #>
    param(
        [string]$ExePath,
        [string]$WorkDir,
        [string]$Fen,
        [int]$SearchDepth,
        [int]$ThreadCount
    )

    $script = @(
        'uci'
        'isready'
        "setoption name Threads value $ThreadCount"
        "position fen $Fen"
        "go depth $SearchDepth"
        'quit'
    ) -join "`n"

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName               = $ExePath
    $psi.Arguments              = 'uci'
    $psi.WorkingDirectory       = $WorkDir
    $psi.RedirectStandardInput  = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true
    $psi.UseShellExecute        = $false

    $proc = [System.Diagnostics.Process]::Start($psi)
    $proc.StandardInput.Write($script + "`n")
    $proc.StandardInput.Flush()
    $proc.StandardInput.Close()

    # Generous ceiling: a deep search on a complex position is legitimately slow,
    # and killing it early would silently corrupt the aggregate.
    if (-not $proc.WaitForExit(600000)) {
        $proc.Kill()
        throw "Engine did not finish within 600s (depth $SearchDepth): $Fen"
    }

    $out = $proc.StandardOutput.ReadToEnd()

    # The engine emits one summary info line, then bestmove. Take the LAST info
    # line so this keeps working if per-iteration output is ever added.
    $info = [regex]::Matches($out, 'info depth \d+.*?nodes (\d+) time (\d+)')
    $best = [regex]::Match($out, 'bestmove (\S+)')

    if ($info.Count -eq 0) {
        throw "No parseable 'info ... nodes N time T' line for FEN: $Fen`nEngine output:`n$out"
    }

    $last = $info[$info.Count - 1]
    [pscustomobject]@{
        Nodes = [int64]$last.Groups[1].Value
        Ms    = [int64]$last.Groups[2].Value
        Best  = if ($best.Success) { $best.Groups[1].Value } else { '(none)' }
    }
}

# --- main ------------------------------------------------------------------

$exePath = (Resolve-Path $Exe).Path
if (-not (Test-Path $exePath)) { throw "Engine not found: $Exe" }

# Run from the engine's own directory so it finds game_settings.json and writes
# logs/ where it expects (CLAUDE.md).
$workDir = Split-Path -Parent $exePath

# Named $positionList, not $positions: PowerShell variable names are
# case-insensitive, so $positions would BE the [string]$Positions parameter and
# the array would be silently coerced to a single string.
$positionList = @(Resolve-Positions -Path $Positions)

Write-Host ""
Write-Host "Engine  : $exePath"
Write-Host "Depth   : $Depth    Threads: $Threads    Positions: $($positionList.Count)"
if ($Threads -gt 1) {
    Write-Host "WARNING : Threads > 1 makes node counts non-deterministic; the" -ForegroundColor Yellow
    Write-Host "          equivalence check between builds is not valid."       -ForegroundColor Yellow
}
Write-Host ""
Write-Host ("{0,-12} {1,14} {2,9} {3,12}  {4}" -f 'position', 'nodes', 'ms', 'nps', 'best')
Write-Host ('-' * 62)

$rows       = [System.Collections.Generic.List[object]]::new()
$totalNodes = [int64]0
$totalMs    = [int64]0
$fastCount  = 0

foreach ($p in $positionList) {
    $r = Invoke-Search -ExePath $exePath -WorkDir $workDir -Fen $p.Fen `
                       -SearchDepth $Depth -ThreadCount $Threads

    $nps = if ($r.Ms -gt 0) { [int64]($r.Nodes * 1000 / $r.Ms) } else { 0 }
    $totalNodes += $r.Nodes
    $totalMs    += $r.Ms

    $flag = ''
    if ($r.Ms -lt $MinTimeMs) { $flag = ' (too fast to time)'; $fastCount++ }

    Write-Host ("{0,-12} {1,14:N0} {2,9:N0} {3,12:N0}  {4}{5}" -f `
                $p.Name, $r.Nodes, $r.Ms, $nps, $r.Best, $flag)

    $rows.Add([pscustomobject]@{
        Position = $p.Name
        Fen      = $p.Fen
        Nodes    = $r.Nodes
        Ms       = $r.Ms
        Nps      = $nps
        Best     = $r.Best
    })
}

$aggregate = if ($totalMs -gt 0) { [int64]($totalNodes * 1000 / $totalMs) } else { 0 }

Write-Host ('-' * 62)
Write-Host ("{0,-12} {1,14:N0} {2,9:N0} {3,12:N0}" -f 'TOTAL', $totalNodes, $totalMs, $aggregate)
Write-Host ""
Write-Host "Aggregate nps: $('{0:N0}' -f $aggregate)"

if ($fastCount -gt 0) {
    Write-Host ""
    Write-Host "WARNING: $fastCount position(s) finished under ${MinTimeMs}ms. Timing" -ForegroundColor Yellow
    Write-Host "         resolution makes their nps unreliable — raise -Depth."        -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Comparing builds: node counts and best moves must match EXACTLY at"
Write-Host "Threads=1. If they do not, the builds are not searching the same tree"
Write-Host "and the nps difference is not a like-for-like speed comparison."

if ($Csv) {
    $rows | Export-Csv -Path $Csv -NoTypeInformation
    Write-Host ""
    Write-Host "CSV written: $Csv"
}
