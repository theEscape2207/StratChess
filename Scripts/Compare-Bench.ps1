<#
.SYNOPSIS
    Paired nps comparison of two engine builds: a balanced, validated Run-Bench series with a
    verdict of Speedup, No slowdown, Slowdown or Unresolved.

.DESCRIPTION
    Runs Run-Bench.ps1 over the same positions for each arm, once per arm per round, in a
    fixed balanced order, and keeps every CSV. The arms are the baseline, the candidate and,
    with -Control, a byte-identical copy of the baseline. A copy of the baseline measures how
    much timing noise the machine produces when nothing has changed.

    Schedule. Round 0 runs each arm once and is discarded as warm-up. The -Rounds measured
    rounds then alternate the run order. Two arms alternate baseline-first and
    candidate-first. Three arms rotate all six permutations. -Rounds must be a multiple of
    the number of orders, so every arm runs in every slot equally often.

    Validity. Every run must report the same positions, main/qs/total nodes and best move as
    the first run, and every position must take at least -MinTimeMs. Any mismatch, short
    search or failed run rejects the whole comparison: its nps would not compare like with
    like.

    Statistics. The sampling unit is the round, not the position. Each round gives one
    aggregate delta, computed from total nodes / total ms over the suite. That is valid only
    because the node counts were checked identical. The interval is a two-sided 95% Student-t
    interval over the kept rounds. Above 30 degrees of freedom it uses the critical value of
    the next lower table row, which errs wide.
    With -Control, the verdict compares the candidate against the mean of both baseline arms.

    Verdict, from the 95% interval of the verdict comparison, checked in this order:
      Speedup      lower bound > 0, only with -Control and only while the whole A/A interval
                   lies within +-0.5%: a control that is wide or offset cannot show the
                   machine was quiet. Pending until a rerun on a New-OrderedBuildPair.ps1 pair
                   (shared /ORDER) agrees: placement alone moves node-identical builds by
                   several percent either way.
      No slowdown  lower bound >= -0.5%; a small cost inside that tolerance still passes, and
                   the printed interval shows it.
      Slowdown     upper bound < 0.
      Unresolved   anything else. Rerun with -Control or relink both builds with
                   New-OrderedBuildPair.ps1 (measure-strength regression-check). Do not add
                   rounds to the same series until it passes.
    Any other positive delta is unconfirmed: timing noise and placement are not ruled out.
    No verdict is an Elo claim.

    -TrendOnly replaces the verdict with the interval alone, for builds whose code placement
    cannot be controlled (GCC, which has no ordered pair): every number is still reported.

    Build both executables the same way (measure-strength regression-check). Run on a quiet
    machine: finish builds and reviews first, because spare cores do not mean the machine is
    idle. A run is (Rounds + 1) suite passes per arm.

.PARAMETER Baseline
    Baseline StratChessEvolved.exe, normally built from the merge base.

.PARAMETER Candidate
    Candidate StratChessEvolved.exe. It may be the baseline's path, for an A/A check.

.PARAMETER Rounds
    Measured rounds, after the warm-up round. Default 12. Fix it before the run starts.

.PARAMETER Control
    Add a byte-identical copy of the baseline as a third arm, an A/A noise control.

.PARAMETER Depth
    Fixed search depth, passed to Run-Bench. Default 13, which clears -MinTimeMs on every
    built-in position on the dev machine.

.PARAMETER Positions
    Optional FEN file, passed to Run-Bench. Defaults to its built-in set.

.PARAMETER Affinity
    Optional processor affinity mask (for example 4 = logical processor 2). It applies to this
    process, and every engine process inherits it. Default 0 leaves affinity unchanged.

.PARAMETER MinTimeMs
    Shortest search per position that may be timed. Default 200.

.PARAMETER BaselineCommit
    Optional commit the baseline was built from, recorded in metadata.json and the report.

.PARAMETER CandidateCommit
    Optional commit the candidate was built from, recorded likewise.

.PARAMETER TrendOnly
    Report the verdict comparison's interval as a trend instead of a verdict.

.PARAMETER OutDir
    Where to keep the CSVs, metadata.json and report.txt. Defaults to a new directory under the
    system temp path. Must not already contain CSVs.

.PARAMETER SelfTest
    Assert the schedule, validation, aggregation, interval and verdict on synthetic data and
    exit. Runs no engine. Exits 1 on any failure.

.EXAMPLE
    .\Compare-Bench.ps1 -Baseline ..\base\build\windows-clang-cl\StratChessEvolved.exe `
                        -Candidate .\build\windows-clang-cl\StratChessEvolved.exe

.EXAMPLE
    .\Compare-Bench.ps1 -Baseline $b -Candidate $c -Control -Rounds 60 -Affinity 4
#>
[CmdletBinding()]
param(
    [string]$Baseline = '',

    [string]$Candidate = '',

    [ValidateRange(1, 1000)]
    [int]$Rounds = 12,

    [switch]$Control,

    [ValidateRange(1, 30)]
    [int]$Depth = 13,

    [string]$Positions = '',

    [int64]$Affinity = 0,

    [int]$MinTimeMs = 200,

    [string]$BaselineCommit = '',

    [string]$CandidateCommit = '',

    [string]$OutDir = '',

    [switch]$TrendOnly,

    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# The largest slowdown, in percent, that still counts as No slowdown. Fixed so it cannot move after a run.
$SlowdownTolerancePct = 0.5

# $DefaultPositions, Resolve-Positions and Get-PositionSetHash: the set Run-Bench measures.
. (Join-Path $PSScriptRoot 'BenchPositions.ps1')

function Get-Schedule {
    <# Arm order per measured round: each arm in each slot equally often. #>
    param([Parameter(Mandatory)][string[]]$Arms, [Parameter(Mandatory)][int]$RoundCount)

    $orders = if ($Arms.Count -eq 2) {
        @(@($Arms[0], $Arms[1]), @($Arms[1], $Arms[0]))
    } else {
        $a, $b, $c = $Arms
        @(@($a, $b, $c), @($b, $c, $a), @($c, $a, $b), @($a, $c, $b), @($b, $a, $c), @($c, $b, $a))
    }
    if ($RoundCount % $orders.Count -ne 0) {
        throw "-Rounds $RoundCount is not a multiple of $($orders.Count), so the run order would be unbalanced."
    }
    $schedule = for ($r = 0; $r -lt $RoundCount; $r++) { , $orders[$r % $orders.Count] }
    return $schedule
}

function Get-RunSignature {
    <# What must be identical in every run: positions, node counts and best moves. #>
    param([Parameter(Mandatory)][object[]]$Rows)
    ($Rows | ForEach-Object { '{0}|{1}|{2}|{3}|{4}|{5}' -f $_.Position, $_.Fen, $_.MainNodes, $_.QsNodes, $_.Nodes, $_.Best }) -join "`n"
}

function Assert-ComparableRuns {
    <# Throws, naming the run, unless every run matches the first run's signature and clears the floor. #>
    param([Parameter(Mandatory)][object[]]$Runs, [Parameter(Mandatory)][int]$FloorMs)

    $reference = Get-RunSignature -Rows $Runs[0].Rows
    foreach ($run in $Runs) {
        $label = "round $($run.Round) $($run.Arm)"
        if ((Get-RunSignature -Rows $run.Rows) -ne $reference) {
            throw "Run $label differs from round $($Runs[0].Round) $($Runs[0].Arm) in positions, nodes or best moves; the builds do not search the same tree, so the comparison is rejected."
        }
        foreach ($row in $run.Rows) {
            if ([int64]$row.Ms -lt $FloorMs) {
                throw "Run $label searched $($row.Position) in $($row.Ms) ms, under the $FloorMs ms floor; raise -Depth."
            }
        }
    }
}

function Get-AggregateNps {
    param([Parameter(Mandatory)][object[]]$Rows)
    $nodes = [double]0; $ms = [double]0
    foreach ($row in $Rows) { $nodes += [int64]$row.Nodes; $ms += [int64]$row.Ms }
    return $nodes * 1000 / $ms
}

function Get-Interval {
    <# Mean, sample SD, median, range and a two-sided 95% Student-t interval. #>
    param([Parameter(Mandatory)][double[]]$Values)

    $n = $Values.Count
    if ($n -lt 2) { throw "An interval needs at least two values, got $n." }
    $mean = ($Values | Measure-Object -Average).Average
    $ss = 0.0
    foreach ($v in $Values) { $ss += ($v - $mean) * ($v - $mean) }
    $sd = [math]::Sqrt($ss / ($n - 1))
    # t(0.975, df) for df 1..30; above that, the next lower tabled row, which errs wide.
    $t = @(12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228,
           2.201, 2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086,
           2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042)
    $df = $n - 1
    $crit = if ($df -le 30) { $t[$df - 1] } elseif ($df -le 40) { 2.042 } elseif ($df -le 60) { 2.021 } elseif ($df -le 120) { 2.000 } else { 1.980 }
    $half = $crit * $sd / [math]::Sqrt($n)
    $sorted = @($Values | Sort-Object)
    $mid = [math]::Floor($n / 2)
    $median = if ($n % 2) { $sorted[$mid] } else { ($sorted[$mid - 1] + $sorted[$mid]) / 2 }
    return [pscustomobject]@{
        N = $n; Mean = $mean; Sd = $sd; Median = $median
        Min = $sorted[0]; Max = $sorted[$n - 1]; Low = $mean - $half; High = $mean + $half
    }
}

function Get-Verdict {
    <# $AaInterval is the control-vs-baseline interval, or $null without -Control: no speedup without it.
       Overlapping zero is not enough: the A/A interval must sit inside the tolerance band on both sides. #>
    param([Parameter(Mandatory)][object]$Interval, [Parameter(Mandatory)][double]$TolerancePct, [object]$AaInterval = $null)
    $quietMachine = $null -ne $AaInterval -and $AaInterval.Low -ge -$TolerancePct -and $AaInterval.High -le $TolerancePct
    if ($Interval.Low -gt 0 -and $quietMachine) { return 'Speedup' }
    if ($Interval.Low -ge -$TolerancePct) { return 'No slowdown' }
    if ($Interval.High -lt 0) { return 'Slowdown' }
    return 'Unresolved'
}

function Get-Comparison {
    <#
        Turns the runs into the report's numbers. Round 0 is the warm-up and is excluded.
        Runs carry Round, Slot, Arm ('baseline', 'candidate', 'control') and Rows.
    #>
    param([Parameter(Mandatory)][object[]]$Runs, [Parameter(Mandatory)][double]$TolerancePct)

    $kept = @($Runs | Where-Object { $_.Round -ge 1 })
    # String keys: an [int] index into an ordered dictionary is positional, not a key lookup.
    $byRound = [ordered]@{}
    foreach ($run in $kept) {
        $key = [string]$run.Round
        if (-not $byRound.Contains($key)) { $byRound[$key] = @{} }
        $byRound[$key][$run.Arm] = $run
    }
    $hasControl = @($kept | Where-Object { $_.Arm -eq 'control' }).Count -gt 0
    $pct = { param($a, $b) 100.0 * ($a / $b - 1) }

    # One delta per round per comparison; 'reference' is what the candidate is judged against.
    $pairs = [ordered]@{ 'candidate vs baseline' = @('candidate', 'baseline') }
    if ($hasControl) {
        $pairs['candidate vs control'] = @('candidate', 'control')
        $pairs['control vs baseline (A/A)'] = @('control', 'baseline')
    }
    $comparisons = [ordered]@{}
    foreach ($name in $pairs.Keys) {
        $x, $y = $pairs[$name]
        $deltas = foreach ($r in $byRound.Values) { & $pct (Get-AggregateNps $r[$x].Rows) (Get-AggregateNps $r[$y].Rows) }
        $comparisons[$name] = Get-Interval -Values $deltas
    }
    # The verdict judges the candidate against the baseline, or against mean(baseline, control).
    # $select picks which of a run's rows to aggregate: all of them, or one position's.
    $candidateDelta = {
        param($r, $select)
        $ref = Get-AggregateNps (& $select $r['baseline'])
        if ($hasControl) { $ref = ($ref + (Get-AggregateNps (& $select $r['control']))) / 2 }
        & $pct (Get-AggregateNps (& $select $r['candidate'])) $ref
    }
    $verdictName = if ($hasControl) { 'candidate vs mean(baseline, control)' } else { 'candidate vs baseline' }
    $roundDeltas = @(foreach ($r in $byRound.Values) { & $candidateDelta $r { param($run) $run.Rows } })
    if ($hasControl) { $comparisons[$verdictName] = Get-Interval -Values $roundDeltas }

    # Per position, against the same reference, so an outlier position stays visible.
    $perPosition = [ordered]@{}
    foreach ($position in @($kept[0].Rows | ForEach-Object { $_.Position })) {
        $deltas = foreach ($r in $byRound.Values) {
            & $candidateDelta $r { param($run) @($run.Rows | Where-Object { $_.Position -eq $position }) }
        }
        $perPosition[$position] = Get-Interval -Values $deltas
    }

    # Slot effect: every arm's aggregate nps by run slot, against the overall mean.
    $all = @($kept | ForEach-Object { Get-AggregateNps $_.Rows })
    $overall = ($all | Measure-Object -Average).Average
    $slots = [ordered]@{}
    foreach ($group in ($kept | Group-Object Slot | Sort-Object { [int]$_.Name })) {
        $slotMean = (@($group.Group | ForEach-Object { Get-AggregateNps $_.Rows }) | Measure-Object -Average).Average
        $slots["slot $($group.Name)"] = & $pct $slotMean $overall
    }

    # Each arm's absolute speed: the median of its per-round aggregate nps.
    $armNps = [ordered]@{}
    foreach ($arm in @('baseline', 'candidate', 'control')) {
        $values = @(foreach ($r in $byRound.Values) { if ($r.ContainsKey($arm)) { Get-AggregateNps $r[$arm].Rows } })
        if ($values.Count -gt 0) { $armNps[$arm] = (Get-Interval -Values $values).Median }
    }

    $verdictInterval = $comparisons[$verdictName]
    $aaInterval = if ($hasControl) { $comparisons['control vs baseline (A/A)'] } else { $null }
    return [pscustomobject]@{
        Rounds       = $byRound.Count
        Comparisons  = $comparisons
        VerdictBasis = $verdictName
        RoundDeltas  = $roundDeltas
        Verdict      = Get-Verdict -Interval $verdictInterval -TolerancePct $TolerancePct -AaInterval $aaInterval
        PerPosition  = $perPosition
        Slots        = $slots
        ArmNps       = $armNps
    }
}

function Format-Report {
    param([Parameter(Mandatory)][object]$Result, [Parameter(Mandatory)][double]$TolerancePct, [switch]$TrendOnly)

    $line = '{0,-38} {1,8:+0.00;-0.00;0.00}% {2,6:0.00} [{3,7:+0.00;-0.00;0.00}%, {4,7:+0.00;-0.00;0.00}%] {5,8:+0.00;-0.00;0.00}%  {6,7:+0.00;-0.00;0.00}% .. {7:+0.00;-0.00;0.00}%'
    # Invariant culture: the report is pasted into PRs, whatever the machine's decimal separator.
    $inv = [cultureinfo]::InvariantCulture
    # Rounded first: a tiny negative would otherwise take the negative section and print as '-+0.00'.
    $fmt = { param($pattern) [string]::Format($inv, $pattern, @($args | ForEach-Object { if ($_ -is [double]) { [math]::Round($_, 2) } else { $_ } })) }
    $out = [System.Collections.Generic.List[string]]::new()
    $out.Add("Kept rounds: $($Result.Rounds) (round 0 discarded as warm-up). Nodes and best moves identical in every run.")
    $out.Add('')
    $out.Add(('{0,-38} {1,9} {2,6} {3,20} {4,9}  {5}' -f 'comparison (aggregate nps, per round)', 'mean', 'sd', '95% t-interval', 'median', 'range'))
    foreach ($name in $Result.Comparisons.Keys) {
        $c = $Result.Comparisons[$name]
        $out.Add((& $fmt $line $name $c.Mean $c.Sd $c.Low $c.High $c.Median $c.Min $c.Max))
    }
    $out.Add("Per-round deltas, $($Result.VerdictBasis): " +
             (($Result.RoundDeltas | ForEach-Object { & $fmt '{0:+0.00;-0.00;0.00}%' $_ }) -join ' '))
    $out.Add('')
    $out.Add("Per position, candidate vs the verdict's reference:")
    foreach ($name in $Result.PerPosition.Keys) {
        $c = $Result.PerPosition[$name]
        $out.Add((& $fmt $line "  $name" $c.Mean $c.Sd $c.Low $c.High $c.Median $c.Min $c.Max))
    }
    $out.Add('')
    $out.Add('Run-order effect, aggregate nps by slot vs the overall mean: ' +
             (($Result.Slots.Keys | ForEach-Object { & $fmt '{0} {1:+0.00;-0.00;0.00}%' $_ $Result.Slots[$_] }) -join ', '))
    $out.Add('Aggregate nps per arm, median over kept rounds: ' +
             (($Result.ArmNps.Keys | ForEach-Object { & $fmt '{0} {1:N0}' $_ $Result.ArmNps[$_] }) -join ', '))
    $out.Add('')
    if ($TrendOnly) {
        $c = $Result.Comparisons[$Result.VerdictBasis]
        $out.Add((& $fmt "TREND: {0:+0.00;-0.00;0.00}% [{1:+0.00;-0.00;0.00}%, {2:+0.00;-0.00;0.00}%] ('$($Result.VerdictBasis)'); no verdict without placement control. Not Elo." $c.Mean $c.Low $c.High))
        return $out
    }
    $out.Add("VERDICT: $($Result.Verdict)  (from '$($Result.VerdictBasis)'; tolerance -$TolerancePct%)")
    switch ($Result.Verdict) {
        'Speedup'     { $out.Add('PENDING: relink both builds with New-OrderedBuildPair.ps1 and rerun with -Control; claim it only if Speedup holds there too. Not Elo.') }
        'No slowdown' { $out.Add('A positive delta here is no confirmed speedup: timing noise and placement are not ruled out. A speedup claim needs -Control and a Speedup verdict. Not Elo.') }
        'Slowdown'    { $out.Add('Relink both builds with New-OrderedBuildPair.ps1 before treating it as real (measure-strength regression-check).') }
        'Unresolved'  { $out.Add('Rerun with -Control, or relink with New-OrderedBuildPair.ps1. Do not extend this series until it passes.') }
    }
    return $out
}

# ---------------------------------------------------------------------------
# Self-test: the pure halves. Running Run-Bench itself is not covered.
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

    function Test-Refuses {
        param([scriptblock]$Action, [Parameter(Mandatory)][string]$Match)
        try { & $Action | Out-Null; return $false }
        catch { return $_.Exception.Message -match $Match }
    }

    function New-Rows {
        <# Two positions; $Ms scales the time so nps varies while the nodes stay fixed. #>
        param([double]$MsA = 1000, [double]$MsB = 1000, [string]$BestB = 'g1f3', [int64]$NodesB = 2000000)
        @(
            [pscustomobject]@{ Position = 'a'; Fen = 'fa'; MainNodes = '700000'; QsNodes = '300000'; Nodes = '1000000'; Ms = [string][int64]$MsA; Best = 'e2e4' }
            [pscustomobject]@{ Position = 'b'; Fen = 'fb'; MainNodes = '1500000'; QsNodes = '500000'; Nodes = [string]$NodesB; Ms = [string][int64]$MsB; Best = $BestB }
        )
    }

    function New-Series {
        <# Runs following the real schedule; $Speed maps arm -> nps multiplier, $Round0 the warm-up baseline's.
           $Jitter maps round -> a multiplier common to all arms; $ArmJitter maps round -> @{ arm = multiplier }. #>
        param([string[]]$Arms, [int]$RoundCount, [hashtable]$Speed, [hashtable]$Jitter = @{}, [hashtable]$ArmJitter = @{}, [double]$Round0 = 1.0)
        $series = [System.Collections.Generic.List[object]]::new()
        $slot = 0
        foreach ($arm in $Arms) {
            $ms = if ($arm -eq 'baseline') { 100000 / $Round0 } else { 100000 }
            $series.Add([pscustomobject]@{ Round = 0; Slot = $slot++; Arm = $arm; Rows = (New-Rows -MsA $ms -MsB $ms) })
        }
        $schedule = @(Get-Schedule -Arms $Arms -RoundCount $RoundCount)
        for ($r = 1; $r -le $RoundCount; $r++) {
            $slot = 0
            foreach ($arm in $schedule[$r - 1]) {
                $j = if ($Jitter.ContainsKey($r)) { $Jitter[$r] } else { 1.0 }
                if ($ArmJitter.ContainsKey($r) -and $ArmJitter[$r].ContainsKey($arm)) { $j *= $ArmJitter[$r][$arm] }
                $ms = 100000 / ($Speed[$arm] * $j)
                $series.Add([pscustomobject]@{ Round = $r; Slot = $slot++; Arm = $arm; Rows = (New-Rows -MsA $ms -MsB $ms) })
            }
        }
        return $series
    }

    # Schedule balance: every arm in every slot equally often.
    foreach ($case in @(@{ Arms = @('baseline', 'candidate'); Rounds = 12; Per = 6 }, @{ Arms = @('baseline', 'candidate', 'control'); Rounds = 12; Per = 4 })) {
        $schedule = @(Get-Schedule -Arms $case.Arms -RoundCount $case.Rounds)
        $balanced = $schedule.Count -eq $case.Rounds
        foreach ($arm in $case.Arms) {
            for ($s = 0; $s -lt $case.Arms.Count; $s++) {
                $count = @($schedule | Where-Object { $_[$s] -eq $arm }).Count
                if ($count -ne $case.Per) { $balanced = $false }
            }
        }
        Assert-Case "$($case.Arms.Count)-arm schedule puts every arm in every slot $($case.Per) times" $balanced
    }
    Assert-Case 'FALSIFY: an unbalanced round count is refused' `
        (Test-Refuses -Match 'not a multiple of 6' { Get-Schedule -Arms @('a', 'b', 'c') -RoundCount 8 })

    # Aggregation is total nodes / total ms, not a mean of per-position nps.
    $agg = Get-AggregateNps -Rows (New-Rows -MsA 1000 -MsB 3000)
    Assert-Case 'aggregate nps is total nodes over total time' ([math]::Abs($agg - 750000) -lt 1e-6) "got $agg"

    # The interval against a hand-computed case: mean 3, sd sqrt(2.5), t(4) 2.776.
    $iv = Get-Interval -Values @(1, 2, 3, 4, 5)
    Assert-Case 't-interval matches the hand computation' `
        ([math]::Abs($iv.Mean - 3) -lt 1e-9 -and [math]::Abs($iv.Sd - [math]::Sqrt(2.5)) -lt 1e-9 -and
         [math]::Abs($iv.High - (3 + 2.776 * [math]::Sqrt(2.5) / [math]::Sqrt(5))) -lt 1e-9 -and $iv.Median -eq 3) `
        "got mean $($iv.Mean) sd $($iv.Sd) high $($iv.High) median $($iv.Median)"
    Assert-Case 'even-count median averages the middle pair' ((Get-Interval -Values @(4, 1, 3, 2)).Median -eq 2.5)

    $flatAa = [pscustomobject]@{ Low = -0.3; High = 0.4 }
    $driftAa = [pscustomobject]@{ Low = 0.6; High = 0.9 }
    $wideAa = [pscustomobject]@{ Low = -20; High = 20 }
    $preciseAa = [pscustomobject]@{ Low = 0.001; High = 0.002 }
    foreach ($case in @(
            @{ Low = 0.2; High = 1.0; Aa = $flatAa; Expect = 'Speedup' }
            @{ Low = 0.2; High = 1.0; Aa = $null; Expect = 'No slowdown' }
            @{ Low = 0.2; High = 1.0; Aa = $driftAa; Expect = 'No slowdown' }
            @{ Low = 0.2; High = 1.0; Aa = $wideAa; Expect = 'No slowdown' }
            @{ Low = 0.2; High = 1.0; Aa = $preciseAa; Expect = 'Speedup' }
            @{ Low = -0.1; High = 1.0; Aa = $flatAa; Expect = 'No slowdown' }
            @{ Low = -0.3; High = 0.4; Expect = 'No slowdown' }
            @{ Low = -0.45; High = -0.1; Expect = 'No slowdown' }
            @{ Low = -1.5; High = -0.2; Expect = 'Slowdown' }
            @{ Low = -1.0; High = 0.5; Expect = 'Unresolved' })) {
        $aa = if ($case.ContainsKey('Aa')) { $case.Aa } else { $null }
        $got = Get-Verdict -Interval ([pscustomobject]@{ Low = $case.Low; High = $case.High }) -TolerancePct 0.5 -AaInterval $aa
        $aaLabel = if ($null -eq $aa) { 'no control' } else { "A/A [$($aa.Low), $($aa.High)]" }
        Assert-Case "verdict [$($case.Low), $($case.High)], $aaLabel -> $($case.Expect)" ($got -eq $case.Expect) "got $got"
    }

    # End to end over synthetic runs, with jitter so the interval has width.
    $jitter = @{ 1 = 1.004; 2 = 0.996; 3 = 1.002; 4 = 0.998; 5 = 1.001; 6 = 0.999 }
    $runs = New-Series -Arms @('baseline', 'candidate') -RoundCount 12 -Speed @{ baseline = 1.0; candidate = 0.97 } -Jitter $jitter
    $res = Get-Comparison -Runs $runs -TolerancePct 0.5
    Assert-Case 'a 3% slower candidate is a Slowdown near -3%' `
        ($res.Verdict -eq 'Slowdown' -and [math]::Abs($res.Comparisons['candidate vs baseline'].Mean + 3) -lt 0.01 -and $res.Rounds -eq 12) `
        "got $($res.Verdict) mean $($res.Comparisons['candidate vs baseline'].Mean)"

    $runs = New-Series -Arms @('baseline', 'candidate') -RoundCount 12 -Speed @{ baseline = 1.0; candidate = 1.0 } -Jitter $jitter -Round0 0.5
    $res = Get-Comparison -Runs $runs -TolerancePct 0.5
    Assert-Case 'a slow warm-up round is excluded from the result' `
        ($res.Verdict -eq 'No slowdown' -and [math]::Abs($res.Comparisons['candidate vs baseline'].Mean) -lt 1e-9) `
        "got $($res.Verdict) mean $($res.Comparisons['candidate vs baseline'].Mean)"

    $runs = New-Series -Arms @('baseline', 'candidate', 'control') -RoundCount 12 -Speed @{ baseline = 1.0; candidate = 1.0; control = 1.0 } -Jitter $jitter
    $res = Get-Comparison -Runs $runs -TolerancePct 0.5
    Assert-Case 'with a control the verdict is judged against mean(baseline, control)' `
        ($res.VerdictBasis -eq 'candidate vs mean(baseline, control)' -and $res.Comparisons.Contains('control vs baseline (A/A)') -and
         $res.PerPosition.Count -eq 2 -and $res.Slots.Count -eq 3)
    $runs = New-Series -Arms @('baseline', 'candidate', 'control') -RoundCount 12 -Speed @{ baseline = 1.0; candidate = 1.03; control = 1.0 } -Jitter $jitter
    $res = Get-Comparison -Runs $runs -TolerancePct 0.5
    Assert-Case 'a 3% faster candidate with a quiet control is a pending Speedup' `
        ($res.Verdict -eq 'Speedup' -and @(Format-Report -Result $res -TolerancePct 0.5 | Where-Object { $_ -match '^PENDING: relink' }).Count -eq 1) `
        "got $($res.Verdict)"
    $runs = New-Series -Arms @('baseline', 'candidate') -RoundCount 12 -Speed @{ baseline = 1.0; candidate = 1.03 } -Jitter $jitter
    $res = Get-Comparison -Runs $runs -TolerancePct 0.5
    Assert-Case 'the same speedup without -Control is only No slowdown' ($res.Verdict -eq 'No slowdown') "got $($res.Verdict)"

    # Differential noise: per-arm timing that does not cancel in the ratios.
    $opposed = @{}
    for ($r = 1; $r -le 12; $r++) {
        $m = if ($r % 2) { 1.1 } else { 0.9 }
        $opposed[$r] = @{ baseline = $m; control = 2.0 - $m }
    }
    $runs = New-Series -Arms @('baseline', 'candidate', 'control') -RoundCount 12 -Speed @{ baseline = 1.0; candidate = 1.01; control = 1.0 } -ArmJitter $opposed
    $res = Get-Comparison -Runs $runs -TolerancePct 0.5
    $aa = $res.Comparisons['control vs baseline (A/A)']
    Assert-Case 'FALSIFY: a wide control whose arms cancel in the mean blocks Speedup' `
        ($res.Verdict -ne 'Speedup' -and $aa.High - $aa.Low -gt 10) "got $($res.Verdict), A/A [$($aa.Low), $($aa.High)]"
    $noisy = @{}
    for ($r = 1; $r -le 12; $r++) { $noisy[$r] = @{ candidate = $(if ($r % 2) { 1.03 } else { 0.97 }) } }
    $runs = New-Series -Arms @('baseline', 'candidate') -RoundCount 12 -Speed @{ baseline = 1.0; candidate = 1.0 } -ArmJitter $noisy
    $res = Get-Comparison -Runs $runs -TolerancePct 0.5
    $iv = $res.Comparisons['candidate vs baseline']
    Assert-Case 'a noisy candidate with no true change is Unresolved' `
        ($res.Verdict -eq 'Unresolved' -and $iv.Low -lt -0.5 -and $iv.High -gt 0) "got $($res.Verdict) [$($iv.Low), $($iv.High)]"
    $runs = New-Series -Arms @('baseline', 'candidate', 'control') -RoundCount 12 -Speed @{ baseline = 1.0; candidate = 1.0; control = 1.0 } -Jitter $jitter
    $res = Get-Comparison -Runs $runs -TolerancePct 0.5
    Assert-Case 'the report renders every section' `
        (@(Format-Report -Result $res -TolerancePct 0.5 | Where-Object { $_ -match 'VERDICT: No slowdown|Per position|Per-round deltas|slot 2|per arm.*control' }).Count -eq 5)
    $runs = New-Series -Arms @('baseline', 'candidate', 'control') -RoundCount 12 -Speed @{ baseline = 1.0; candidate = 1.03; control = 1.0 } -Jitter $jitter
    $res = Get-Comparison -Runs $runs -TolerancePct 0.5
    $ratio = $res.ArmNps['candidate'] / $res.ArmNps['baseline']
    Assert-Case 'per-arm nps: the 3% faster candidate is 3% above the baseline' ([math]::Abs($ratio - 1.03) -lt 0.005) "got ratio $ratio"
    $trend = @(Format-Report -Result $res -TolerancePct 0.5 -TrendOnly)
    Assert-Case 'FALSIFY: -TrendOnly turns a Speedup into a trend, with no verdict or relink advice' `
        ($res.Verdict -eq 'Speedup' -and @($trend | Where-Object { $_ -match '^TREND: \+3\.' }).Count -eq 1 -and
         @($trend | Where-Object { $_ -cmatch 'VERDICT|PENDING|New-OrderedBuildPair' }).Count -eq 0) ($trend[-1])

    # The refusals. Each one would otherwise average an invalid run into the verdict.
    $good = @(New-Series -Arms @('baseline', 'candidate') -RoundCount 2 -Speed @{ baseline = 1.0; candidate = 1.0 })
    Assert-Case 'matching runs are accepted' (-not (Test-Refuses -Match '.' { Assert-ComparableRuns -Runs $good -FloorMs 200 }))
    foreach ($case in @(
            @{ Name = 'a node-count difference'; Rows = (New-Rows -NodesB 2000001); Match = 'differs from round 0' }
            @{ Name = 'a best-move difference';  Rows = (New-Rows -BestB 'b1c3');  Match = 'differs from round 0' }
            @{ Name = 'a missing position';      Rows = @((New-Rows)[0]);          Match = 'differs from round 0' }
            @{ Name = 'a search under the floor'; Rows = (New-Rows -MsB 150);      Match = 'under the 200 ms floor' })) {
        $bad = @($good | Select-Object -SkipLast 1) + [pscustomobject]@{ Round = 2; Slot = 1; Arm = 'candidate'; Rows = $case.Rows }
        Assert-Case "FALSIFY: $($case.Name) rejects the comparison" `
            (Test-Refuses -Match $case.Match { Assert-ComparableRuns -Runs $bad -FloorMs 200 })
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

if (-not $Baseline -or -not $Candidate) { throw '-Baseline and -Candidate are both required.' }
$baselinePath = (Resolve-Path $Baseline).Path
$candidatePath = (Resolve-Path $Candidate).Path
$runBench = Join-Path $PSScriptRoot 'Run-Bench.ps1'

$arms = if ($Control) { @('baseline', 'candidate', 'control') } else { @('baseline', 'candidate') }
$schedule = @(Get-Schedule -Arms $arms -RoundCount $Rounds)

if (-not $OutDir) { $OutDir = Join-Path ([System.IO.Path]::GetTempPath()) "compare-bench-$(Get-Date -Format yyyyMMdd-HHmmss)" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$outPath = (Resolve-Path $OutDir).Path
if (@(Get-ChildItem -Path $outPath -Filter '*.csv' -File).Count -gt 0) {
    throw "$outPath already holds CSVs; use a fresh -OutDir so runs from two series cannot mix."
}

$exes = @{ baseline = $baselinePath; candidate = $candidatePath }
if ($Control) {
    # A separate file with the baseline's exact bytes: any difference it reads is noise.
    $controlDir = Join-Path $outPath 'control'
    New-Item -ItemType Directory -Force -Path $controlDir | Out-Null
    $exes.control = Join-Path $controlDir (Split-Path -Leaf $baselinePath)
    Copy-Item -Path $baselinePath -Destination $exes.control -Force
}
$hashes = @{}
foreach ($arm in $arms) { $hashes[$arm] = (Get-FileHash -Path $exes[$arm] -Algorithm SHA256).Hash.ToLower() }

$positionList = @(Resolve-Positions -Path $Positions)
$metadata = [ordered]@{
    Started     = (Get-Date).ToString('o')
    Host        = [Environment]::MachineName
    Exes        = $exes
    Commits     = @{ baseline = $BaselineCommit; candidate = $CandidateCommit }
    Sha256      = $hashes
    Depth       = $Depth
    Threads     = 1
    PositionSet = if ($Positions) { (Resolve-Path $Positions).Path } else { 'builtin' }
    PositionSha = Get-PositionSetHash -List $positionList
    Rounds      = $Rounds
    WarmUp      = 'round 0, one run per arm, discarded'
    Schedule    = @($schedule | ForEach-Object { $_ -join ',' })
    Affinity    = $Affinity
    MinTimeMs   = $MinTimeMs
    Tolerance   = $SlowdownTolerancePct
    TrendOnly   = [bool]$TrendOnly
}
$metadata | ConvertTo-Json -Depth 4 | Set-Content -Path (Join-Path $outPath 'metadata.json')

Write-Host "Comparing $($arms.Count) arms, $Rounds rounds + warm-up, depth $Depth, $($positionList.Count) positions -> $outPath"
$runs = [System.Collections.Generic.List[object]]::new()
$self = [System.Diagnostics.Process]::GetCurrentProcess()
$priorAffinity = $self.ProcessorAffinity
if ($Affinity -ne 0) { $self.ProcessorAffinity = [IntPtr]$Affinity }
try {
    for ($round = 0; $round -le $Rounds; $round++) {
        $order = if ($round -eq 0) { $arms } else { $schedule[$round - 1] }
        for ($slot = 0; $slot -lt $order.Count; $slot++) {
            $arm = $order[$slot]
            $csv = Join-Path $outPath ('r{0:D3}_{1}_{2}.csv' -f $round, $slot, $arm)
            # Run-Bench's table goes to the information stream; the CSV is the record.
            & $runBench -Exe $exes[$arm] -Depth $Depth -Threads 1 -Positions $Positions -MinTimeMs $MinTimeMs -Csv $csv 6>$null | Out-Null
            $runs.Add([pscustomobject]@{ Round = $round; Slot = $slot; Arm = $arm; Rows = @(Import-Csv -Path $csv) })
            # Checked as each run lands, so an invalid comparison stops before the rest of the budget.
            Assert-ComparableRuns -Runs @($runs[0], $runs[$runs.Count - 1]) -FloorMs $MinTimeMs
        }
        Write-Host ("{0:HH:mm:ss} round {1}/{2} done" -f (Get-Date), $round, $Rounds)
    }
} finally {
    # Restored so an in-process caller's session is not left pinned.
    $self.ProcessorAffinity = $priorAffinity
}

$result = Get-Comparison -Runs $runs -TolerancePct $SlowdownTolerancePct
$report = @(Format-Report -Result $result -TolerancePct $SlowdownTolerancePct -TrendOnly:$TrendOnly)
if ($BaselineCommit -or $CandidateCommit) { $report = @("Baseline $BaselineCommit  candidate $CandidateCommit") + $report }
$report | Set-Content -Path (Join-Path $outPath 'report.txt')
Write-Host ''
$report | ForEach-Object { Write-Host $_ }
Write-Host ''
Write-Host "Raw CSVs, metadata.json and report.txt: $outPath"
