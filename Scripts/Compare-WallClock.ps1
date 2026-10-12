<#
.SYNOPSIS
    Fixed-depth wall clock of two engine builds over a position set: the mean per-position paired
    time ratio, candidate over baseline, with +-2 standard errors. The #636 Method's timing input.

.DESCRIPTION
    Searches every position once per build at a fixed depth, Threads=1, each in a fresh engine
    process (Run-Bench.ps1), and pairs the two times per position. The time is the engine's own
    reported search time, not process startup.

    Schedule. Positions run in chunks of -ChunkSize. Within a chunk one build searches every
    position, then the other; the build that goes first alternates per chunk, starting with the
    baseline, so machine drift over the run lands on both sides.

    Statistics. The unit is the position: ratio = candidate ms / baseline ms. The report gives the
    mean ratio with +-2 SE (sample SD over positions / sqrt(n)), the total time per build, how many
    positions each build searched faster, and the mean ratio by which build went first: a large gap
    there means drift, and the alternation is what keeps it out of the mean. Any change to move
    order or pruning reshapes every tree, so the spread is mostly tree noise, not timer noise;
    that is why the default set is 200 positions rather than Run-Bench's 8 (#636).

    Gate. It also prints #636's wall-clock gate, which applies only when the owner declines the
    lab: PASS needs a mean change of -3% or better whose +2 SE bound is below zero. With a lab run
    it is context, never a verdict.

    Refusals: a build that returns a different position, or a different number of them, than the
    chunk it was given; a search that reports 0 ms, whose ratio is undefined (raise -Depth).
    Different node counts and best moves are expected and not refused.

    Build both executables the same way, the baseline from the exact merge base (measure-strength).
    Run on a quiet machine: the phase plan and banners say for how long.

.PARAMETER Baseline
    Baseline StratChessEvolved.exe, built from the candidate's merge base.

.PARAMETER Candidate
    Candidate StratChessEvolved.exe. It may be the baseline's path, for an A/A check.

.PARAMETER Depth
    Fixed search depth. Default 14, the #636 Method's.

.PARAMETER Positions
    FEN file, one per line, as Run-Bench.ps1 takes. Default Tests/profile-screen.fen (200).

.PARAMETER ChunkSize
    Positions per chunk. Default 10.

.PARAMETER BaselineCommit
    Commit the baseline was built from, recorded in metadata.json and named in the report.

.PARAMETER CandidateCommit
    Commit the candidate was built from, recorded likewise.

.PARAMETER OutDir
    Where to keep the chunk CSVs, pairs.csv, metadata.json and report.txt. Defaults to a new
    directory under the system temp path. Must not already contain CSVs.

.PARAMETER SelfTest
    Assert the schedule, pairing, refusals, statistics, gate and report on synthetic runs and
    exit. Runs no engine. Exits 1 on any failure.

.EXAMPLE
    .\Compare-WallClock.ps1 -Baseline ..\base\build\windows-clang-cl\StratChessEvolved.exe `
                            -Candidate .\build\windows-clang-cl\StratChessEvolved.exe `
                            -BaselineCommit f9fcf8a -CandidateCommit 6528d36

.EXAMPLE
    .\Compare-WallClock.ps1 -SelfTest
#>
[CmdletBinding()]
param(
    [string]$Baseline = '',

    [string]$Candidate = '',

    [ValidateRange(1, 30)]
    [int]$Depth = 14,

    [string]$Positions = '',

    [ValidateRange(1, 1000)]
    [int]$ChunkSize = 10,

    [string]$BaselineCommit = '',

    [string]$CandidateCommit = '',

    [string]$OutDir = '',

    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Resolve-Positions and Get-PositionSetHash: the file format Run-Bench reads.
. (Join-Path $PSScriptRoot 'BenchPositions.ps1')
# Write-PhasePlan and Write-PhaseBanner: the quiet-window notices.
. (Join-Path $PSScriptRoot 'QuietWindow.ps1')

# #636's wall-clock gate: a mean change at least this fast, in percent, with its +2 SE bound below 0.
$GateChangePct = -3.0
# Rough time of one depth-14 search on the dev machine, for the phase plan only.
$RoughSearchSeconds = 2.5
# Below this a 1 ms timer step is over 1% of a ratio, so the report counts such searches.
$ShortSearchMs = 100

function Get-ChunkPlan {
    <# Consecutive chunks of $Size positions; the first build alternates, the baseline first. #>
    param([Parameter(Mandatory)][int]$Count, [Parameter(Mandatory)][int]$Size)

    $chunks = for ($start = 0; $start -lt $Count; $start += $Size) {
        # Floor returns a double; the cast only changes its type, as the value is integral.
        $index = [int][math]::Floor($start / $Size)
        [pscustomobject]@{
            Index  = $index
            Start  = $start
            Length = [math]::Min($Size, $Count - $start)
            Order  = if ($index % 2 -eq 0) { @('baseline', 'candidate') } else { @('candidate', 'baseline') }
        }
    }
    return $chunks
}

function Join-ChunkRun {
    <#
        Pairs one chunk's two Run-Bench row sets by position. Throws, naming chunk and build, when
        a build returned other positions than it was given or a search reported 0 ms.
    #>
    param(
        [Parameter(Mandatory)][object]$Chunk,
        [Parameter(Mandatory)][string[]]$Fens,
        [Parameter(Mandatory)][hashtable]$Rows
    )

    foreach ($arm in @('baseline', 'candidate')) {
        $armRows = @($Rows[$arm])
        if ($armRows.Count -ne $Fens.Count) {
            throw "Chunk $($Chunk.Index + 1): the $arm build returned $($armRows.Count) positions for $($Fens.Count)."
        }
        for ($i = 0; $i -lt $Fens.Count; $i++) {
            if ($armRows[$i].Fen -ne $Fens[$i]) {
                throw "Chunk $($Chunk.Index + 1), row $($i + 1): the $arm build returned '$($armRows[$i].Fen)', expected '$($Fens[$i])'."
            }
            if ([int64]$armRows[$i].Ms -le 0) {
                throw "Chunk $($Chunk.Index + 1), position $($Chunk.Start + $i + 1): the $arm build reported 0 ms, so the ratio is undefined; raise -Depth."
            }
        }
    }
    $paired = for ($i = 0; $i -lt $Fens.Count; $i++) {
        $b = $Rows['baseline'][$i]
        $c = $Rows['candidate'][$i]
        [pscustomobject]@{
            Position       = $Chunk.Start + $i + 1
            Chunk          = $Chunk.Index + 1
            First          = $Chunk.Order[0]
            BaselineMs     = [int64]$b.Ms
            CandidateMs    = [int64]$c.Ms
            Ratio          = [double]$c.Ms / [double]$b.Ms
            BaselineNodes  = [int64]$b.Nodes
            CandidateNodes = [int64]$c.Nodes
            Fen            = $Fens[$i]
        }
    }
    return $paired
}

function Invoke-WallClockSeries {
    <#
        Runs every chunk in plan order. $RunChunk is called as (arm, fen file, csv path) and must
        leave Run-Bench's CSV at that path; $Progress as (chunks done, chunk count, seconds so far).
        Each chunk is paired as it lands, so a refusal stops before the rest of the budget.
    #>
    param(
        [Parameter(Mandatory)][object[]]$Plan,
        [Parameter(Mandatory)][object[]]$PositionSet,
        [Parameter(Mandatory)][scriptblock]$RunChunk,
        [Parameter(Mandatory)][string]$WorkDir,
        [scriptblock]$Progress = {}
    )

    $all = [System.Collections.Generic.List[object]]::new()
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    foreach ($chunk in $Plan) {
        $fens = @($PositionSet[$chunk.Start..($chunk.Start + $chunk.Length - 1)] | ForEach-Object { $_.Fen })
        $fenFile = Join-Path $WorkDir ('chunk{0:D3}.fen' -f ($chunk.Index + 1))
        Set-Content -LiteralPath $fenFile -Value $fens
        $chunkRows = @{}
        for ($slot = 0; $slot -lt 2; $slot++) {
            $arm = $chunk.Order[$slot]
            $csv = Join-Path $WorkDir ('chunk{0:D3}_{1}_{2}.csv' -f ($chunk.Index + 1), $slot, $arm)
            & $RunChunk $arm $fenFile $csv
            $chunkRows[$arm] = @(Import-Csv -LiteralPath $csv)
        }
        foreach ($pair in @(Join-ChunkRun -Chunk $chunk -Fens $fens -Rows $chunkRows)) { $all.Add($pair) }
        & $Progress ($chunk.Index + 1) $Plan.Count $timer.Elapsed.TotalSeconds
    }
    return $all.ToArray()
}

function Get-WallClockSummary {
    <# Mean ratio +-2 SE over positions, totals per build, faster/slower counts and the order split. #>
    param([Parameter(Mandatory)][object[]]$Pairs)

    $n = $Pairs.Count
    if ($n -lt 2) { throw "A standard error needs at least two positions, got $n." }
    $ratios = @($Pairs | ForEach-Object { $_.Ratio })
    $mean = ($ratios | Measure-Object -Average).Average
    $ss = 0.0
    foreach ($r in $ratios) { $ss += ($r - $mean) * ($r - $mean) }
    $se = [math]::Sqrt($ss / ($n - 1)) / [math]::Sqrt($n)
    $byFirst = [ordered]@{}
    foreach ($arm in @('baseline', 'candidate')) {
        $group = @($Pairs | Where-Object { $_.First -eq $arm })
        $byFirst[$arm] = if ($group.Count -gt 0) { ($group | ForEach-Object { $_.Ratio } | Measure-Object -Average).Average } else { $null }
    }
    return [pscustomobject]@{
        N                = $n
        Mean             = $mean
        Se               = $se
        Low              = $mean - 2 * $se
        High             = $mean + 2 * $se
        BaselineSeconds  = ($Pairs | ForEach-Object { $_.BaselineMs } | Measure-Object -Sum).Sum / 1000.0
        CandidateSeconds = ($Pairs | ForEach-Object { $_.CandidateMs } | Measure-Object -Sum).Sum / 1000.0
        Faster           = @($Pairs | Where-Object { $_.CandidateMs -lt $_.BaselineMs }).Count
        Slower           = @($Pairs | Where-Object { $_.CandidateMs -gt $_.BaselineMs }).Count
        Short            = @($Pairs | Where-Object { [math]::Min($_.BaselineMs, $_.CandidateMs) -lt $ShortSearchMs }).Count
        MeanByFirst      = $byFirst
    }
}

function Test-WallClockGate {
    <# #636: a mean change of $GateChangePct% or better, and the +2 SE bound below zero. #>
    param([Parameter(Mandatory)][object]$Summary)
    # Rounded so a mean of exactly 0.97 is -3%, not -2.9999999999999973%.
    $change = [math]::Round(100 * ($Summary.Mean - 1), 9)
    return $change -le $GateChangePct -and $Summary.High -lt 1
}

function Format-WallClockReport {
    param(
        [Parameter(Mandatory)][object]$Summary,
        [Parameter(Mandatory)][int]$SearchDepth,
        [Parameter(Mandatory)][int]$Size,
        [Parameter(Mandatory)][int]$ChunkCount,
        [AllowEmptyString()][string]$BaseCommit = '',
        [AllowEmptyString()][string]$CandCommit = ''
    )

    # Invariant culture: the report is pasted into ledgers, whatever the machine's decimal separator.
    $inv = [cultureinfo]::InvariantCulture
    $f = { param($pattern) [string]::Format($inv, $pattern, [object[]]$args) }
    $s = $Summary
    $baseName = if ($BaseCommit) { $BaseCommit } else { 'the baseline' }
    $out = [System.Collections.Generic.List[string]]::new()
    $commits = @(if ($BaseCommit) { "Baseline $BaseCommit" }; if ($CandCommit) { "candidate $CandCommit" })
    if ($commits.Count -gt 0) { $out.Add($commits -join '  ') }
    $out.Add((& $f 'Fixed depth {0}, Threads=1: {1} positions in {2} chunks of {3}, build order alternated per chunk.' $SearchDepth $s.N $ChunkCount $Size))
    $out.Add((& $f 'Candidate/baseline time ratio, mean per position: {0:0.000} +/- {1:0.000} (2 SE), [{2:0.000}, {3:0.000}]' $s.Mean (2 * $s.Se) $s.Low $s.High))
    $out.Add((& $f 'Total: baseline {0:0.0} s, candidate {1:0.0} s' $s.BaselineSeconds $s.CandidateSeconds))
    $out.Add((& $f 'Candidate faster on {0} of {1} positions, slower on {2}' $s.Faster $s.N $s.Slower))
    $firstNote = foreach ($arm in $s.MeanByFirst.Keys) {
        if ($null -ne $s.MeanByFirst[$arm]) { & $f '{0} first {1:0.000}' $arm $s.MeanByFirst[$arm] }
    }
    $out.Add('Mean ratio by the build that ran first: ' + ($firstNote -join ', '))
    if ($s.Short -gt 0) {
        $out.Add((& $f 'WARNING: {0} position(s) searched under {1} ms on either build; timer steps weigh on their ratios. Raise -Depth.' $s.Short $ShortSearchMs))
    }
    $verdict = if (Test-WallClockGate -Summary $s) { 'PASS' } else { 'FAIL' }
    $out.Add((& $f '#636 wall-clock gate, used only when the lab is declined (mean change {0}% or better, +2 SE bound below 0): {1}' $GateChangePct $verdict))
    $out.Add('')
    $out.Add('Ledger: ' + (& $f 'Fixed-depth {0} wall clock over the same {1} positions against {2}, in chunks of {3} with the order alternated: after/before **{4:0.00} +/- {5:0.00}** (mean per-position ratio, 2 SE), {6:0} s -> {7:0} s in total.' `
        $SearchDepth $s.N $baseName $Size $s.Mean (2 * $s.Se) $s.BaselineSeconds $s.CandidateSeconds))
    return $out
}

# ---------------------------------------------------------------------------
# Self-test: the pure halves and the series loop with a fake Run-Bench. No engine runs.
# ---------------------------------------------------------------------------

if ($SelfTest) {
    $failures = 0

    function Assert-Case {
        param([string]$Name, [bool]$Ok, [string]$Detail = '')
        if ($Ok) {
            Write-Host ("  PASS  {0}" -f $Name) -ForegroundColor Green
        } else {
            Write-Host ("  FAIL  {0}{1}" -f $Name, $(if ($Detail) { " -- $Detail" } else { '' })) -ForegroundColor Red
            $script:failures++
        }
    }

    function Test-Refuses {
        param([scriptblock]$Action, [Parameter(Mandatory)][string]$Match)
        try { & $Action | Out-Null; return $false }
        catch { return $_.Exception.Message -match $Match }
    }

    # Schedule: consecutive, complete, alternating.
    $chunkPlan = @(Get-ChunkPlan -Count 25 -Size 10)
    Assert-Case '25 positions in chunks of 10 are 10, 10 and 5 from 0, 10 and 20' `
        (($chunkPlan | ForEach-Object { "$($_.Start)+$($_.Length)" }) -join ' ' -eq '0+10 10+10 20+5')
    Assert-Case 'the first build alternates per chunk, baseline first' `
        (($chunkPlan | ForEach-Object { $_.Order[0] }) -join ' ' -eq 'baseline candidate baseline')
    Assert-Case 'one chunk is still a collection of one' (@(Get-ChunkPlan -Count 4 -Size 10).Count -eq 1)

    # Statistics against a hand computation: ratios 1.1 and 1.3, mean 1.2, SD sqrt(0.02), SE 0.1.
    $twoPairs = @(
        [pscustomobject]@{ First = 'baseline';  BaselineMs = 100; CandidateMs = 110; Ratio = 1.1 }
        [pscustomobject]@{ First = 'candidate'; BaselineMs = 100; CandidateMs = 130; Ratio = 1.3 }
    )
    $sum = Get-WallClockSummary -Pairs $twoPairs
    Assert-Case 'mean ratio and +-2 SE match the hand computation' `
        ([math]::Abs($sum.Mean - 1.2) -lt 1e-12 -and [math]::Abs($sum.Se - 0.1) -lt 1e-12 -and
         [math]::Abs($sum.Low - 1.0) -lt 1e-12 -and [math]::Abs($sum.High - 1.4) -lt 1e-12) `
        "got mean $($sum.Mean) se $($sum.Se)"
    Assert-Case 'totals, slower count, short searches and order split' `
        ($sum.BaselineSeconds -eq 0.2 -and $sum.CandidateSeconds -eq 0.24 -and $sum.Slower -eq 2 -and $sum.Faster -eq 0 -and $sum.Short -eq 0 -and
         [math]::Abs($sum.MeanByFirst['candidate'] - 1.3) -lt 1e-12)
    Assert-Case 'FALSIFY: one position has no standard error' `
        (Test-Refuses -Match 'at least two positions' { Get-WallClockSummary -Pairs @($twoPairs[0]) })

    # The gate.
    foreach ($case in @(
            @{ Mean = 0.95; Se = 0.005; Expect = $true;  Why = '-5%, bound below 0' }
            @{ Mean = 0.97; Se = 0.001; Expect = $true;  Why = 'exactly -3%' }
            @{ Mean = 0.98; Se = 0.001; Expect = $false; Why = 'FALSIFY: -2% is not enough' }
            @{ Mean = 0.95; Se = 0.03;  Expect = $false; Why = 'FALSIFY: -5% whose +2 SE bound is above 0' })) {
        $gateInput = [pscustomobject]@{ Mean = $case.Mean; High = $case.Mean + 2 * $case.Se }
        $got = Test-WallClockGate -Summary $gateInput
        Assert-Case "gate: $($case.Why) -> $(if ($case.Expect) { 'PASS' } else { 'FAIL' })" ($got -eq $case.Expect)
    }

    # Pairing refusals.
    $twoChunk = (Get-ChunkPlan -Count 2 -Size 2)[0]
    $twoFens = @('fen-a', 'fen-b')
    $okRows = @([pscustomobject]@{ Fen = 'fen-a'; Ms = '100'; Nodes = '1' }, [pscustomobject]@{ Fen = 'fen-b'; Ms = '200'; Nodes = '2' })
    $twoJoined = @(Join-ChunkRun -Chunk $twoChunk -Fens $twoFens -Rows @{ baseline = $okRows; candidate = $okRows })
    Assert-Case 'matching rows pair by position' ($twoJoined.Count -eq 2 -and $twoJoined[1].Position -eq 2 -and $twoJoined[1].Ratio -eq 1)
    foreach ($case in @(
            @{ Name = 'a reordered chunk'; Rows = @($okRows[1], $okRows[0]); Match = "returned 'fen-b', expected 'fen-a'" }
            @{ Name = 'a missing position'; Rows = @($okRows[0]); Match = 'returned 1 positions for 2' }
            @{ Name = 'a 0 ms search'; Rows = @($okRows[0], [pscustomobject]@{ Fen = 'fen-b'; Ms = '0'; Nodes = '2' }); Match = 'raise -Depth' })) {
        Assert-Case "FALSIFY: $($case.Name) is refused" `
            (Test-Refuses -Match $case.Match { Join-ChunkRun -Chunk $twoChunk -Fens $twoFens -Rows @{ baseline = $okRows; candidate = $case.Rows } })
    }

    # The series loop, end to end through real CSV files, with a fake Run-Bench whose candidate
    # takes 1.2x the baseline's time.
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) "compare-wallclock-selftest-$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Path $tmp -Force | Out-Null
    try {
        $list = @(for ($i = 1; $i -le 25; $i++) { @{ Name = "pos-$i"; Fen = "fen-$i" } })
        $calls = [System.Collections.Generic.List[string]]::new()
        $fakeBench = {
            param($arm, $fenFile, $csv)
            $calls.Add("$arm $(Split-Path -Leaf $fenFile)")
            $factor = if ($arm -eq 'candidate') { 1.2 } else { 1.0 }
            @(Get-Content -LiteralPath $fenFile) | ForEach-Object {
                [pscustomobject]@{ Position = 'x'; Fen = $_; Nodes = 5; Ms = [int64](1000 * $factor * [int]($_ -replace '\D', '')) }
            } | Export-Csv -LiteralPath $csv -NoTypeInformation
        }.GetNewClosure()
        $series = @(Invoke-WallClockSeries -Plan @(Get-ChunkPlan -Count 25 -Size 10) -PositionSet $list -RunChunk $fakeBench -WorkDir $tmp)
        Assert-Case 'every position is paired once, in file order' `
            ($series.Count -eq 25 -and (($series | ForEach-Object { $_.Position }) -join ',') -eq ((1..25) -join ',') -and
             $series[24].Fen -eq 'fen-25' -and $series[24].Chunk -eq 3)
        Assert-Case 'the runner ran each chunk once per build, in the alternated order' `
            (($calls -join '; ') -eq 'baseline chunk001.fen; candidate chunk001.fen; candidate chunk002.fen; baseline chunk002.fen; baseline chunk003.fen; candidate chunk003.fen') `
            ($calls -join '; ')
        $sum = Get-WallClockSummary -Pairs $series
        Assert-Case 'a 1.2x slower candidate measures 1.2 with no spread' `
            ([math]::Abs($sum.Mean - 1.2) -lt 1e-9 -and $sum.Se -lt 1e-9 -and $sum.Slower -eq 25)

        $shuffled = {
            param($arm, $fenFile, $csv)
            $lines = @(Get-Content -LiteralPath $fenFile)
            if ($arm -eq 'candidate') { [array]::Reverse($lines) }
            $lines | ForEach-Object { [pscustomobject]@{ Fen = $_; Nodes = 5; Ms = 100 } } | Export-Csv -LiteralPath $csv -NoTypeInformation
        }
        $refusedDir = Join-Path $tmp 'refused'
        New-Item -ItemType Directory -Path $refusedDir | Out-Null
        Assert-Case 'FALSIFY: a build that answers in another order is refused, not paired' `
            (Test-Refuses -Match "chunk 1, row 1: the candidate build returned 'fen-10'" {
                Invoke-WallClockSeries -Plan @(Get-ChunkPlan -Count 25 -Size 10) -PositionSet $list -RunChunk $shuffled -WorkDir $refusedDir })
    } finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }

    # The report, under a comma-decimal culture: what is pasted must not depend on the machine.
    $prior = [cultureinfo]::CurrentCulture
    try {
        [cultureinfo]::CurrentCulture = [cultureinfo]::GetCultureInfo('de-DE')
        $result = [pscustomobject]@{
            N = 200; Mean = 1.24; Se = 0.035; Low = 1.17; High = 1.31; BaselineSeconds = 483.2; CandidateSeconds = 548.4
            Faster = 40; Slower = 158; Short = 0; MeanByFirst = [ordered]@{ baseline = 1.23; candidate = 1.25 }
        }
        $report = @(Format-WallClockReport -Summary $result -SearchDepth 14 -Size 10 -ChunkCount 20 -BaseCommit 'f9fcf8a' -CandCommit '6528d36')
    } finally {
        [cultureinfo]::CurrentCulture = $prior
    }
    $text = $report -join "`n"
    Assert-Case 'the ledger line reads as the recorded rows do' `
        ($text -match [regex]::Escape('against f9fcf8a, in chunks of 10 with the order alternated: after/before **1.24 +/- 0.07** (mean per-position ratio, 2 SE), 483 s -> 548 s in total.')) `
        (@($report | Where-Object { $_ -like 'Ledger:*' }) -join '')
    Assert-Case 'FALSIFY: a comma-decimal machine still prints decimal points' `
        ($text -match '1\.240 \+/- 0\.070' -and $text -notmatch '\d,\d')
    Assert-Case 'a slower candidate fails the gate in the report' ($text -match 'lab is declined .*: FAIL')
    Assert-Case 'no short searches, no warning' ($text -notmatch 'WARNING')
    $result.Short = 3
    $warned = @(Format-WallClockReport -Summary $result -SearchDepth 14 -Size 10 -ChunkCount 20) -join "`n"
    Assert-Case 'FALSIFY: short searches are named in the report' ($warned -match 'WARNING: 3 position\(s\) searched under 100 ms')

    Write-Host ''
    if ($failures -gt 0) {
        Write-Host "$failures self-test case(s) FAILED." -ForegroundColor Red
        exit 1
    }
    Write-Host 'All self-test cases passed.' -ForegroundColor Green
    exit 0
}

# --- main ------------------------------------------------------------------

if (-not $Baseline -or -not $Candidate) { throw '-Baseline and -Candidate are both required.' }
$baselinePath = (Resolve-Path -LiteralPath $Baseline).Path
$candidatePath = (Resolve-Path -LiteralPath $Candidate).Path
$exes = @{ baseline = $baselinePath; candidate = $candidatePath }
$runBench = Join-Path $PSScriptRoot 'Run-Bench.ps1'

$positionFile = if ($Positions) { (Resolve-Path -LiteralPath $Positions).Path } else { Join-Path (Split-Path $PSScriptRoot -Parent) 'Tests\profile-screen.fen' }
$positionList = @(Resolve-Positions -Path $positionFile)
if ($positionList.Count -lt 2) { throw "$positionFile holds $($positionList.Count) position; a standard error needs at least two." }
$chunkPlan = @(Get-ChunkPlan -Count $positionList.Count -Size $ChunkSize)

if (-not $OutDir) { $OutDir = Join-Path ([System.IO.Path]::GetTempPath()) "compare-wallclock-$(Get-Date -Format yyyyMMdd-HHmmss)" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$outPath = (Resolve-Path -LiteralPath $OutDir).Path
if (@(Get-ChildItem -LiteralPath $outPath -Filter '*.csv' -File).Count -gt 0) {
    throw "$outPath already holds CSVs; use a fresh -OutDir so two series cannot mix."
}

[ordered]@{
    Started     = (Get-Date).ToString('o')
    Host        = [Environment]::MachineName
    Exes        = $exes
    Commits     = @{ baseline = $BaselineCommit; candidate = $CandidateCommit }
    Sha256      = @{ baseline = (Get-FileHash -LiteralPath $baselinePath -Algorithm SHA256).Hash.ToLower()
                     candidate = (Get-FileHash -LiteralPath $candidatePath -Algorithm SHA256).Hash.ToLower() }
    Depth       = $Depth
    Threads     = 1
    PositionSet = $positionFile
    PositionSha = Get-PositionSetHash -List $positionList
    ChunkSize   = $ChunkSize
    Schedule    = @($chunkPlan | ForEach-Object { $_.Order -join ',' })
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $outPath 'metadata.json')

if (-not $BaselineCommit) {
    Write-Host 'NOTE: no -BaselineCommit; the ledger line will not name the merge base.' -ForegroundColor Yellow
}
# Each ply roughly doubles a search; rough either way, and replaced by a measurement after chunk 1.
$searchCount = 2 * $positionList.Count
$estimate = $RoughSearchSeconds * [math]::Pow(2, $Depth - 14) * $searchCount
$phaseName = "fixed-depth searches ($($positionList.Count) positions x 2 builds, depth $Depth)"
Write-Host "Output: $outPath" -ForegroundColor Cyan
Write-PhasePlan -Phase @(@{ Name = $phaseName; Quiet = $true; Seconds = $estimate; Rough = $true })
Write-PhaseBanner -Name $phaseName -Quiet $true -Seconds $estimate -Rough $true

$benchChunk = {
    param($arm, $fenFile, $csv)
    # Run-Bench's table goes to the information stream; the CSV is the record. Its 0 ms check is ours.
    & $runBench -Exe $exes[$arm] -Depth $Depth -Threads 1 -Positions $fenFile -MinTimeMs 0 -Csv $csv 6>$null | Out-Null
}
$onChunk = {
    param($done, $total, $seconds)
    if ($done -lt $total) {
        $left = $seconds / $done * ($total - $done)
        Write-PhaseBanner -Name "$phaseName, chunk $done of $total done" -Quiet $true -Seconds $left
    }
}
$paired = @(Invoke-WallClockSeries -Plan $chunkPlan -PositionSet $positionList -RunChunk $benchChunk -WorkDir $outPath -Progress $onChunk)
Write-PhaseBanner -Name 'report' -Quiet $false -Seconds 1

$paired | Export-Csv -LiteralPath (Join-Path $outPath 'pairs.csv') -NoTypeInformation
$result = Get-WallClockSummary -Pairs $paired
$report = @(Format-WallClockReport -Summary $result -SearchDepth $Depth -Size $ChunkSize -ChunkCount $chunkPlan.Count `
    -BaseCommit $BaselineCommit -CandCommit $CandidateCommit)
$report | Set-Content -LiteralPath (Join-Path $outPath 'report.txt')
Write-Host ''
$report | ForEach-Object { Write-Host $_ }
Write-Host ''
Write-Host "Chunk CSVs, pairs.csv, metadata.json and report.txt: $outPath"
