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
    aggregate nps per arm, total nodes / total ms over the suite. That is valid only because
    the node counts were checked identical. Each contrast is the round's log nps ratio, not a
    percentage ratio: a mean of percentage ratios reads above zero for identical arms
    (mean(x/y) > 1), a mean of log ratios does not. The interval is a two-sided 95% Student-t
    interval over the kept rounds' log ratios. Above 30 degrees of freedom it uses the
    critical value of the next lower table row, which errs wide. Mean, bounds, median and
    range print as the percentage each log value stands for, 100 x (e^v - 1); sd as 100 x sd.
    With -Control, the verdict compares the candidate against the geometric mean of both
    baseline arms, i.e. the average of its two per-arm log contrasts.

    Verdict, checked in this order; every threshold is applied to the log values:
      Speedup      only with -Control: the 95% lower bound > 0 against the baseline AND
                   against the control, and, when the A/A interval excludes zero, the lower
                   of those bounds above the run's own steady A/A offset |A/A mean|. The
                   reference contrast needs no condition of its own: its lower bound is at
                   least the average of the per-arm ones. The report says which condition
                   failed when the reference's lower bound is > 0 but Speedup was not issued.
                   Confirmed only on a New-OrderedBuildPair.ps1 pair (shared /ORDER):
                   placement alone moves node-identical builds by several percent either way.
      No slowdown  reference lower bound >= -0.5%; a small cost inside that tolerance still
                   passes, and the printed interval shows it.
      Slowdown     reference upper bound < 0.
      Unresolved   anything else. Rerun with -Control or relink both builds with
                   New-OrderedBuildPair.ps1 (measure-strength regression-check). Do not add
                   rounds to the same series until it passes.
    An A/A interval not wholly inside +-0.5% prints a WARNING under any verdict, never a
    veto: a 0.2% and an 8% gain need not be measured equally precisely. The band is its own
    constant, recorded as ControlBand in metadata.json, apart from the slowdown tolerance.
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
    Report the verdict comparison's interval as a trend instead of a verdict, with no A/A warning.

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
# How far apart the two identical baseline copies may read before the report warns. A warning only:
# per-arm intervals carry imprecision and Get-Verdict gates a steady offset. Kept apart from the
# slowdown tolerance, because acceptable regression and control agreement are separate questions.
$ControlBandPct = 0.5

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

function ConvertTo-Pct {
    <# A log nps ratio as the percentage change it stands for. #>
    param([Parameter(Mandatory)][double]$Log)
    return 100.0 * ([math]::Exp($Log) - 1)
}

function Get-Verdict {
    <#
        Every interval is in log units; gates never compare displayed percentages, which are not
        symmetric under swapping the two identical baseline copies.
        Speedup needs -Control ($ArmIntervals: candidate vs 'baseline' and vs 'control', and
        $AaInterval), both per-arm lower bounds > 0 and, when the A/A interval excludes zero, the
        lower of them above the run's own steady offset |A/A mean|. No third condition on the
        reference interval: against the geometric mean its lower bound is at least the average of
        the per-arm ones, so the first condition implies it. Returns the verdict and the Speedup
        conditions that failed, if any.
    #>
    param([Parameter(Mandatory)][object]$Interval, [Parameter(Mandatory)][double]$TolerancePct,
          [hashtable]$ArmIntervals = $null, [object]$AaInterval = $null)
    $failed = [System.Collections.Generic.List[string]]::new()
    if ($null -ne $ArmIntervals) {
        foreach ($arm in @('baseline', 'control')) {
            if ($ArmIntervals[$arm].Low -le 0) { $failed.Add("lower bound <= 0 against $arm") }
        }
        $aaExcludesZero = $AaInterval.Low -gt 0 -or $AaInterval.High -lt 0
        $minLow = [math]::Min($ArmIntervals['baseline'].Low, $ArmIntervals['control'].Low)
        if ($aaExcludesZero -and $minLow -le [math]::Abs($AaInterval.Mean)) {
            $failed.Add(('lower bound does not exceed the steady A/A offset of {0}%' -f
                         (ConvertTo-Pct ([math]::Abs($AaInterval.Mean))).ToString('0.00', [cultureinfo]::InvariantCulture)))
        }
        if ($failed.Count -eq 0) { return [pscustomobject]@{ Verdict = 'Speedup'; SpeedupFailures = @() } }
    }
    $verdict = if ($Interval.Low -ge [math]::Log(1 - $TolerancePct / 100)) { 'No slowdown' }
               elseif ($Interval.High -lt 0) { 'Slowdown' }
               else { 'Unresolved' }
    return [pscustomobject]@{ Verdict = $verdict; SpeedupFailures = @($failed) }
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
    # Contrasts are log ratios, converted to percent only for display. A mean of percentage ratios
    # reads above zero for identically distributed arms (mean(x/y) > 1, Jensen); a mean of log
    # ratios is exactly zero for them.
    $logRatio = { param($a, $b) [math]::Log($a / $b) }

    # One delta per round per comparison; 'reference' is what the candidate is judged against.
    $pairs = [ordered]@{ 'candidate vs baseline' = @('candidate', 'baseline') }
    if ($hasControl) {
        $pairs['candidate vs control'] = @('candidate', 'control')
        $pairs['control vs baseline (A/A)'] = @('control', 'baseline')
    }
    $comparisons = [ordered]@{}
    foreach ($name in $pairs.Keys) {
        $x, $y = $pairs[$name]
        $deltas = foreach ($r in $byRound.Values) { & $logRatio (Get-AggregateNps $r[$x].Rows) (Get-AggregateNps $r[$y].Rows) }
        $comparisons[$name] = Get-Interval -Values $deltas
    }
    # The verdict judges the candidate against the baseline, or against the geometric mean of
    # baseline and control: the average of the two per-arm log contrasts. The arithmetic mean
    # would read a phantom slowdown on equal arms under noise.
    # $select picks which of a run's rows to aggregate: all of them, or one position's.
    $candidateDelta = {
        param($r, $select)
        $candidateLog = [math]::Log((Get-AggregateNps (& $select $r['candidate'])))
        $refLog = [math]::Log((Get-AggregateNps (& $select $r['baseline'])))
        if ($hasControl) { $refLog = ($refLog + [math]::Log((Get-AggregateNps (& $select $r['control'])))) / 2 }
        $candidateLog - $refLog
    }
    $verdictName = if ($hasControl) { 'candidate vs geomean(baseline, control)' } else { 'candidate vs baseline' }
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

    $verdictArgs = @{ Interval = $comparisons[$verdictName]; TolerancePct = $TolerancePct }
    if ($hasControl) {
        $verdictArgs.ArmIntervals = @{ baseline = $comparisons['candidate vs baseline']; control = $comparisons['candidate vs control'] }
        $verdictArgs.AaInterval = $comparisons['control vs baseline (A/A)']
    }
    $judged = Get-Verdict @verdictArgs
    return [pscustomobject]@{
        Rounds       = $byRound.Count
        Comparisons  = $comparisons
        VerdictBasis = $verdictName
        RoundDeltas  = $roundDeltas
        Verdict      = $judged.Verdict
        SpeedupFailures = $judged.SpeedupFailures
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
    # Intervals are in log units: each statistic prints as the percentage it stands for, sd as 100 x sd.
    $row = { param($label, $c) & $fmt $line $label (ConvertTo-Pct $c.Mean) (100 * $c.Sd) (ConvertTo-Pct $c.Low) (ConvertTo-Pct $c.High) (ConvertTo-Pct $c.Median) (ConvertTo-Pct $c.Min) (ConvertTo-Pct $c.Max) }
    $out = [System.Collections.Generic.List[string]]::new()
    $out.Add("Kept rounds: $($Result.Rounds) (round 0 discarded as warm-up). Nodes and best moves identical in every run.")
    $out.Add('')
    $out.Add(('{0,-38} {1,9} {2,6} {3,20} {4,9}  {5}' -f 'comparison (aggregate nps, per round)', 'mean', 'sd', '95% t-interval', 'median', 'range'))
    foreach ($name in $Result.Comparisons.Keys) {
        $out.Add((& $row $name $Result.Comparisons[$name]))
    }
    $out.Add("Per-round deltas, $($Result.VerdictBasis): " +
             (($Result.RoundDeltas | ForEach-Object { & $fmt '{0:+0.00;-0.00;0.00}%' (ConvertTo-Pct $_) }) -join ' '))
    $out.Add('')
    $out.Add("Per position, candidate vs the verdict's reference:")
    foreach ($name in $Result.PerPosition.Keys) {
        $out.Add((& $row "  $name" $Result.PerPosition[$name]))
    }
    $out.Add('')
    $out.Add('Run-order effect, aggregate nps by slot vs the overall mean: ' +
             (($Result.Slots.Keys | ForEach-Object { & $fmt '{0} {1:+0.00;-0.00;0.00}%' $_ $Result.Slots[$_] }) -join ', '))
    $out.Add('Aggregate nps per arm, median over kept rounds: ' +
             (($Result.ArmNps.Keys | ForEach-Object { & $fmt '{0} {1:N0}' $_ $Result.ArmNps[$_] }) -join ', '))
    $out.Add('')
    if ($TrendOnly) {
        $c = $Result.Comparisons[$Result.VerdictBasis]
        $out.Add((& $fmt "TREND: {0:+0.00;-0.00;0.00}% [{1:+0.00;-0.00;0.00}%, {2:+0.00;-0.00;0.00}%] ('$($Result.VerdictBasis)'); no verdict without placement control. Not Elo." (ConvertTo-Pct $c.Mean) (ConvertTo-Pct $c.Low) (ConvertTo-Pct $c.High)))
        return $out
    }
    $out.Add("VERDICT: $($Result.Verdict)  (from '$($Result.VerdictBasis)'; tolerance -$TolerancePct%)")
    if ($Result.SpeedupFailures.Count -gt 0 -and $Result.Comparisons[$Result.VerdictBasis].Low -gt 0) {
        $out.Add("Speedup not issued: $($Result.SpeedupFailures -join '; ').")
    }
    if ($Result.Comparisons.Contains('control vs baseline (A/A)')) {
        $aa = $Result.Comparisons['control vs baseline (A/A)']
        # A log band, so swapping which identical copy is the control cannot change whether it warns.
        $band = [math]::Log(1 + $ControlBandPct / 100)
        if ($aa.Low -lt -$band -or $aa.High -gt $band) {
            $out.Add((& $fmt "WARNING: A/A [{0:+0.00;-0.00;0.00}%, {1:+0.00;-0.00;0.00}%] leaves +-{2}%: the two baseline copies disagree by more than the band (offset, imprecision or both). Per-arm intervals carry the imprecision; a steady offset is gated by the offset condition. No arm is named faulty." (ConvertTo-Pct $aa.Low) (ConvertTo-Pct $aa.High) $ControlBandPct))
        }
    }
    switch ($Result.Verdict) {
        'Speedup'     { $out.Add('Confirmed only if both exes came from New-OrderedBuildPair.ps1; otherwise relink and rerun with -Control. Not Elo.') }
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

    # Get-Verdict takes log intervals; the table states them in percent, as the report shows them.
    $toLog = { param($p) [math]::Log(1 + $p / 100) }
    # @(low, high[, mean]); the mean defaults to the midpoint.
    $logIv = {
        param([double[]]$Bounds)
        $mid = if ($Bounds.Count -gt 2) { $Bounds[2] } else { ($Bounds[0] + $Bounds[1]) / 2 }
        $low = & $toLog $Bounds[0]; $high = & $toLog $Bounds[1]; $mean = & $toLog $mid
        [pscustomobject]@{ Low = $low; High = $high; Mean = $mean; Sd = 0.0; Median = $mean; Min = $low; Max = $high }
    }
    foreach ($case in @(
            @{ Name = '+2% over both, A/A spans zero';            Ref = @(1.5, 2.7);   Base = @(1.32, 2.44); Ctrl = @(1.75, 2.95); Aa = @(-0.94, 0.03); Expect = 'Speedup' }
            @{ Name = 'baseline lower bound -0.01'; Ref = @(0.3, 1.5); Base = @(-0.01, 1.44); Ctrl = @(0.4, 1.6); Aa = @(-0.3, 0.2); Expect = 'No slowdown'; Fails = 'against baseline' }
            @{ Name = '+1% over both, A/A low -0.51'; Ref = @(0.9, 1.3); Base = @(0.76, 1.28); Ctrl = @(0.96, 1.44); Aa = @(-0.51, 0.1); Expect = 'Speedup' }
            @{ Name = '+8% over both, A/A +-1%';                 Ref = @(7, 9);       Base = @(7, 9);       Ctrl = @(7, 9);       Aa = @(-1, 1);        Expect = 'Speedup' }
            @{ Name = 'steady A/A -2%, +0.8% over the faster arm'; Ref = @(1.5, 2.1); Base = @(0.5, 1.1); Ctrl = @(2.5, 3.2); Aa = @(-2.3, -1.7, -2.0); Expect = 'No slowdown'; Fails = 'steady A/A offset' }
            @{ Name = 'steady A/A -2%, +3% over the faster arm';  Ref = @(3.7, 4.3);  Base = @(2.7, 3.3);   Ctrl = @(4.7, 5.4);   Aa = @(-2.3, -1.7, -2.0); Expect = 'Speedup' }
            @{ Name = 'FALSIFY: a lower bound exactly 0';         Ref = @(0.2, 1.0);   Base = @(0, 1.0);     Ctrl = @(0.4, 1.0);   Aa = @(-0.2, 0.2);    Expect = 'No slowdown'; Fails = 'against baseline' }
            @{ Name = 'FALSIFY: control lower bound below 0';     Ref = @(0.2, 1.0);   Base = @(0.4, 1.0);   Ctrl = @(-0.1, 1.0);  Aa = @(-0.2, 0.2);    Expect = 'No slowdown'; Fails = 'against control' }
            @{ Name = 'narrow A/A, ordinary gain';                Ref = @(0.2, 1.0);   Base = @(0.2, 1.0);   Ctrl = @(0.2, 1.0);   Aa = @(-0.1, 0.1);    Expect = 'Speedup' }
            @{ Name = 'FALSIFY: no control, clear gain';          Ref = @(0.2, 1.0);   Expect = 'No slowdown' }
            @{ Name = 'lower bound exactly -0.5%';                Ref = @(-0.5, 0.4);  Expect = 'No slowdown' }
            @{ Name = 'no control, small cost';                   Ref = @(-0.45, -0.1); Expect = 'No slowdown' }
            @{ Name = 'no control, clear cost';                   Ref = @(-1.5, -0.2); Expect = 'Slowdown' }
            @{ Name = 'no control, wide';                         Ref = @(-1.0, 0.5);  Expect = 'Unresolved' })) {
        $verdictArgs = @{ Interval = (& $logIv $case.Ref); TolerancePct = 0.5 }
        if ($case.ContainsKey('Base')) {
            $verdictArgs.ArmIntervals = @{ baseline = (& $logIv $case.Base); control = (& $logIv $case.Ctrl) }
            $verdictArgs.AaInterval = & $logIv $case.Aa
        }
        $got = Get-Verdict @verdictArgs
        $failsOk = if ($case.ContainsKey('Fails')) { @($got.SpeedupFailures | Where-Object { $_ -match $case.Fails }).Count -eq 1 } else { $got.SpeedupFailures.Count -eq 0 }
        Assert-Case "verdict: $($case.Name) -> $($case.Expect)" ($got.Verdict -eq $case.Expect -and $failsOk) "got $($got.Verdict), failed: $($got.SpeedupFailures -join '; ')"
    }
    $baseIv = (& $logIv @(0.4, 1.0)); $ctrlIv = (& $logIv @(0.2, 1.0))
    $swapped = (Get-Verdict -Interval (& $logIv @(0.3, 1.0)) -TolerancePct 0.5 -ArmIntervals @{ baseline = $ctrlIv; control = $baseIv } -AaInterval (& $logIv @(-0.2, 0.2))).Verdict
    Assert-Case 'verdict: swapping the per-arm intervals keeps the verdict' ($swapped -eq 'Speedup') "got $swapped"

    # The A/A warning, by A/A interval in percent; any verdict, so the reference is a fixed gain.
    foreach ($case in @(
            @{ Aa = @(-0.94, 0.03); Warn = $true }
            @{ Aa = @(-0.51, 0.1);  Warn = $true }
            @{ Aa = @(-1, 1);       Warn = $true }
            @{ Aa = @(-2.3, -1.7);  Warn = $true }
            @{ Aa = @(-0.499, 0.2); Warn = $true }
            @{ Aa = @(-0.49, 0.49); Warn = $false })) {
        foreach ($flip in @($false, $true)) {
            # Swapping the copies negates the log interval: [l, h] becomes [-h, -l].
            $aaBounds = if ($flip) { @((ConvertTo-Pct (-(& $toLog $case.Aa[1]))), (ConvertTo-Pct (-(& $toLog $case.Aa[0])))) } else { $case.Aa }
            $aaIv = & $logIv $aaBounds
            $fake = [pscustomobject]@{
                Rounds = 12; VerdictBasis = 'ref'; RoundDeltas = @(0.01); Verdict = 'Speedup'; SpeedupFailures = @()
                Comparisons = [ordered]@{ 'ref' = (& $logIv @(1, 2)); 'control vs baseline (A/A)' = $aaIv }
                PerPosition = [ordered]@{}; Slots = [ordered]@{ 'slot 0' = 0.0 }; ArmNps = [ordered]@{ baseline = 1.0 }
            }
            $warned = @(Format-Report -Result $fake -TolerancePct 0.5 | Where-Object { $_ -match '^WARNING: A/A' }).Count -eq 1
            $label = if ($flip) { ', copies swapped' } else { '' }
            $aaText = ($case.Aa | ForEach-Object { $_.ToString([cultureinfo]::InvariantCulture) }) -join ', '
            Assert-Case "A/A [$aaText]%$label warns: $($case.Warn)" ($warned -eq $case.Warn)
        }
    }

    # End to end over synthetic runs, with jitter so the interval has width.
    $jitter = @{ 1 = 1.004; 2 = 0.996; 3 = 1.002; 4 = 0.998; 5 = 1.001; 6 = 0.999 }
    $runs = New-Series -Arms @('baseline', 'candidate') -RoundCount 12 -Speed @{ baseline = 1.0; candidate = 0.97 } -Jitter $jitter
    $res = Get-Comparison -Runs $runs -TolerancePct 0.5
    Assert-Case 'a 3% slower candidate is a Slowdown near -3%' `
        ($res.Verdict -eq 'Slowdown' -and [math]::Abs((ConvertTo-Pct $res.Comparisons['candidate vs baseline'].Mean) + 3) -lt 0.01 -and $res.Rounds -eq 12) `
        "got $($res.Verdict) mean $($res.Comparisons['candidate vs baseline'].Mean)"

    $runs = New-Series -Arms @('baseline', 'candidate') -RoundCount 12 -Speed @{ baseline = 1.0; candidate = 1.0 } -Jitter $jitter -Round0 0.5
    $res = Get-Comparison -Runs $runs -TolerancePct 0.5
    Assert-Case 'a slow warm-up round is excluded from the result' `
        ($res.Verdict -eq 'No slowdown' -and [math]::Abs($res.Comparisons['candidate vs baseline'].Mean) -lt 1e-9) `
        "got $($res.Verdict) mean $($res.Comparisons['candidate vs baseline'].Mean)"

    $runs = New-Series -Arms @('baseline', 'candidate', 'control') -RoundCount 12 -Speed @{ baseline = 1.0; candidate = 1.0; control = 1.0 } -Jitter $jitter
    $res = Get-Comparison -Runs $runs -TolerancePct 0.5
    Assert-Case 'with a control the verdict is judged against geomean(baseline, control)' `
        ($res.VerdictBasis -eq 'candidate vs geomean(baseline, control)' -and $res.Comparisons.Contains('control vs baseline (A/A)') -and
         $res.PerPosition.Count -eq 2 -and $res.Slots.Count -eq 3)
    $runs = New-Series -Arms @('baseline', 'candidate', 'control') -RoundCount 12 -Speed @{ baseline = 1.0; candidate = 1.03; control = 1.0 } -Jitter $jitter
    $res = Get-Comparison -Runs $runs -TolerancePct 0.5
    $report = @(Format-Report -Result $res -TolerancePct 0.5)
    Assert-Case 'a 3% faster candidate with a quiet control is Speedup, ordered-pair caveat, no warning' `
        ($res.Verdict -eq 'Speedup' -and @($report | Where-Object { $_ -match '^Confirmed only if both exes came from New-OrderedBuildPair' }).Count -eq 1 -and
         @($report | Where-Object { $_ -match '^WARNING|^Speedup not issued' }).Count -eq 0) `
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
        ($res.Verdict -ne 'Speedup' -and (ConvertTo-Pct $aa.High) - (ConvertTo-Pct $aa.Low) -gt 10) "got $($res.Verdict), A/A [$($aa.Low), $($aa.High)]"
    $noisy = @{}
    for ($r = 1; $r -le 12; $r++) { $noisy[$r] = @{ candidate = $(if ($r % 2) { 1.03 } else { 0.97 }) } }
    $runs = New-Series -Arms @('baseline', 'candidate') -RoundCount 12 -Speed @{ baseline = 1.0; candidate = 1.0 } -ArmJitter $noisy
    $res = Get-Comparison -Runs $runs -TolerancePct 0.5
    $iv = $res.Comparisons['candidate vs baseline']
    Assert-Case 'a noisy candidate with no true change is Unresolved' `
        ($res.Verdict -eq 'Unresolved' -and (ConvertTo-Pct $iv.Low) -lt -0.5 -and $iv.High -gt 0) "got $($res.Verdict) [$($iv.Low), $($iv.High)]"
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
         @($trend | Where-Object { $_ -cmatch 'VERDICT|WARNING|Confirmed|New-OrderedBuildPair' }).Count -eq 0) ($trend[-1])

    # Identically distributed arms. Each round permutes (0.7, 1.0, 1.3) over the arms, and every
    # permutation meets every schedule order, so the log contrasts cancel exactly.
    $values = @(0.7, 1.0, 1.3)
    $perms = @(@(0, 1, 2), @(0, 2, 1), @(1, 0, 2), @(1, 2, 0), @(2, 0, 1), @(2, 1, 0))
    $permuted = @{}
    for ($r = 1; $r -le 576; $r++) {
        $perm = $perms[[math]::Floor(($r - 1) / 6) % 6]
        $permuted[$r] = @{ baseline = $values[$perm[0]]; candidate = $values[$perm[1]]; control = $values[$perm[2]] }
    }
    $runs = New-Series -Arms @('baseline', 'candidate', 'control') -RoundCount 576 -Speed @{ baseline = 1.0; candidate = 1.0; control = 1.0 } -ArmJitter $permuted
    $res = Get-Comparison -Runs $runs -TolerancePct 0.5
    $means = @($res.Comparisons.Values | ForEach-Object { $_.Mean })
    $report = @(Format-Report -Result $res -TolerancePct 0.5)
    Assert-Case 'FALSIFY: equal arms under +-30% noise read a zero mean contrast, not Speedup, with a warning' `
        ($res.Verdict -ne 'Speedup' -and @($means | Where-Object { [math]::Abs($_) -gt 1e-9 }).Count -eq 0 -and
         @($report | Where-Object { $_ -match '^WARNING: A/A' }).Count -eq 1) "got $($res.Verdict), means $($means -join ', ')"
    $runs = New-Series -Arms @('baseline', 'candidate', 'control') -RoundCount 576 -Speed @{ baseline = 1.0; candidate = 0.99; control = 1.0 } -ArmJitter $permuted
    $res = Get-Comparison -Runs $runs -TolerancePct 0.5
    Assert-Case 'FALSIFY: the same noise with a 1% slower candidate reads negative, not Speedup' `
        ($res.Verdict -ne 'Speedup' -and $res.Comparisons[$res.VerdictBasis].Mean -lt 0) "got $($res.Verdict)"

    # A steady offset between the identical copies, larger in log units than the candidate's gain over
    # the faster one. A percent comparison would pass in one orientation only.
    foreach ($case in @(@{ Name = 'baseline faster'; Speed = @{ baseline = 1.0; control = 100000 / 102000 } },
                        @{ Name = 'control faster';  Speed = @{ baseline = 100000 / 102000; control = 1.0 } })) {
        $case.Speed.candidate = 100000 / 98058
        $runs = New-Series -Arms @('baseline', 'candidate', 'control') -RoundCount 12 -Speed $case.Speed
        $res = Get-Comparison -Runs $runs -TolerancePct 0.5
        $report = @(Format-Report -Result $res -TolerancePct 0.5)
        Assert-Case "FALSIFY: a steady A/A offset above the gain over the faster arm blocks Speedup ($($case.Name))" `
            ($res.Verdict -eq 'No slowdown' -and @($report | Where-Object { $_ -match '^Speedup not issued: .*steady A/A offset' }).Count -eq 1) `
            "got $($res.Verdict); $($report -match 'not issued')"
    }

    # Opposed references: baseline and control alternate 1.1/0.9 against a flat candidate.
    $opposedLong = @{}
    $swappedLong = @{}
    for ($r = 1; $r -le 600; $r++) {
        $m = if ($r % 2) { 1.1 } else { 0.9 }
        $opposedLong[$r] = @{ baseline = $m; control = 2.0 - $m }
        $swappedLong[$r] = @{ baseline = 2.0 - $m; control = $m }
    }
    foreach ($case in @(@{ Candidate = 1.00; Expect = 'No slowdown' }, @{ Candidate = 1.30; Expect = 'Speedup' })) {
        $got = foreach ($noise in @($opposedLong, $swappedLong)) {
            $runs = New-Series -Arms @('baseline', 'candidate', 'control') -RoundCount 600 -Speed @{ baseline = 1.0; candidate = $case.Candidate; control = 1.0 } -ArmJitter $noise
            $res = Get-Comparison -Runs $runs -TolerancePct 0.5
            $res.Verdict
        }
        Assert-Case "opposed references over 600 rounds, candidate $($case.Candidate) -> $($case.Expect), swapped or not" `
            (@($got | Where-Object { $_ -eq $case.Expect }).Count -eq 2) "got $($got -join ', ')"
    }
    $report = @(Format-Report -Result $res -TolerancePct 0.5)
    Assert-Case 'a Speedup with a wide A/A prints the warning' (@($report | Where-Object { $_ -match '^WARNING: A/A \[' }).Count -eq 1) ($report -join ' | ')

    # Percent conversion of a known log contrast: alternating 0.01 / 0.03 over 12 rounds gives mean 0.02,
    # sd sqrt(12e-4 / 11) and half-width t(11) 2.201 x sd / sqrt(12).
    $known = @{}
    for ($r = 1; $r -le 12; $r++) { $known[$r] = @{ candidate = [math]::Exp($(if ($r % 2) { 0.01 } else { 0.03 })) } }
    $runs = New-Series -Arms @('baseline', 'candidate') -RoundCount 12 -Speed @{ baseline = 1.0; candidate = 1.0 } -ArmJitter $known
    $res = Get-Comparison -Runs $runs -TolerancePct 0.5
    $knownSd = [math]::Sqrt(12e-4 / 11)
    $knownHalf = 2.201 * $knownSd / [math]::Sqrt(12)
    $inv = [cultureinfo]::InvariantCulture
    $expected = @((100 * ([math]::Exp(0.02) - 1)), (100 * $knownSd), (100 * ([math]::Exp(0.02 - $knownHalf) - 1)), (100 * ([math]::Exp(0.02 + $knownHalf) - 1))) |
        ForEach-Object { $_.ToString('0.00', $inv) }
    $knownLine = @(Format-Report -Result $res -TolerancePct 0.5 | Where-Object { $_ -match '^candidate vs baseline ' })[0]
    $pattern = '\+{0}%\s+{1} \[\s*\+{2}%,\s+\+{3}%\]' -f @($expected | ForEach-Object { [regex]::Escape($_) })
    Assert-Case 'the report prints a known log contrast as 100(e^v - 1) % and sd as 100 sd' ($knownLine -match $pattern) "got '$knownLine', want $($expected -join ' ')"

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
    ControlBand = $ControlBandPct
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
