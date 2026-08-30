<#
.SYNOPSIS
    Measure search speed (nodes per second) at fixed depth over a position set.

.DESCRIPTION
    Drives the engine over UCI and reports the main-tree and quiescence node
    counts, their total, time, nps and the best move for each position, plus an
    aggregate.

    Intended for comparing two BUILDS of the same source — a different compiler,
    optimisation setting, or an optimisation change. Run it once per binary and
    compare the tables.

    Why nps and not node counts: at a fixed depth the node count is a property of
    the search, not of the machine code, so it is the wrong thing to compare
    between builds. It is still reported, because at Threads=1 it is the
    EQUIVALENCE check — two builds of identical source must visit identical nodes
    and return identical best moves. A node-count difference means code
    generation changed search behaviour, and any nps comparison is then
    meaningless. See Docs/EloMeasurement.md and issue #161 for why node counts
    alone have misled this project before.

    Why the main/qs split: nps is only meaningful when the node count covers all the
    work the clock is charged for. Before issue #312 the engine counted main-tree
    nodes only, so a change that moved work into quiescence showed up as an nps
    collapse with no visible cause — that is exactly how #306 was misread. If the
    two columns move in opposite directions, read the WALL CLOCK, not nps: it is
    the only figure that cannot be distorted by relocating work between the trees.

    A build predating #312 does not emit the split; those rows show '-' and the
    script says so, because that build's nps is not comparable with a newer one's.

    Each position runs in a FRESH ENGINE PROCESS so no transposition-table state
    carries between them. Process startup is excluded from the measurement: the
    reported time is the engine's own, not wall clock.

.PARAMETER Exe
    Path to StratChessEvolved.exe. Required for a benchmark run.

.PARAMETER Depth
    Fixed search depth. Default 12, calibrated on the current dev machine to keep
    every position above MinTimeMs while holding a full pass to under ten seconds.
    Raise it on faster hardware — the script warns per position when a search
    finishes too quickly to time reliably.

    Completion is strict: every position must emit both `info depth <Depth>` and
    `bestmove`. A terminal, mate, or other early-stop custom position that ends
    before -Depth is rejected rather than benchmarked as a fixed-depth result.
    Choose nonterminal positions or lower -Depth for custom suites.

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

.PARAMETER SelfTest
    Assert the transcript parsing and position resolution against synthetic input and
    exit. Runs no engine. Exits 1 on any failure.

.EXAMPLE
    .\Run-Bench.ps1 -Exe (.\Scripts\Get-BuildArtifact.ps1)

.EXAMPLE
    .\Run-Bench.ps1 -SelfTest

.EXAMPLE
    # Comparing two builds. Both must come from the same compiler unless the
    # compiler IS what is being compared -- otherwise the toolchain difference
    # swamps whatever change is under test.
    .\Run-Bench.ps1 -Exe .\build\windows-clang-cl\StratChessEvolved.exe -Depth 10 -Csv before.csv
    .\Run-Bench.ps1 -Exe .\build\windows-msvc\StratChessEvolved.exe     -Depth 10 -Csv after.csv
#>
[CmdletBinding()]
param(
    # Not Mandatory, so -SelfTest can run without one. A missing -Exe is rejected
    # explicitly in the main section instead: a mandatory parameter would prompt, and
    # the validation gate runs this script in a child pwsh with no console to prompt on.
    [string]$Exe = '',

    [ValidateRange(1, 30)]
    [int]$Depth = 12,

    [ValidateRange(1, 32)]
    [int]$Threads = 1,

    [string]$Positions = '',

    [string]$Csv = '',

    [int]$MinTimeMs = 200,

    [switch]$SelfTest
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

function ConvertTo-BenchResult {
    <#
        Turn one engine transcript into the row the table is built from. Pure, so
        -SelfTest can assert both the parsing and the refusals without an engine.
    #>
    param(
        [Parameter(Mandatory)][AllowEmptyString()][string]$Output,
        [Parameter(Mandatory)][int]$SearchDepth,
        [Parameter(Mandatory)][string]$Fen
    )

    # The engine emits one summary info line, then bestmove. Take the LAST info
    # line so this keeps working if per-iteration output is ever added.
    $info = [regex]::Matches($Output, 'info depth \d+.*?nodes (\d+) time (\d+)')
    $best = [regex]::Match($Output, 'bestmove (\S+)')

    if ($info.Count -eq 0) {
        throw "No parseable 'info ... nodes N time T' line for FEN: $Fen`nEngine output:`n$Output"
    }
    if (@($info | Where-Object { $_.Value -match "^info depth $SearchDepth\b" }).Count -eq 0 -or -not $best.Success) {
        throw ("Fixed-depth search did not complete depth $SearchDepth for FEN: $Fen" +
               "`nEngine output:`n$Output")
    }

    # Main-tree/quiescence split (issue #312). Engines built before that change do not
    # emit it, and comparing against such a build is the normal case for a before/after
    # run, so its absence is reported as unknown rather than as zero.
    $contract = [regex]::Match($Output, 'info string benchcontract (\d+)')
    $split    = [regex]::Matches($Output, 'info string treenodes main (\d+) qs (\d+)')
    $contractNo = if ($contract.Success) { [int]$contract.Groups[1].Value } else { 0 }
    $nodes     = [int64]$info[$info.Count - 1].Groups[1].Value
    $mainNodes = $null
    $qsNodes   = $null
    if ($split.Count -gt 0) {
        $lastSplit = $split[$split.Count - 1]
        $mainNodes = [int64]$lastSplit.Groups[1].Value
        $qsNodes   = [int64]$lastSplit.Groups[2].Value
    }

    # A contract >= 1 build promises the split on every search, and that it sums to the
    # total. Either failing means engine and parser have drifted, so refuse the run.
    if ($contractNo -ge 1 -and $split.Count -eq 0) {
        throw ("Contract $contractNo build emitted no 'info string treenodes' line for FEN: $Fen" +
               "`nThe engine and this script disagree about the measurement contract." +
               "`nEngine output:`n$Output")
    }
    if ($null -ne $mainNodes -and ($mainNodes + $qsNodes) -ne $nodes) {
        throw ("Node split does not sum to the reported total for FEN: $Fen" +
               "`n  main $mainNodes + qs $qsNodes = $($mainNodes + $qsNodes), but 'nodes' says $nodes." +
               "`nEngine output:`n$Output")
    }

    $last = $info[$info.Count - 1]
    return [pscustomobject]@{
        Nodes     = $nodes
        MainNodes = $mainNodes
        QsNodes   = $qsNodes
        Ms        = [int64]$last.Groups[2].Value
        Best      = if ($best.Success) { $best.Groups[1].Value } else { '(none)' }
        Contract  = $contractNo
    }
}

# Invoke-UciSearchToBestMove lives in the shared library because
# Compare-SearchEquivalence.ps1 drives the engine the same way; a fix to the shutdown
# sequence has to reach both.
. (Join-Path $PSScriptRoot 'UciDriver.ps1')

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

    $commands = @(
        'uci'
        'isready'
        "setoption name Threads value $ThreadCount"
        "position fen $Fen"
        "go depth $SearchDepth"
    )

    # Generous ceiling: a deep search on a complex position is legitimately slow,
    # and killing it early would silently corrupt the aggregate.
    $out = Invoke-UciSearchToBestMove -ExePath $ExePath -WorkDir $WorkDir -Commands $commands `
                                      -SearchDepth $SearchDepth -Description $Fen

    ConvertTo-BenchResult -Output $out -SearchDepth $SearchDepth -Fen $Fen
}

# ---------------------------------------------------------------------------
# Self-test — covers the pure halves: transcript parsing and position resolution.
# The UCI driver above is NOT covered; it spawns a real engine, and nothing here
# substitutes for an actual bench pass.
# ---------------------------------------------------------------------------

if ($SelfTest) {
    $failures = 0

    function Assert-Case {
        param([string]$Name, [bool]$Ok, [string]$Detail = '')
        if ($Ok) {
            Write-Host ("  PASS  {0}" -f $Name) -ForegroundColor Green
        } else {
            Write-Host ("  FAIL  {0}{1}" -f $Name, $(if ($Detail) { " — $Detail" } else { '' })) -ForegroundColor Red
            $script:failures++
        }
    }

    # -Match is not decoration: several refusals overlap, so asserting only "it threw"
    # would let a guard be deleted while a later one still throws something else. The
    # pattern pins which diagnostic the caller actually gets.
    function Test-Refuses {
        param([scriptblock]$Action, [Parameter(Mandatory)][string]$Match)
        try { & $Action | Out-Null; return $false }
        catch { return $_.Exception.Message -match $Match }
    }

    $contract1 = @(
        'id name StratChessEvolved'
        'uciok'
        'readyok'
        'info string benchcontract 1'
        'info depth 1 score cp 24 nodes 21 time 3 pv e2e4'
        'info depth 2 score cp 12 nodes 140 time 9 pv e2e4 e7e5'
        'info string treenodes main 100 qs 40'
        'bestmove e2e4'
    ) -join "`n"

    $r = ConvertTo-BenchResult -Output $contract1 -SearchDepth 2 -Fen 'startpos'
    Assert-Case 'nodes, time, best move and contract are parsed' `
        ($r.Nodes -eq 140 -and $r.Ms -eq 9 -and $r.Best -eq 'e2e4' -and $r.Contract -eq 1) `
        "got nodes $($r.Nodes) ms $($r.Ms) best $($r.Best) contract $($r.Contract)"
    Assert-Case 'main/qs split is parsed' ($r.MainNodes -eq 100 -and $r.QsNodes -eq 40) `
        "got main $($r.MainNodes) qs $($r.QsNodes)"

    # A build predating #312: no contract line, no split. Reported as unknown, not zero,
    # because comparing against such a build is the normal before/after case.
    $contract0 = @(
        'info depth 1 score cp 24 nodes 21 time 3 pv e2e4'
        'info depth 2 score cp 12 nodes 140 time 9 pv e2e4 e7e5'
        'bestmove e2e4'
    ) -join "`n"
    $r = ConvertTo-BenchResult -Output $contract0 -SearchDepth 2 -Fen 'startpos'
    Assert-Case 'a pre-#312 build reports the split as unknown' `
        ($null -eq $r.MainNodes -and $null -eq $r.QsNodes -and $r.Contract -eq 0)

    # The last info line wins, so per-iteration output cannot make an early iteration
    # the reported result. Same for the split.
    $twoFinals = $contract1 -replace 'bestmove e2e4', @(
        'info depth 2 score cp 12 nodes 150 time 11 pv e2e4 e7e5'
        'info string treenodes main 110 qs 40'
        'bestmove e2e4'
    ) -join "`n"
    $r = ConvertTo-BenchResult -Output $twoFinals -SearchDepth 2 -Fen 'startpos'
    Assert-Case 'the LAST info and split lines are the reported result' `
        ($r.Nodes -eq 150 -and $r.MainNodes -eq 110) "got nodes $($r.Nodes) main $($r.MainNodes)"

    # The refusals. Each is a drift between engine and parser that would otherwise be
    # averaged into an aggregate and reported as a measurement.
    Assert-Case 'FALSIFY: contract 1 without a treenodes line is refused' `
        (Test-Refuses -Match "no 'info string treenodes' line" `
            { ConvertTo-BenchResult -Output ($contract1 -replace 'info string treenodes main 100 qs 40\r?\n', '') -SearchDepth 2 -Fen 'x' })
    Assert-Case 'FALSIFY: a split that does not sum to the total is refused' `
        (Test-Refuses -Match 'does not sum to the reported total' `
            { ConvertTo-BenchResult -Output ($contract1 -replace 'qs 40', 'qs 39') -SearchDepth 2 -Fen 'x' })
    Assert-Case 'FALSIFY: not reaching the requested depth is refused' `
        (Test-Refuses -Match 'did not complete depth 3' `
            { ConvertTo-BenchResult -Output $contract1 -SearchDepth 3 -Fen 'x' })
    Assert-Case 'FALSIFY: a missing bestmove is refused' `
        (Test-Refuses -Match 'did not complete depth 2' `
            { ConvertTo-BenchResult -Output ($contract1 -replace 'bestmove e2e4', '') -SearchDepth 2 -Fen 'x' })
    Assert-Case 'FALSIFY: output with no info line is refused, by name' `
        (Test-Refuses -Match 'No parseable' `
            { ConvertTo-BenchResult -Output 'uciok' -SearchDepth 2 -Fen 'x' })

    # Position resolution, against real files -- the parsing is file-shaped.
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) "run-bench-selftest-$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Path $tmp -Force | Out-Null
    try {
        Assert-Case 'no -Positions uses the built-in set' `
            (@(Resolve-Positions -Path '').Count -eq $DefaultPositions.Count)

        $multi = Join-Path $tmp 'multi.txt'
        Set-Content $multi @(
            '# a comment'
            ''
            '8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1'
            '   '
            '2r3k1/pp3pp1/4p2p/3n4/3P4/P1NBP3/1P3PPP/2R3K1 w - - 0 1'
        )
        $list = @(Resolve-Positions -Path $multi)
        Assert-Case 'comments and blank lines are skipped' ($list.Count -eq 2) "got $($list.Count)"
        Assert-Case 'positions are named in file order' `
            ($list[0].Name -eq 'pos-1' -and $list[1].Name -eq 'pos-2')

        # A one-element result unrolls on return, so the call site's @() is what makes
        # .Count safe. Assert the shape the caller actually sees.
        $single = Join-Path $tmp 'single.txt'
        Set-Content $single '8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1'
        Assert-Case 'a single-FEN file is still a collection of one' `
            (@(Resolve-Positions -Path $single).Count -eq 1)

        $empty = Join-Path $tmp 'empty.txt'
        Set-Content $empty @('# nothing but a comment', '')
        Assert-Case 'FALSIFY: a file with no FENs is refused' `
            (Test-Refuses -Match 'No FENs found' { Resolve-Positions -Path $empty })
        Assert-Case 'FALSIFY: a missing positions file is refused' `
            (Test-Refuses -Match 'Positions file not found' { Resolve-Positions -Path (Join-Path $tmp 'absent.txt') })
    } finally {
        Remove-Item -Path $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }

    Write-Host ''
    if ($failures -gt 0) {
        Write-Host "$failures self-test case(s) FAILED." -ForegroundColor Red
        exit 1
    }
    Write-Host 'All self-test cases passed.' -ForegroundColor Green
    exit 0
}

# --- main ------------------------------------------------------------------

if (-not $Exe) { throw "-Exe is required (the engine binary to benchmark)." }

$exePath = (Resolve-Path $Exe).Path
if (-not (Test-Path $exePath)) { throw "Engine not found: $Exe" }

# Run from the engine's own directory so it finds game_settings.json and writes
# logs/ where it expects (CLAUDE.md).
$workDir = Split-Path -Parent $exePath

# Named $positionList, not $positions: PowerShell variable names are
# case-insensitive, so $positions would BE the [string]$Positions parameter and
# the array would be silently coerced to a single string.
$positionList = @(Resolve-Positions -Path $Positions)

# Everything deciding whether two runs may be compared, on one line, so it survives
# being pasted into a doc. The position hash is the part with no other signal:
# editing the suite silently invalidates every recorded table.
$posHash = & {
    $sha    = [System.Security.Cryptography.SHA256]::Create()
    $joined = ($positionList | ForEach-Object { $_.Fen }) -join "`n"
    $bytes  = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($joined))
    $sha.Dispose()
    (( $bytes | ForEach-Object { $_.ToString('x2') } ) -join '').Substring(0, 12)
}
$exeHash = (Get-FileHash -Path $exePath -Algorithm SHA256).Hash.Substring(0, 12).ToLower()

Write-Host ""
Write-Host "Engine  : $exePath"
Write-Host "Depth   : $Depth    Threads: $Threads    Positions: $($positionList.Count)"
$setName = if ($Positions) { Split-Path -Leaf $Positions } else { 'builtin' }
Write-Host "Set     : $setName sha $posHash    Build: sha $exeHash    Host: $([Environment]::MachineName)"
if ($Threads -gt 1) {
    Write-Host "WARNING : Threads > 1 makes node counts non-deterministic; the" -ForegroundColor Yellow
    Write-Host "          equivalence check between builds is not valid."       -ForegroundColor Yellow
}
Write-Host ""
Write-Host ("{0,-12} {1,13} {2,13} {3,13} {4,8} {5,12}  {6}" -f `
            'position', 'main nodes', 'qs nodes', 'nodes', 'ms', 'nps', 'best')
Write-Host ('-' * 92)

$rows        = [System.Collections.Generic.List[object]]::new()
$totalNodes  = [int64]0
$totalMain   = [int64]0
$totalQs     = [int64]0
$haveSplit   = $true
$totalMs     = [int64]0
$fastCount   = 0
$contracts   = [System.Collections.Generic.HashSet[int]]::new()

foreach ($p in $positionList) {
    $r = Invoke-Search -ExePath $exePath -WorkDir $workDir -Fen $p.Fen `
                       -SearchDepth $Depth -ThreadCount $Threads

    $nps = if ($r.Ms -gt 0) { [int64]($r.Nodes * 1000 / $r.Ms) } else { 0 }
    $totalNodes += $r.Nodes
    $totalMs    += $r.Ms
    [void]$contracts.Add($r.Contract)

    if ($null -eq $r.MainNodes) {
        $haveSplit = $false
        $mainCell = '-'
        $qsCell   = '-'
    } else {
        $totalMain += $r.MainNodes
        $totalQs   += $r.QsNodes
        $mainCell = '{0:N0}' -f $r.MainNodes
        $qsCell   = '{0:N0}' -f $r.QsNodes
    }

    $flag = ''
    if ($r.Ms -lt $MinTimeMs) { $flag = ' (too fast to time)'; $fastCount++ }

    Write-Host ("{0,-12} {1,13} {2,13} {3,13:N0} {4,8:N0} {5,12:N0}  {6}{7}" -f `
                $p.Name, $mainCell, $qsCell, $r.Nodes, $r.Ms, $nps, $r.Best, $flag)

    $rows.Add([pscustomobject]@{
        Position  = $p.Name
        Fen       = $p.Fen
        MainNodes = $r.MainNodes
        QsNodes   = $r.QsNodes
        Nodes     = $r.Nodes
        Ms        = $r.Ms
        Nps       = $nps
        Best      = $r.Best
    })
}

$aggregate = if ($totalMs -gt 0) { [int64]($totalNodes * 1000 / $totalMs) } else { 0 }

$mainTotalCell = if ($haveSplit) { '{0:N0}' -f $totalMain } else { '-' }
$qsTotalCell   = if ($haveSplit) { '{0:N0}' -f $totalQs }   else { '-' }

Write-Host ('-' * 92)
Write-Host ("{0,-12} {1,13} {2,13} {3,13:N0} {4,8:N0} {5,12:N0}" -f `
            'TOTAL', $mainTotalCell, $qsTotalCell, $totalNodes, $totalMs, $aggregate)
Write-Host ""
$contractLabel = ($contracts | Sort-Object) -join ','
Write-Host "Aggregate nps: $('{0:N0}' -f $aggregate)    Wall clock: $('{0:N0}' -f $totalMs) ms    Contract: $contractLabel"

if ($haveSplit -and $totalNodes -gt 0) {
    $qsShare = [math]::Round(100.0 * $totalQs / $totalNodes, 1)
    Write-Host "Quiescence share of nodes: $qsShare%"
}

if ($contracts.Count -gt 1) {
    # One binary answered two different contracts across positions, which should be
    # impossible -- the contract is a compile-time constant. Treat it as a corrupt run.
    Write-Host ""
    Write-Host "ERROR: positions reported differing contracts ($contractLabel). This run" -ForegroundColor Red
    Write-Host "       is not internally consistent; discard it."                          -ForegroundColor Red
} elseif ($contracts.Contains(0)) {
    Write-Host ""
    Write-Host "NOTE: contract 0 -- this build predates issue #312 and counts main-tree"  -ForegroundColor Yellow
    Write-Host "      nodes only, so its 'nodes' and 'nps' are NOT comparable with a"      -ForegroundColor Yellow
    Write-Host "      contract 1 build. Compare wall clock, which is unaffected."          -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Two tables are comparable only if Contract, Set sha, Depth and Threads all"
Write-Host "match. Build sha and Host are recorded so a difference can be attributed."

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
