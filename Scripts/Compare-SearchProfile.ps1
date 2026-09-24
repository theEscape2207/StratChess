<#
.SYNOPSIS
    Compare how two STRAT_SEARCH_PROFILE builds search the same positions: tree shape, ordering,
    reductions, pruning, quiescence, iterations and best-move stability, side by side.

.DESCRIPTION
    Runs each position once per build at a fixed depth, Threads=1, in a fresh engine process, and
    prints a before / after / delta table per scope: Pooled, Endgame, Non-endgame, then each
    position. It answers "how did the tree change", which Run-Bench (time) and
    Compare-SearchEquivalence (identity) do not. It does not judge significance: at Threads=1 a
    search is deterministic, so each delta is one exact difference, not a sample.

    Pooling: every counter is summed over the scope's positions first, then each ratio is taken,
    so a pooled rate is a ratio of sums. 'maxdepth' pools by max, the settled iteration by mean.

    Rows derived from the 'info depth' lines (the first line per depth; the summary line that
    repeats the last depth is not an iteration):
      - n(d), the nodes of iteration d, is the cumulative count less the previous iteration's.
      - EBF is the geometric mean of n(d)/n(d-1) over the last four iterations,
        (n(D)/n(D-4))^(1/4). Earlier iterations are dominated by the root move count.
      - Best-move changes count iterations whose first PV move differs from the previous one.
      - The settled iteration is the first from which the first PV move always equals bestmove.
      - Score swing is the mean |score(d) - score(d-1)| in centipawns; a pair with a mate score
        is excluded.

    Deltas: counts as relative %, rates (every '% of' row) in percentage points, and EBF, ratios,
    qs nodes per root, maxdepth, settled iteration and swing as an absolute difference.

    Groups: a position is an endgame when each side's non-pawn material is at most 13
    (N and B 3, R 5, Q 9), read from its FEN. On the built-in set that is rook-endgm and
    piece-endgm; pooled numbers can hide opposite effects in the two groups.

    Refusals, per side and position, naming both:
      - A missing required line: treenodes, ordering, nodetypes, qsearch. Each always prints in a
        completed profile search at depth >= 5, so its absence means a default build, or one
        older than the six-line profile contract. That is refused, not read as zeros.
      - A malformed line: every present line must match its exact field list and histogram
        lengths. The wording is a contract (Docs/Engine-Readme.md).
      - A search that did not reach -Depth or print bestmove.
    The optional lines aspiration, lmr, nullmove, pruning, frontier skips and lmp skips print only
    when their first field is non-zero ('pruning' on either field), so their absence reads as zero.

    Not refused: the same binary on both sides (the zero-delta self-check), or differing node
    counts, which are what this measures. Node identity is Compare-SearchEquivalence's job.

.PARAMETER Before
    The baseline profile build (configured with -DSTRAT_SEARCH_PROFILE=1).

.PARAMETER After
    The candidate profile build.

.PARAMETER Depth
    Fixed search depth. Default 16, the depth of the recorded baselines. Minimum 5: EBF needs four
    ratios, and the required lines need cuts.

.PARAMETER Positions
    Optional file of FENs, one per line, as Run-Bench.ps1 takes. Defaults to Run-Bench's built-in
    set (Scripts/BenchPositions.ps1).

.PARAMETER SelfTest
    Assert parsing, refusals, pooling, derived rows, grouping and delta formats on synthetic
    transcripts, and exit. Runs no engine.

.EXAMPLE
    Compare-SearchProfile.ps1 -Before .\base-profile.exe -After .\cand-profile.exe

.EXAMPLE
    Compare-SearchProfile.ps1 -SelfTest
#>
[CmdletBinding()]
param(
    [string]$Before = '',

    [string]$After = '',

    [ValidateRange(5, 30)]
    [int]$Depth = 16,

    [string]$Positions = '',

    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'BenchPositions.ps1')

# Field lists of every parsed 'info string' line, in print order. A number is a scalar; a
# histogram has that many slash-separated bins. Keys and wording are the engine's output contract.
$ProfileSchema = [ordered]@{
    treenodes  = @{ Required = $true;  Fields = [ordered]@{ main = 1; qs = 1 } }
    frontier   = @{ Required = $false; Fields = [ordered]@{ skips = 1 } }
    lmp        = @{ Required = $false; Fields = [ordered]@{ skips = 1 } }
    aspiration = @{ Required = $false; Fields = [ordered]@{ iterations = 1; faillow = 1; failhigh = 1; fullwindow = 1; failnodes = 1 } }
    ordering   = @{ Required = $true;  Fields = [ordered]@{ cuts = 1; index = 5; latecut = 4; hashnodes = 1; hashcuts = 1; latenodes = 1; latebands = 3 } }
    lmr        = @{ Required = $false; Fields = [ordered]@{ reduced = 1; reducednodes = 1; researched = 1; confirmed = 1; researchnodes = 1 } }
    nodetypes  = @{ Required = $true;  Fields = [ordered]@{ pv = 3; cut = 3; all = 3; cutfaillow = 3 } }
    nullmove   = @{ Required = $false; Fields = [ordered]@{ tried = 1; cutoffs = 1; failed = 1; failnodes = 1 } }
    pruning    = @{ Required = $false; Fields = [ordered]@{ rfp = 6; floorbinds = 1 } }
    qsearch    = @{ Required = $true;  Fields = [ordered]@{ roots = 1; delta = 1; see = 1; maxdepth = 1 } }
}

# ---------------------------------------------------------------------------
# Pure helpers — everything -SelfTest asserts lives here
# ---------------------------------------------------------------------------

function Get-LinePattern {
    param([Parameter(Mandatory)][string]$Key)

    $parts = foreach ($f in $ProfileSchema[$Key].Fields.GetEnumerator()) {
        $bins = if ($f.Value -eq 1) { '(\d+)' } else { '(\d+(?:/\d+){' + ($f.Value - 1) + '})' }
        "$($f.Key) $bins"
    }
    return '^info string ' + $Key + ' ' + ($parts -join ' ') + '$'
}

function ConvertFrom-ProfileTranscript {
    <#
        One engine transcript into counters, iterations and bestmove. Throws, naming side,
        position and line, on anything D4 of the design refuses.
    #>
    param(
        [Parameter(Mandatory)][AllowEmptyString()][string]$Output,
        [Parameter(Mandatory)][int]$SearchDepth,
        [Parameter(Mandatory)][string]$Side,
        [Parameter(Mandatory)][string]$Position
    )

    $where = "$Side build, position $Position"
    $lines = @($Output -split "`r?`n" | ForEach-Object { $_.Trim() })

    $iterations = [System.Collections.Generic.List[object]]::new()
    $seen = @{}
    foreach ($line in $lines) {
        if ($line -match '^info depth (\d+) score (cp|mate) (-?\d+) .*?\bnodes (\d+)\b.*? pv (\S+)') {
            $d = [int]$Matches[1]
            if ($seen.ContainsKey($d)) { continue }
            $seen[$d] = $true
            $iterations.Add([pscustomobject]@{
                Depth = $d
                Score = if ($Matches[2] -eq 'cp') { [int]$Matches[3] } else { $null }
                Nodes = [int64]$Matches[4]
                Move  = $Matches[5]
            })
        }
    }
    $best = @($lines | Where-Object { $_ -match '^bestmove \S+' })
    if (-not $seen.ContainsKey($SearchDepth) -or $best.Count -eq 0) {
        throw "$($where): the search did not complete depth $SearchDepth with a bestmove.`nEngine output:`n$Output"
    }
    for ($d = 1; $d -le $SearchDepth; $d++) {
        if (-not $seen.ContainsKey($d)) { throw "$($where): no 'info depth $d' line; iterations cannot be derived." }
    }

    $counters = @{}
    foreach ($key in $ProfileSchema.Keys) {
        $present = @($lines | Where-Object { $_ -match "^info string $key " })
        $fields = $ProfileSchema[$key].Fields
        $values = @{}
        if ($present.Count -eq 0) {
            if ($ProfileSchema[$key].Required) {
                throw ("$($where): no 'info string $key' line. A completed profile search always prints it, so this" +
                       " is a default build or one older than the profile contract.")
            }
            foreach ($f in $fields.GetEnumerator()) {
                $values[$f.Key] = if ($f.Value -eq 1) { [int64]0 } else { [int64[]]::new($f.Value) }
            }
        } else {
            $line = $present[$present.Count - 1]
            $m = [regex]::Match($line, (Get-LinePattern $key))
            if (-not $m.Success) { throw "$($where): malformed line, expected $(Get-LinePattern $key)`n  got: $line" }
            $g = 1
            foreach ($f in $fields.GetEnumerator()) {
                $text = $m.Groups[$g].Value
                $values[$f.Key] = if ($f.Value -eq 1) { [int64]$text } else { [int64[]]@($text -split '/' | ForEach-Object { [int64]$_ }) }
                $g++
            }
        }
        $counters[$key] = $values
    }

    return [pscustomobject]@{
        Counters   = $counters
        Iterations = $iterations.ToArray()
        Best       = ($best[0] -split ' ')[1]
    }
}

function Get-Stability {
    <# Best-move changes, settled iteration and the score-swing pairs of one search. #>
    param([Parameter(Mandatory)][object]$Record)

    $its = $Record.Iterations
    $changes = 0
    $settled = $its.Count
    $swings = [System.Collections.Generic.List[double]]::new()
    for ($i = 1; $i -lt $its.Count; $i++) {
        if ($its[$i].Move -ne $its[$i - 1].Move) { $changes++ }
        if ($null -ne $its[$i].Score -and $null -ne $its[$i - 1].Score) {
            $swings.Add([math]::Abs($its[$i].Score - $its[$i - 1].Score))
        }
    }
    for ($i = $its.Count - 1; $i -ge 0 -and $its[$i].Move -eq $Record.Best; $i--) { $settled = $its[$i].Depth }
    return [pscustomobject]@{ Changes = $changes; Settled = $settled; Swings = $swings.ToArray() }
}

function Get-ScopeRows {
    <#
        The rows of one scope, pooled over its records. Kind 'count' deltas as relative %,
        'rate' (already in %) as percentage points, 'abs' as an absolute difference.
    #>
    param([Parameter(Mandatory)][object[]]$Records)

    $sum = @{}
    foreach ($key in $ProfileSchema.Keys) {
        $sum[$key] = @{}
        foreach ($f in $ProfileSchema[$key].Fields.GetEnumerator()) {
            if ($f.Value -eq 1) {
                $sum[$key][$f.Key] = [int64]0
                foreach ($r in $Records) {
                    $v = $r.Counters[$key][$f.Key]
                    $sum[$key][$f.Key] = if ($key -eq 'qsearch' -and $f.Key -eq 'maxdepth') { [math]::Max($sum[$key][$f.Key], $v) } else { $sum[$key][$f.Key] + $v }
                }
            } else {
                $acc = [int64[]]::new($f.Value)
                foreach ($r in $Records) { for ($b = 0; $b -lt $f.Value; $b++) { $acc[$b] += $r.Counters[$key][$f.Key][$b] } }
                $sum[$key][$f.Key] = $acc
            }
        }
    }

    $rows = [System.Collections.Generic.List[object]]::new()
    function Add-Row([string]$Name, [string]$Kind, $Value) { $rows.Add([pscustomobject]@{ Name = $Name; Kind = $Kind; Value = $Value }) }
    function Pct($Num, $Den) { if ($Den -eq 0) { $null } else { 100.0 * $Num / $Den } }

    $t = $sum.treenodes; $o = $sum.ordering; $l = $sum.lmr; $nt = $sum.nodetypes
    $nm = $sum.nullmove; $pr = $sum.pruning; $q = $sum.qsearch; $a = $sum.aspiration
    $nodes = $t.main + $t.qs
    $bands = @('1-2', '3-6', '7+')

    Add-Row 'nodes' 'count' $nodes
    Add-Row 'qs share, % of nodes' 'rate' (Pct $t.qs $nodes)

    # Iterations: n(d) summed over records by depth. Every record reached the same depth.
    $maxD = @($Records[0].Iterations).Count
    $n = [int64[]]::new($maxD + 1)
    foreach ($r in $Records) {
        $prev = [int64]0
        foreach ($it in $r.Iterations) { $n[$it.Depth] += $it.Nodes - $prev; $prev = $it.Nodes }
    }
    Add-Row 'EBF, last 4 iterations' 'abs' $(if ($n[$maxD - 4] -gt 0) { [math]::Pow($n[$maxD] / $n[$maxD - 4], 0.25) } else { $null })
    for ($d = 1; $d -le $maxD; $d++) {
        Add-Row "iteration $d nodes" 'count' $n[$d]
        if ($d -gt 1) { Add-Row "iteration $d / $($d - 1)" 'abs' $(if ($n[$d - 1] -gt 0) { $n[$d] / $n[$d - 1] } else { $null }) }
    }

    Add-Row 'aspirated iterations' 'count' $a.iterations
    Add-Row 'aspiration fail-lows' 'count' $a.faillow
    Add-Row 'aspiration fail-highs' 'count' $a.failhigh
    Add-Row 'aspiration full windows' 'count' $a.fullwindow
    Add-Row 'aspiration failnodes, % of nodes' 'rate' (Pct $a.failnodes $nodes)

    Add-Row 'cuts' 'count' $o.cuts
    Add-Row 'first-move cuts, % of cuts' 'rate' (Pct $o.index[0] $o.cuts)
    $idx = @('1', '2', '3-5', '6+')
    for ($i = 0; $i -lt 4; $i++) { Add-Row "index $($idx[$i]), % of cuts" 'rate' (Pct $o.index[$i + 1] $o.cuts) }
    $late = ($o.latecut | Measure-Object -Sum).Sum
    $kinds = @('hash', 'capture', 'killer', 'quiet')
    for ($i = 0; $i -lt 4; $i++) { Add-Row "late cut $($kinds[$i]), % of late" 'rate' (Pct $o.latecut[$i] $late) }
    Add-Row 'hashnodes, % of cuts' 'rate' (Pct $o.hashnodes $o.cuts)
    Add-Row 'hashcuts, % of hashnodes' 'rate' (Pct $o.hashcuts $o.hashnodes)
    Add-Row 'latenodes, % of nodes' 'rate' (Pct $o.latenodes $nodes)
    for ($i = 0; $i -lt 3; $i++) { Add-Row "latenodes depth $($bands[$i]), % of nodes" 'rate' (Pct $o.latebands[$i] $nodes) }

    Add-Row 'lmr reduced' 'count' $l.reduced
    Add-Row 'reducednodes, % of nodes' 'rate' (Pct $l.reducednodes $nodes)
    Add-Row 'researched, % of reduced' 'rate' (Pct $l.researched $l.reduced)
    Add-Row 'researchnodes, % of nodes' 'rate' (Pct $l.researchnodes $nodes)
    Add-Row 'confirmed, % of researched' 'rate' (Pct $l.confirmed $l.researched)

    $pvF  = ($nt.pv  | Measure-Object -Sum).Sum
    $cutF = ($nt.cut | Measure-Object -Sum).Sum
    $allF = ($nt.all | Measure-Object -Sum).Sum
    $frames = $pvF + $cutF + $allF
    Add-Row 'frames' 'count' $frames
    Add-Row 'PV frames, % of frames' 'rate' (Pct $pvF $frames)
    Add-Row 'cut frames, % of frames' 'rate' (Pct $cutF $frames)
    Add-Row 'all frames, % of frames' 'rate' (Pct $allF $frames)
    Add-Row 'cutfaillow, % of cut frames' 'rate' (Pct (($nt.cutfaillow | Measure-Object -Sum).Sum) $cutF)
    for ($i = 0; $i -lt 3; $i++) { Add-Row "cutfaillow depth $($bands[$i]), % of cut" 'rate' (Pct $nt.cutfaillow[$i] $nt.cut[$i]) }

    Add-Row 'null move tried' 'count' $nm.tried
    Add-Row 'null cutoffs, % of tried' 'rate' (Pct $nm.cutoffs $nm.tried)
    Add-Row 'null failed, % of tried' 'rate' (Pct $nm.failed $nm.tried)
    Add-Row 'null failnodes, % of nodes' 'rate' (Pct $nm.failnodes $nodes)

    $rfpBins = @('1', '2', '3', '4', '5', '6+')
    for ($i = 0; $i -lt 6; $i++) { Add-Row "rfp cutoffs depth $($rfpBins[$i])" 'count' $pr.rfp[$i] }
    Add-Row 'frontier floor binds' 'count' $pr.floorbinds
    Add-Row 'frontier skips' 'count' $sum.frontier.skips
    Add-Row 'lmp skips' 'count' $sum.lmp.skips

    Add-Row 'qs roots' 'count' $q.roots
    Add-Row 'qs nodes per root' 'abs' $(if ($q.roots -gt 0) { $t.qs / $q.roots } else { $null })
    Add-Row 'qs delta prunes' 'count' $q.delta
    Add-Row 'qs SEE prunes' 'count' $q.see
    Add-Row 'qs maxdepth' 'abs' $q.maxdepth

    $stab = @($Records | ForEach-Object { Get-Stability $_ })
    Add-Row 'best-move changes' 'count' ([int64](($stab | Measure-Object -Property Changes -Sum).Sum))
    Add-Row 'settled iteration, mean' 'abs' (($stab | Measure-Object -Property Settled -Average).Average)
    $allSwings = @($stab | ForEach-Object { $_.Swings })
    Add-Row 'score swing, mean cp' 'abs' $(if ($allSwings.Count -gt 0) { ($allSwings | Measure-Object -Average).Average } else { $null })

    return $rows.ToArray()
}

# Invariant culture: the tables are pasted into PR bodies, so a host's decimal comma must not leak in.
function Format-Invariant([string]$Format, $Value) { [string]::Format([cultureinfo]::InvariantCulture, $Format, $Value) }

function Format-Value {
    param([string]$Kind, $Value)
    if ($null -eq $Value) { return '-' }
    switch ($Kind) {
        'count' { return Format-Invariant '{0:N0}' $Value }
        'rate'  { return Format-Invariant '{0:N1}%' $Value }
        default { return Format-Invariant '{0:N2}' $Value }
    }
}

function Format-Delta {
    param([string]$Kind, $BeforeValue, $AfterValue)
    if ($null -eq $BeforeValue -or $null -eq $AfterValue) { return 'n/a' }
    switch ($Kind) {
        'count' {
            if ($BeforeValue -eq 0) { return $(if ($AfterValue -eq 0) { '0.0%' } else { 'n/a' }) }
            return Format-Invariant '{0:+0.0;-0.0;0.0}%' (100.0 * ($AfterValue - $BeforeValue) / $BeforeValue)
        }
        'rate'  { return Format-Invariant '{0:+0.0;-0.0;0.0}pp' ($AfterValue - $BeforeValue) }
        default { return Format-Invariant '{0:+0.00;-0.00;0.00}' ($AfterValue - $BeforeValue) }
    }
}

function Format-ScopeTable {
    param(
        [Parameter(Mandatory)][string]$Title,
        [Parameter(Mandatory)][object[]]$BeforeRows,
        [Parameter(Mandatory)][object[]]$AfterRows
    )
    $out = [System.Collections.Generic.List[string]]::new()
    $out.Add('')
    $out.Add("== $Title")
    $out.Add(('{0,-40} {1,15} {2,15} {3,10}' -f 'measure', 'before', 'after', 'delta'))
    for ($i = 0; $i -lt $BeforeRows.Count; $i++) {
        $b = $BeforeRows[$i]; $a = $AfterRows[$i]
        $out.Add(('{0,-40} {1,15} {2,15} {3,10}' -f $b.Name, (Format-Value $b.Kind $b.Value),
                  (Format-Value $a.Kind $a.Value), (Format-Delta $b.Kind $b.Value $a.Value)))
    }
    return $out.ToArray()
}

function Test-Endgame {
    <# Each side's non-pawn material at most 13 (N, B 3; R 5; Q 9). #>
    param([Parameter(Mandatory)][string]$Fen)

    $value = @{ n = 3; b = 3; r = 5; q = 9 }
    $white = 0; $black = 0
    foreach ($c in (($Fen -split ' ')[0]).ToCharArray()) {
        $k = [string]$c
        if (-not $value.ContainsKey($k.ToLower())) { continue }
        if ([char]::IsUpper($c)) { $white += $value[$k.ToLower()] } else { $black += $value[$k.ToLower()] }
    }
    return ($white -le 13 -and $black -le 13)
}

# ---------------------------------------------------------------------------
# Self-test
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

    # A real profile transcript: kiwipete, depth 8.
    $kiwi = @(
        'info string benchcontract 2'
        'info depth 1 score cp 99 nodes 1020 hashfull 0 time 0 pv e2a6'
        'info depth 2 score cp 99 nodes 2237 hashfull 0 time 1 pv e2a6 b4c3'
        'info depth 3 score cp 43 nodes 5610 hashfull 0 time 2 pv e2a6 b4c3 d2c3'
        'info depth 4 score cp 43 nodes 9261 hashfull 0 time 4 pv e2a6 b4c3 d2c3 h3g2'
        'info depth 5 score cp 4 nodes 37252 hashfull 1 time 16 pv d5e6 a6e2 e6f7 e8d8 e1e2'
        'info depth 6 score cp 4 nodes 63165 hashfull 2 time 28 pv d5e6 a6e2 e6f7 e8d8 e1e2 e7e5'
        'info depth 7 score cp 2 nodes 113560 hashfull 2 time 49 pv d5e6 e7e6 e2a6 h3g2 f3g2 e6e5 f2f4'
        'info depth 8 score cp 0 nodes 194493 hashfull 8 time 86 pv d5e6 e7e6 e2a6 h3g2 f3g2 e6e5 f2f4 e5d4'
        'info depth 8 score cp 0 nodes 194493 hashfull 8 time 86 pv d5e6'
        'info string treenodes main 83904 qs 110589'
        'info string frontier skips 43487'
        'info string lmp skips 23941'
        'info string aspiration iterations 7 faillow 1 failhigh 0 fullwindow 0 failnodes 1813'
        'info string ordering cuts 7713 index 7412/126/103/52/20 latecut 0/260/21/20 hashnodes 3249 hashcuts 3229 latenodes 15403 latebands 12329/3074/0'
        'info string lmr reduced 26106 reducednodes 8560 researched 1 confirmed 1 researchnodes 78'
        'info string nodetypes pv 28/25/3 cut 41779/4324/47 all 3653/951/0 cutfaillow 179/11/0'
        'info string nullmove tried 3729 cutoffs 1169 failed 2560 failnodes 36257'
        'info string pruning rfp 33663/2017/1250/0/0/0 floorbinds 0'
        'info string qsearch roots 36849 delta 38697 see 33414 maxdepth 17'
        'bestmove d5e6'
    ) -join "`n"
    function Parse([string]$Text) { ConvertFrom-ProfileTranscript -Output $Text -SearchDepth 8 -Side 'after' -Position 'kiwi' }
    function Row([object[]]$Rows, [string]$Name) { @($Rows | Where-Object { $_.Name -eq $Name })[0].Value }

    $r = Parse $kiwi
    Assert-Case 'histograms parse to their bins' (($r.Counters.ordering.index -join '/') -eq '7412/126/103/52/20' -and ($r.Counters.pruning.rfp -join '/') -eq '33663/2017/1250/0/0/0')
    Assert-Case 'aspirated iterations and null failures are read' ($r.Counters.aspiration.iterations -eq 7 -and $r.Counters.nullmove.failed -eq 2560)
    Assert-Case 'the repeated summary line is not an iteration' ($r.Iterations.Count -eq 8 -and $r.Iterations[7].Nodes -eq 194493)
    Assert-Case 'bestmove is read' ($r.Best -eq 'd5e6')

    $rows = Get-ScopeRows @($r)
    $ebf = Row $rows 'EBF, last 4 iterations'
    Assert-Case 'EBF is (n(8)/n(4))^(1/4) over iteration nodes' ([math]::Abs($ebf - 2.16984) -lt 0.0001) "got $ebf"
    Assert-Case 'iteration nodes are cumulative differences' ((Row $rows 'iteration 5 nodes') -eq 27991 -and (Row $rows 'iteration 1 nodes') -eq 1020)
    Assert-Case 'best move changed once, settled at 5' ((Row $rows 'best-move changes') -eq 1 -and (Row $rows 'settled iteration, mean') -eq 5)
    Assert-Case 'score swing is the mean absolute step' ([math]::Abs((Row $rows 'score swing, mean cp') - 99 / 7) -lt 1e-9)
    Assert-Case 'first-move rate is I0 over cuts' ([math]::Abs((Row $rows 'first-move cuts, % of cuts') - 100.0 * 7412 / 7713) -lt 1e-9)
    Assert-Case 'nodes are main plus qs' ((Row $rows 'nodes') -eq 194493)
    Assert-Case 'qs nodes per root' ([math]::Abs((Row $rows 'qs nodes per root') - 110589 / 36849) -lt 1e-9)

    $mateStep = $kiwi -replace 'depth 7 score cp 2', 'depth 7 score mate 5'
    $rows = Get-ScopeRows @(Parse $mateStep)
    Assert-Case 'a pair with a mate score is excluded from the swing' ([math]::Abs((Row $rows 'score swing, mean cp') - 95 / 5) -lt 1e-9)

    # Optional lines: absent reads as zero.
    $silent = $kiwi -replace "info string (nullmove|pruning|lmr|aspiration|frontier|lmp) [^\n]*\n", ''
    $rows = Get-ScopeRows @(Parse $silent)
    Assert-Case 'absent optional lines read as zeros' ((Row $rows 'null move tried') -eq 0 -and (Row $rows 'lmr reduced') -eq 0 -and (Row $rows 'lmp skips') -eq 0 -and $null -eq (Row $rows 'null cutoffs, % of tried'))

    # Pooling: sums, except maxdepth by max.
    $other = $kiwi -replace 'maxdepth 17', 'maxdepth 9' -replace 'roots 36849', 'roots 1'
    $pool = Get-ScopeRows @((Parse $kiwi), (Parse $other))
    Assert-Case 'maxdepth pools by max' ((Row $pool 'qs maxdepth') -eq 17)
    Assert-Case 'counts pool by sum' ((Row $pool 'qs roots') -eq 36850 -and (Row $pool 'iteration 8 nodes') -eq 2 * 80933)

    # Refusals.
    foreach ($key in @('treenodes', 'ordering', 'nodetypes', 'qsearch')) {
        $missing = $kiwi -replace "info string $key [^\n]*\n", ''
        Assert-Case "FALSIFY: a missing '$key' line is refused, naming side and position" `
            (Test-Refuses -Match "after build, position kiwi: no 'info string $key' line" { Parse $missing })
    }
    $default = $kiwi -replace "info string (ordering|lmr|nodetypes|nullmove|pruning|qsearch) [^\n]*\n", ''
    Assert-Case 'FALSIFY: a default-build transcript is refused' (Test-Refuses -Match "no 'info string ordering' line" { Parse $default })
    $pr2a = $kiwi -replace "info string (nodetypes|nullmove|pruning|qsearch) [^\n]*\n", ''
    Assert-Case 'FALSIFY: a transcript from before the four-line addition is refused' (Test-Refuses -Match "no 'info string nodetypes' line" { Parse $pr2a })
    Assert-Case 'FALSIFY: a short histogram is refused' (Test-Refuses -Match 'malformed line' { Parse ($kiwi -replace 'latebands 12329/3074/0', 'latebands 12329/3074') })
    Assert-Case 'FALSIFY: a malformed optional line is refused' (Test-Refuses -Match 'malformed line' { Parse ($kiwi -replace 'failed 2560', 'fails 2560') })
    Assert-Case 'FALSIFY: an unfinished search is refused' (Test-Refuses -Match 'did not complete depth 9' { ConvertFrom-ProfileTranscript -Output $kiwi -SearchDepth 9 -Side 'before' -Position 'x' })
    Assert-Case 'FALSIFY: a missing bestmove is refused' (Test-Refuses -Match 'did not complete depth 8' { Parse ($kiwi -replace 'bestmove d5e6', '') })
    Assert-Case 'FALSIFY: a missing iteration is refused' (Test-Refuses -Match "no 'info depth 3' line" { Parse ($kiwi -replace "info depth 3 [^\n]*\n", '') })

    # Delta formats.
    Assert-Case 'count delta is relative' ((Format-Delta 'count' 200 150) -eq '-25.0%')
    Assert-Case 'rate delta is in points' ((Format-Delta 'rate' 93.1 93.4) -eq '+0.3pp')
    Assert-Case 'abs delta is a difference' ((Format-Delta 'abs' 2.5 2.25) -eq '-0.25')
    Assert-Case 'identical values delta to zero' ((Format-Delta 'count' 5 5) -eq '0.0%' -and (Format-Delta 'rate' 1.5 1.5) -eq '0.0pp')
    Assert-Case 'a zero count base is n/a' ((Format-Delta 'count' 0 4) -eq 'n/a' -and (Format-Delta 'count' 0 0) -eq '0.0%')
    Assert-Case 'an undefined rate is n/a' ((Format-Delta 'rate' $null 3.0) -eq 'n/a')

    # Grouping: the built-in endgames, and a queen ending.
    $endgames = @($DefaultPositions | Where-Object { Test-Endgame $_.Fen } | ForEach-Object { $_.Name })
    Assert-Case 'built-in endgames are rook-endgm and piece-endgm' (($endgames -join ',') -eq 'rook-endgm,piece-endgm') "got $($endgames -join ',')"
    Assert-Case 'a queen ending is an endgame' (Test-Endgame '6k1/5ppp/8/8/8/8/5PPP/3Q2K1 w - - 0 1')
    Assert-Case 'queen and rook is not' (-not (Test-Endgame '3r2k1/5ppp/8/8/8/8/5PPP/3QR1K1 w - - 0 1'))

    Write-Host ''
    if ($failures -gt 0) {
        Write-Host "$failures self-test case(s) FAILED." -ForegroundColor Red
        exit 1
    }
    Write-Host 'All self-test cases passed.' -ForegroundColor Green
    exit 0
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if (-not $Before -or -not $After) { throw '-Before and -After are required (two STRAT_SEARCH_PROFILE builds; the same one twice is the self-check).' }

. (Join-Path $PSScriptRoot 'UciDriver.ps1')

$sides = [ordered]@{ before = (Resolve-Path $Before).Path; after = (Resolve-Path $After).Path }
$positionList = @(Resolve-Positions -Path $Positions)

$workDir = Join-Path ([System.IO.Path]::GetTempPath()) 'StratChessProfile-run'
New-Item -ItemType Directory -Force -Path $workDir | Out-Null

Write-Host ''
foreach ($s in $sides.GetEnumerator()) {
    $hash = (Get-FileHash -Path $s.Value -Algorithm SHA256).Hash.Substring(0, 12).ToLower()
    Write-Host ('{0,-8}: {1}  (sha {2})' -f $s.Key, $s.Value, $hash)
}
$setName = if ($Positions) { Split-Path -Leaf $Positions } else { 'builtin' }
Write-Host "Depth   : $Depth    Threads: 1    Set: $setName sha $(Get-PositionSetHash -List $positionList)    Positions: $($positionList.Count)"

$records = @{ before = [System.Collections.Generic.List[object]]::new(); after = [System.Collections.Generic.List[object]]::new() }
foreach ($p in $positionList) {
    foreach ($s in $sides.GetEnumerator()) {
        $commands = @('uci', 'isready', 'setoption name Threads value 1', "position fen $($p.Fen)", "go depth $Depth")
        $out = Invoke-UciSearchToBestMove -ExePath $s.Value -WorkDir $workDir -Commands $commands `
                                          -SearchDepth $Depth -Description $p.Fen
        $rec = ConvertFrom-ProfileTranscript -Output $out -SearchDepth $Depth -Side $s.Key -Position $p.Name
        $rec | Add-Member -NotePropertyName Name -NotePropertyValue $p.Name
        $rec | Add-Member -NotePropertyName Endgame -NotePropertyValue (Test-Endgame $p.Fen)
        $records[$s.Key].Add($rec)
    }
    Write-Host "  searched $($p.Name)" -ForegroundColor DarkGray
}

$scopes = [System.Collections.Generic.List[object]]::new()
$scopes.Add(@{ Title = 'Pooled'; Filter = { $true } })
$scopes.Add(@{ Title = 'Endgame'; Filter = { $_.Endgame } })
$scopes.Add(@{ Title = 'Non-endgame'; Filter = { -not $_.Endgame } })
foreach ($p in $positionList) {
    $name = $p.Name
    $scopes.Add(@{ Title = $name; Filter = { $_.Name -eq $name }.GetNewClosure() })
}

foreach ($scope in $scopes) {
    $b = @($records.before | Where-Object $scope.Filter)
    if ($b.Count -eq 0) { continue }
    $a = @($records.after | Where-Object $scope.Filter)
    $names = ($b | ForEach-Object { $_.Name }) -join ', '
    $title = if ($scope.Title -in @('Pooled', 'Endgame', 'Non-endgame')) { "$($scope.Title) ($names)" } else { $scope.Title }
    Format-ScopeTable -Title $title -BeforeRows (Get-ScopeRows $b) -AfterRows (Get-ScopeRows $a) | Write-Host
}
