<#
.SYNOPSIS
    Before/after CPU profile of two refs: where the engine's time goes, grouped into areas
    (evaluation, move ordering, move generation, TT, board, search bodies), as a markdown table.

.DESCRIPTION
    Builds a profiling variant of each ref, samples the fixed-depth bench positions under
    VSDiagnostics (1 kHz, no elevation), and reads each function's self time with xperf.

    Variant build. Plain Release writes no PDB, and LTO internalises pvs, quiescence and the TT
    functions, so without full debug info their samples land on an unrelated public symbol. Each
    ref is built with the windows-clang-cl preset plus /Z7 and /DEBUG /OPT:REF /OPT:ICF, which
    keeps Release's image and node counts and adds a PDB. The VS environment is imported first:
    a flags-override configure without it empties CMAKE_CXX_FLAGS.

    Runs. One engine process per run, Threads=1, the BenchPositions.ps1 set with ucinewgame
    between positions. The baseline is profiled -BeforeRuns times, interleaved before/after/before,
    and the per-area spread across those runs is the noise floor: a delta inside it is not a
    finding. Nodes and best moves of every run are compared with the first; a mismatch is
    reported, not fatal, since a behaviour change is a legitimate thing to profile.

    Reading the shares. Sampling slows the engine, so shares are approximate and no nps from
    this script is a measurement (use Compare-Bench.ps1). An inlined helper is counted in its
    caller's area. Areas match the qualified function name only, never parameter types. Shares
    of one build drift between sessions by more than the within-run spread, so compare arms of
    one run, never a table against an earlier session's.

    Artifacts. The exe, PDB, .etl and text outputs of each run stay in one output directory,
    never deleted automatically: the .etl resolves symbols only while its PDB exists.

    Quiet machine. Only profile collection needs an idle machine; builds and xperf analysis just
    run slower under load. The script prints the phase plan first and a QUIET NEEDED / Machine
    free banner at each boundary.

    Linux (-Linux). The strength lab's toolchain: GCC Release plus -g in WSL Ubuntu-26.04,
    sampled with `perf record -F 1000 -e cycles:u` and read with `perf report --no-children
    --sort sym`. -g leaves GCC's code unchanged, so it is the lab's binary with symbols. Each ref
    is exported as a tar (git archive; a dirty worktree through a temporary index) and built on
    WSL's ext4 under ~/strat-cpu-profile, then deleted: FetchContent fails over /mnt/c, and a
    worktree's .git file holds a Windows path WSL cannot follow. WSL is driven through a generated
    .sh with `wsl --exec`, never shell text, whose quoting mangles backslashes. perf cannot write
    its data to /mnt/c ("Bad address"), so it records on ext4 and the file is copied out. The
    engine runs from the output directory's bin\, which perf.data names, so keep the two together.
    Shares differ from Windows in more than codegen (clang-cl inlines the TT probe into the
    search), so compare a Linux table only with another Linux table.

.PARAMETER Before
    Baseline ref: a worktree path (built as it stands, uncommitted changes included) or any
    commit-ish of this repository (checked out to a temporary detached worktree).

.PARAMETER After
    Candidate ref, in the same forms as -Before. The same ref as -Before profiles one build, and
    its table is an A/A noise check.

.PARAMETER Depth
    Fixed search depth. Default 13.

.PARAMETER BeforeRuns
    Profile runs of the baseline, for the noise floor. Default 2; 1 skips the spread.

.PARAMETER Callers
    Regex naming a symbol whose samples to split by caller (for example '_Sort_unchecked'),
    from xperf's butterfly view of the first before run and the after run. Windows only: the
    Linux arm records no call graphs.

.PARAMETER Linux
    Profile GCC builds in WSL (Ubuntu-26.04) with perf instead of clang-cl builds with
    VSDiagnostics.

.PARAMETER Positions
    Optional FEN file, one per line. Defaults to the BenchPositions.ps1 set.

.PARAMETER OutDir
    Output directory. Defaults to build\cpu-profile\<timestamp> in this repository. Must be
    empty or absent.

.PARAMETER Reanalyse
    An earlier run's output directory: rebuild its report from the saved traces, for example
    with a new -Callers, without building or collecting. -Before, -After, -Depth and -Linux are
    read from its metadata.json.

.PARAMETER SelfTest
    Assert the xperf and perf parsers, the area grouping and the quiet-window formatting on
    synthetic text, and exit. Runs no build, engine or profiler. Exits 1 on any failure.

.EXAMPLE
    .\Measure-CpuProfile.ps1 -Before origin/main -After C:\src\my-worktree

.EXAMPLE
    .\Measure-CpuProfile.ps1 -Before 1f93312 -After 2a6062b -Callers '_Sort_unchecked'

.EXAMPLE
    .\Measure-CpuProfile.ps1 -Reanalyse build\cpu-profile\20261006-121806 -Callers 'MoveSorter::'

.EXAMPLE
    .\Measure-CpuProfile.ps1 -Before 79c3217 -After origin/main -Linux
#>
[CmdletBinding()]
param(
    [string]$Before = '',

    [string]$After = '',

    [ValidateRange(1, 30)]
    [int]$Depth = 13,

    [ValidateRange(1, 5)]
    [int]$BeforeRuns = 2,

    [string]$Callers = '',

    [string]$Positions = '',

    [string]$OutDir = '',

    [string]$Reanalyse = '',

    [switch]$Linux,

    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# The report is pasted into issues: '12.3%', not the machine locale's '12,3%'.
[System.Threading.Thread]::CurrentThread.CurrentCulture = [cultureinfo]::InvariantCulture

. (Join-Path $PSScriptRoot 'BenchPositions.ps1')
. (Join-Path $PSScriptRoot 'QuietWindow.ps1')

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$EngineProcess = 'StratChessEvolved'

# The areas, first match wins. Case-sensitive, and matched against Get-SymbolName's output.
$AreaPatterns = [ordered]@{
    'TT locks'        = 'SRWLock|RtlAcquireSRW|RtlReleaseSRW|RtlpWakeSRW|RtlpWaitOnAddress|pthread_rwlock|__pthread_rwlock'
    'Move ordering'   = 'MoveSorter|_Sort_unchecked|_Insertion_sort|_Partition_by_median|_Make_heap|_Pop_heap|_Sort_heap|_Med3|_Guess_median|__introsort|__insertion_sort|__unguarded|__adjust_heap|__heap_select|__move_median|__partial_sort|std::sort|See::|see_ge'
    'Evaluation'      = 'Eval|evaluate'
    'Move generation' = 'MoveGenerator|Magic|GetAttackBoard'
    'TT probe/store'  = 'TranspositionTable'
    'Board'           = 'Board::|Zobrist'
    'Search bodies'   = 'AIPerplex|ThreadData|quiescence|pvs'
}
$OtherArea = 'Other'

# The move list's std::sort instantiation, reported on its own row.
$SortSymbolPattern = '_Sort_unchecked<std::pair<int,\s*int>|__introsort_loop|__insertion_sort|__final_insertion_sort'

# Estimates used until the run has measured its own.
$RoughBuildSeconds = 240
$RoughRunSeconds = 30
$RoughAnalysisSeconds = 60

$WslDistro = 'Ubuntu-26.04'

# Builds one -Linux arm on ext4 and copies the exe out; the tree is deleted on exit. Written with
# LF endings next to the arm's outputs, so bash never sees a CR.
$WslBuildScript = @'
#!/usr/bin/env bash
# Usage: build.sh <source.tar> <bin dir> <work name>
set -euo pipefail
tar_path=$1; bin_dir=$2; name=$3
root="$HOME/strat-cpu-profile"
work="$root/$name"
rm -rf "$work"
mkdir -p "$work/src" "$bin_dir"
trap 'rm -rf "$work"' EXIT
tar -xf "$tar_path" -C "$work/src"
cmake -S "$work/src" -B "$work/build" -G Ninja --log-level=WARNING -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS=-g "-DFETCHCONTENT_BASE_DIR=$root/deps"
cmake --build "$work/build" --target StratChessEvolved --parallel
cp "$work/build/StratChessEvolved" "$bin_dir/"
grep -E '^CMAKE_(CXX_COMPILER|CXX_FLAGS|BUILD_TYPE):' "$work/build/CMakeCache.txt"
"$(grep -E '^CMAKE_CXX_COMPILER:' "$work/build/CMakeCache.txt" | cut -d= -f2)" --version | head -1
'@ -replace "`r`n", "`n"

function Get-SymbolName {
    <#
        The part of a symbol the area patterns may see: no module prefix, no lambda source path
        (a worktree or file name could match a pattern), and nothing from the first top-level '('
        on, since GCC's demangled names list parameter types.
    #>
    param([Parameter(Mandatory)][string]$Symbol)

    $s = $Symbol -replace '^[\w.-]+\.(exe|dll|sys)!', ''
    $s = $s -replace 'lambda at [^''>]*', 'lambda'
    $nesting = 0
    for ($i = 0; $i -lt $s.Length; $i++) {
        $ch = $s[$i]
        if ($ch -eq '(' -and $nesting -eq 0 -and $i -gt 0 -and -not $s.Substring(0, $i).EndsWith('operator')) {
            return $s.Substring(0, $i)
        }
        if ($ch -eq '<') { $nesting++ }
        elseif ($ch -eq '>') { $nesting-- }
    }
    return $s
}

function Get-SymbolArea {
    param([Parameter(Mandatory)][string]$Symbol)
    $name = Get-SymbolName -Symbol $Symbol
    foreach ($area in $AreaPatterns.Keys) {
        if ($name -cmatch $AreaPatterns[$area]) { return $area }
    }
    return $OtherArea
}

function ConvertFrom-XperfProfile {
    <# Self weight per symbol for the engine process, from `xperf -a profile -detail` text. #>
    param([Parameter(Mandatory)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Line)

    $weight = @{}
    foreach ($l in $Line) {
        if ($l -match "^\s*$EngineProcess\.exe \(\s*\d+\),\s*(\d+),\s*[\d.]+,\s*(.+?)\s*$") {
            $symbol = $Matches[2]
            $weight[$symbol] += [double]$Matches[1]
        }
    }
    return $weight
}

function ConvertFrom-PerfReport {
    <#
        Self overhead per symbol, from `perf report --stdio --no-children --sort sym` text. The
        overhead column is cycles; the optional samples column (-n) is not, at -F's adaptive period.
    #>
    param([Parameter(Mandatory)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Line)

    $weight = @{}
    foreach ($l in $Line) {
        if ($l -match '^\s*([\d.]+)%\s+(?:\d+\s+)?\[[.k]\]\s+(.+?)\s*$') {
            $symbol = $Matches[2]
            $weight[$symbol] += [double]$Matches[1]
        }
    }
    return $weight
}

function Get-AreaShare {
    <# Percent of the process's total self weight per area, plus the sort symbol's own share. #>
    param([Parameter(Mandatory)][hashtable]$Weight)

    $total = ($Weight.Values | Measure-Object -Sum).Sum
    if (-not $total) { throw 'No samples for the engine process in this trace.' }
    $share = [ordered]@{}
    foreach ($area in @($AreaPatterns.Keys) + $OtherArea) { $share[$area] = 0.0 }
    $sort = 0.0
    foreach ($symbol in $Weight.Keys) {
        $share[(Get-SymbolArea -Symbol $symbol)] += $Weight[$symbol] / $total * 100
        if ($symbol -match $SortSymbolPattern) { $sort += $Weight[$symbol] / $total * 100 }
    }
    return [pscustomobject]@{ Area = $share; Sort = $sort; Total = $total }
}

function Get-Spread {
    <# Mean and max-min of a list of values; spread is $null for a single value. #>
    param([Parameter(Mandatory)][double[]]$Value)
    $stats = $Value | Measure-Object -Average -Minimum -Maximum
    $spread = if ($Value.Count -gt 1) { $stats.Maximum - $stats.Minimum } else { $null }
    return [pscustomobject]@{ Mean = $stats.Average; Spread = $spread }
}

function ConvertFrom-XperfButterfly {
    <#
        Caller weights of every symbol matching $Pattern, from the HTML of `xperf -a stack
        -butterfly`. Its callers-and-callees table lists each function row, then its callers
        ('<--') and callees ('-->'), each with inclusive hits.
    #>
    param(
        [Parameter(Mandatory)][string]$Html,
        [Parameter(Mandatory)][string]$Pattern
    )

    # The heading, not the table of contents' link to it.
    $start = $Html.IndexOf('Functions by Multi-Inclusive Hits with Callers and Callees</h2>')
    if ($start -lt 0) { throw 'No callers-and-callees table in the xperf stack report.' }
    $end = $Html.IndexOf('</table>', $start)
    $table = $Html.Substring($start, $(if ($end -gt 0) { $end - $start } else { $Html.Length - $start }))

    $byCaller = @{}
    $current = $null
    foreach ($row in [regex]::Matches($table, '<tr(?: class=''(?<class>\w+)'')?><td>(?<name>.*?)</td><td>(?<hits>\d*)</td>')) {
        # Symbol names carry raw template brackets, so strip only the report's own tags.
        $name = [System.Net.WebUtility]::HtmlDecode(($row.Groups['name'].Value -replace '</?(a|td|tr)\b[^>]*>', '')).Trim()
        $hits = if ($row.Groups['hits'].Value) { [double]$row.Groups['hits'].Value } else { 0 }
        $class = $row.Groups['class'].Value
        if (-not $class -or $class -eq 'pp') {
            $current = if ($name -match $Pattern) { $name } else { $null }
            continue
        }
        # A sort calls itself on partitions; that is not a caller worth attributing.
        if ($current -and $name -match '^<--\s*(.+)$' -and $Matches[1] -ne $current) {
            $caller = $Matches[1]
            $byCaller[$caller] += $hits
        }
    }
    return $byCaller
}

function Format-AreaTable {
    <# Markdown rows: before mean, after, delta and the before spread per area, then the sort symbol. #>
    param(
        [Parameter(Mandatory)][object[]]$BeforeShare,
        [Parameter(Mandatory)][object]$AfterShare
    )

    $hasSpread = $BeforeShare.Count -gt 1
    $head = '| Area | Before | After | Δ points |' + $(if ($hasSpread) { ' Before spread |' } else { '' })
    $rule = '|---|---:|---:|---:|' + $(if ($hasSpread) { '---:|' } else { '' })
    $rows = foreach ($area in @($AreaPatterns.Keys) + $OtherArea + 'sort symbol') {
        $values = if ($area -eq 'sort symbol') { @($BeforeShare | ForEach-Object { $_.Sort }) } else { @($BeforeShare | ForEach-Object { $_.Area[$area] }) }
        $afterValue = if ($area -eq 'sort symbol') { $AfterShare.Sort } else { $AfterShare.Area[$area] }
        $stats = Get-Spread -Value $values
        $line = '| {0} | {1:N1}% | {2:N1}% | {3:+0.0;-0.0;0.0} |' -f $area, $stats.Mean, $afterValue, ($afterValue - $stats.Mean)
        if ($hasSpread) { $line += ' {0:N1} |' -f $stats.Spread }
        $line
    }
    return @($head, $rule) + @($rows)
}

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

    $lambdaPath = '`lambda at C:\src\StratEngine\Eval.cpp:12:5'''
    $areaCases = @(
        @{ Symbol = 'ntdll.dll!RtlReleaseSRWLockShared';                         Expect = 'TT locks' }
        @{ Symbol = 'StratChessEvolved.exe!Evaluator::eval_pawns';                 Expect = 'Evaluation' }
        @{ Symbol = 'StratChessEvolved.exe!See::see_ge';                           Expect = 'Move ordering' }
        @{ Symbol = "StratChessEvolved.exe!std::_Sort_unchecked<std::pair<int,int> *,std::_Ref_fn<$lambdaPath> >"; Expect = 'Move ordering' }
        @{ Symbol = 'StratChessEvolved.exe!MoveGenerator::GetAttackBoard';         Expect = 'Move generation' }
        @{ Symbol = 'StratChessEvolved.exe!TranspositionTable::store';             Expect = 'TT probe/store' }
        @{ Symbol = 'StratChessEvolved.exe!Board::DoMove';                         Expect = 'Board' }
        @{ Symbol = 'StratChessEvolved.exe!AIPerplex::pvs';                        Expect = 'Search bodies' }
        @{ Symbol = 'vcruntime140.dll!memset';                                     Expect = 'Other' }
        # GCC lists parameter types: the TT reference must not make pvs TT work.
        @{ Symbol = 'AIPerplex::pvs(ThreadData&, TranspositionTable&, int, int)';  Expect = 'Search bodies' }
        @{ Symbol = 'Foo::operator()(Board const&)';                               Expect = 'Other' }
        # FALSIFY: a lambda's source path names Eval.cpp, but the function is not evaluation.
        @{ Symbol = "StratChessEvolved.exe!std::_Func_impl<$lambdaPath>::_Do_call"; Expect = 'Other' }
        # FALSIFY: the module name holds 'Evolved' and the patterns are case-sensitive.
        @{ Symbol = 'StratChessEvolved.exe!_Smtx_unlock_shared';                   Expect = 'Other' }
    )
    foreach ($case in $areaCases) {
        $got = Get-SymbolArea -Symbol $case.Symbol
        Assert-Case "area: $($case.Symbol.Substring(0, [math]::Min(60, $case.Symbol.Length))) -> $($case.Expect)" ($got -eq $case.Expect) "got $got"
    }
    Assert-Case 'name: operator() keeps its parentheses' ((Get-SymbolName -Symbol 'Foo::operator()(int)') -eq 'Foo::operator()')

    $flat = @(
        '   Process Name ( PID),     Weight,    Usage %,          Module Name!Function Name'
        '           Idle (   0),     192694,       0.03,         ntoskrnl.exe!SwapContext'
        'StratChessEvolved.exe (48708),    3000,       0.36, StratChessEvolved.exe!Evaluator::BuildContext'
        'StratChessEvolved.exe (48708),    1000,       0.19,            ntdll.dll!RtlReleaseSRWLockShared'
        'StratChessEvolved.exe (48708),     500,       0.10, StratChessEvolved.exe!Evaluator::BuildContext'
        'StratChessEvolved.exe (48708),     500,       0.27, StratChessEvolved.exe!std::_Sort_unchecked<std::pair<int,int> *,std::_Ref_fn<x> >'
        '        explorer.exe ( 4242),    9999,       1.00, StratChessEvolved.exe!AIPerplex::pvs'
    )
    $weight = ConvertFrom-XperfProfile -Line $flat
    Assert-Case 'xperf flat: engine rows only, duplicates summed' ($weight.Count -eq 3 -and $weight['StratChessEvolved.exe!Evaluator::BuildContext'] -eq 3500) "got $($weight.Count) symbols"
    $share = Get-AreaShare -Weight $weight
    Assert-Case 'shares: evaluation 70%, locks 20%, ordering 10%, sort 10%' (
        [math]::Abs($share.Area['Evaluation'] - 70) -lt 1e-9 -and [math]::Abs($share.Area['TT locks'] - 20) -lt 1e-9 -and
        [math]::Abs($share.Area['Move ordering'] - 10) -lt 1e-9 -and [math]::Abs($share.Sort - 10) -lt 1e-9) `
        "got $($share.Area['Evaluation'])/$($share.Area['TT locks'])/$($share.Area['Move ordering'])/$($share.Sort)"
    $threw = $false
    try { Get-AreaShare -Weight @{} | Out-Null } catch { $threw = $_.Exception.Message -match 'No samples' }
    Assert-Case 'FALSIFY: a trace without engine samples is refused' $threw

    $spread = Get-Spread -Value @(10.0, 12.0, 11.0)
    Assert-Case 'spread is max-min, mean is the average' ($spread.Mean -eq 11 -and $spread.Spread -eq 2)
    Assert-Case 'one run has no spread' ($null -eq (Get-Spread -Value @(5.0)).Spread)

    $table = @(Format-AreaTable -BeforeShare @($share, $share) -AfterShare $share)
    Assert-Case 'table: header, rule, 8 areas and the sort row' ($table.Count -eq 11 -and $table[0] -match 'Before spread' -and $table[4] -match '^\| Evaluation \| 70\.0% \| 70\.0% \| 0\.0 \| 0\.0 \|$') "got '$($table[4])'"

    $html = "<ol><li><a href='#TblMI'>Functions by Multi-Inclusive Hits with Callers and Callees</a></li></ol>" +
        "<table><tbody><tr><td>StratChessEvolved.exe!std::_Sort_unchecked<x></td><td>999</td></tr></tbody></table>" +
        "<h2>Functions by Multi-Inclusive Hits with Callers and Callees</h2></a><table><tbody>" +
        "<tr><td><a href='#a'>StratChessEvolved.exe</a>!std::_Sort_unchecked<std::pair<int,int> *></td><td>300</td><td>1%</td></tr><tr>" +
        "<tr class='fu'><td>&nbsp;&lt;-- <a>StratChessEvolved.exe</a>!<a>AIPerplex::pvs</a></td><td>200</td><td>1%</td></tr><tr>" +
        "<tr class='fu'><td>&nbsp;&lt;-- <a>StratChessEvolved.exe</a>!<a>AIPerplex::quiescence</a></td><td>100</td><td>1%</td></tr><tr>" +
        "<tr class='fu'><td>&nbsp;&lt;-- <a>StratChessEvolved.exe</a>!<a>std::_Sort_unchecked<std::pair<int,int> *></a></td><td>50</td><td>1%</td></tr><tr>" +
        "<tr class='fi'><td>&nbsp;***itself***</td><td>300</td><td>1%</td></tr><tr>" +
        "<tr class='fd'><td>&nbsp;--&gt; <a>StratChessEvolved.exe</a>!<a>std::_Med3</a></td><td>5</td><td>1%</td></tr><tr>" +
        "<tr><td><a>StratChessEvolved.exe</a>!<a>Board::DoMove</a></td><td>900</td><td>1%</td></tr><tr>" +
        "<tr class='fu'><td>&nbsp;&lt;-- <a>StratChessEvolved.exe</a>!<a>AIPerplex::pvs</a></td><td>900</td><td>1%</td></tr>" +
        "</tbody></table>"
    $callerWeight = ConvertFrom-XperfButterfly -Html $html -Pattern '_Sort_unchecked'
    Assert-Case 'butterfly: callers of the matched symbol only, not the TOC, not recursion' (
        $callerWeight.Count -eq 2 -and $callerWeight['StratChessEvolved.exe!AIPerplex::pvs'] -eq 200 -and
        $callerWeight['StratChessEvolved.exe!AIPerplex::quiescence'] -eq 100) "got $(($callerWeight.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ', ')"

    $perf = @(
        '# Samples: 1K of event ''cycles:u'''
        '# Overhead       Samples  Symbol'
        '# ........  ............  ......'
        '#'
        '    60.00%           600  [.] AIPerplex::pvs(ThreadData&, TranspositionTable&, int, int)'
        '    25.00%            30  [.] TranspositionTable::probe(unsigned long, int) const'
        '    10.00%  [.] void std::__introsort_loop<std::pair<int, int>*, long>(std::pair<int, int>*, std::pair<int, int>*, long)'
        '     5.00%            50  [.] 0x00007f3a12345678'
        ''
        '# (Tip: ...)'
    )
    $perfWeight = ConvertFrom-PerfReport -Line $perf
    # FALSIFY: samples disagree with overhead on the TT row; the weight must be the overhead.
    Assert-Case 'perf: symbol rows only, with or without -n, weighted by overhead' ($perfWeight.Count -eq 4 -and $perfWeight['TranspositionTable::probe(unsigned long, int) const'] -eq 25) "got $($perfWeight.Count) symbols"
    $perfShare = Get-AreaShare -Weight $perfWeight
    Assert-Case 'perf shares: search 60, TT 25, ordering and sort 10, unresolved Other 5' (
        [math]::Abs($perfShare.Area['Search bodies'] - 60) -lt 1e-9 -and [math]::Abs($perfShare.Area['TT probe/store'] - 25) -lt 1e-9 -and
        [math]::Abs($perfShare.Area['Move ordering'] - 10) -lt 1e-9 -and [math]::Abs($perfShare.Sort - 10) -lt 1e-9 -and
        [math]::Abs($perfShare.Area['Other'] - 5) -lt 1e-9) `
        "got $($perfShare.Area['Search bodies'])/$($perfShare.Area['TT probe/store'])/$($perfShare.Area['Move ordering'])/$($perfShare.Sort)"
    Assert-Case 'FALSIFY: the WSL build script carries no CR' (-not $WslBuildScript.Contains("`r"))

    $now = [datetime]'2026-10-06 10:00:20'
    $bannerCases = @(
        @{ Name = 'quiet ends mid-minute, rounded up'; Quiet = $true;  Seconds = 45;    Rough = $false; Expect = '[10:00:20] QUIET NEEDED: collect, ~45 s, until ~10:02' }
        @{ Name = 'quiet ends on the minute';          Quiet = $true;  Seconds = 40;    Rough = $true;  Expect = '[10:00:20] QUIET NEEDED: collect, ~40 s (rough), until ~10:01' }
        @{ Name = 'free phase in minutes';             Quiet = $false; Seconds = 241;   Rough = $false; Expect = '[10:00:20] Machine free: collect, ~5 min' }
        @{ Name = 'no estimate';                       Quiet = $true;  Seconds = $null; Rough = $false; Expect = '[10:00:20] QUIET NEEDED: collect, ~?' }
    )
    foreach ($case in $bannerCases) {
        $got = Format-PhaseBanner -Name 'collect' -Quiet $case.Quiet -Seconds $case.Seconds -Rough $case.Rough -Now $now
        Assert-Case "banner: $($case.Name)" ($got -eq $case.Expect) "got '$got'"
    }
    $plan = @(Format-PhasePlan -Phase @(
            @{ Name = 'build'; Quiet = $false; Seconds = 200; Rough = $true }
            @{ Name = 'collect'; Quiet = $true; Seconds = 60; Rough = $false }
        ))
    Assert-Case 'plan: one row per phase plus the total' ($plan.Count -eq 3 -and $plan[0] -match '^  free   build\s+~4 min \(rough\)$' -and $plan[2] -eq '  total ~5 min, of which quiet ~60 s') "got '$($plan -join ' / ')'"

    if ($failures -gt 0) { Write-Host "$failures self-test case(s) failed." -ForegroundColor Red; exit 1 }
    Write-Host 'All self-test cases passed.' -ForegroundColor Green
    exit 0
}

# ---------------------------------------------------------------------------
# Profiling run
# ---------------------------------------------------------------------------

function Import-VsDevEnvironment {
    <# vcvars64 into this process, so cmake, ninja and clang-cl resolve; returns the VS root. #>
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw "vswhere.exe not found at $vswhere. Is Visual Studio installed?" }
    $vsRoot = (& $vswhere -latest -property installationPath) | Select-Object -First 1
    if (-not $vsRoot) { throw 'No Visual Studio installation found via vswhere.' }
    $vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path -LiteralPath $vcvars)) { throw "vcvars64.bat not found at $vcvars. Is the C++ workload installed?" }

    cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
    # Some agent shells omit this, vcvars64 does not restore it, and a fresh CMake tree needs it.
    if ([string]::IsNullOrWhiteSpace($env:PROCESSOR_ARCHITECTURE)) {
        if ($env:VSCMD_ARG_TGT_ARCH -ine 'x64') { throw "PROCESSOR_ARCHITECTURE is missing and vcvars64 reported target '$env:VSCMD_ARG_TGT_ARCH'." }
        $env:PROCESSOR_ARCHITECTURE = 'AMD64'
    }
    foreach ($tool in 'cmake', 'ninja', 'clang-cl') {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool not on PATH after vcvars64." }
    }
    return $vsRoot
}

function Get-SharedDepsCache {
    <# FetchContent's cache beside the main checkout, shared with build.ps1. #>
    $commonDir = & git -C $RepoRoot rev-parse --path-format=absolute --git-common-dir 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $commonDir) { return $null }
    $mainCheckout = $commonDir -replace '[\\/]\.git[\\/]?$', ''
    return (Join-Path (Split-Path $mainCheckout -Parent) 'StratChessDeps') -replace '\\', '/'
}

function Resolve-ProfileRef {
    <# A worktree path, built in place, or a commit of this repository, built from a temporary checkout. #>
    param([Parameter(Mandatory)][string]$Ref, [Parameter(Mandatory)][string]$Arm)

    if (Test-Path -LiteralPath $Ref -PathType Container) {
        $root = (Resolve-Path -LiteralPath $Ref).Path
        if (-not (Test-Path -LiteralPath (Join-Path $root 'CMakePresets.json'))) { throw "-$Arm '$Ref' is a directory but not a StratChess checkout." }
        $commit = (& git -C $root rev-parse HEAD)
        if ($LASTEXITCODE -ne 0) { throw "-$Arm '$Ref' is not a git worktree." }
        # Untracked files count: CMake globs sources, so a new .cpp is built.
        $dirty = @(& git -C $root status --porcelain).Count -gt 0
        return [pscustomobject]@{ Arm = $Arm; Ref = $Ref; Tree = $root; Commit = [string]$commit; Dirty = $dirty }
    }
    $commit = (& git -C $RepoRoot rev-parse --verify --quiet "$Ref^{commit}")
    if ($LASTEXITCODE -ne 0 -or -not $commit) { throw "-$Arm '$Ref' is neither a directory nor a commit of $RepoRoot." }
    return [pscustomobject]@{ Arm = $Arm; Ref = $Ref; Tree = $null; Commit = [string]$commit; Dirty = $false }
}

function Build-ProfileVariant {
    <#
        Builds the /Z7 variant and copies its exe and PDB to $ArmDir\bin. A worktree keeps its
        build tree (build\windows-clang-cl-profile) for incremental reuse; a commit's temporary
        checkout and build tree are removed once the binaries are copied.
    #>
    param([Parameter(Mandatory)][object]$Ref, [Parameter(Mandatory)][string]$ArmDir, [AllowNull()][string]$DepsCache)

    New-Item -ItemType Directory -Force -Path $ArmDir | Out-Null
    $checkout = $null
    if ($Ref.Tree) {
        $source = $Ref.Tree
        $buildDir = Join-Path $source 'build\windows-clang-cl-profile'
    } else {
        $checkout = Join-Path $ArmDir 'src'
        & git -C $RepoRoot worktree add --detach --quiet $checkout $Ref.Commit 2>&1 | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "git worktree add failed for $($Ref.Commit)" }
        $source = $checkout
        $buildDir = Join-Path $ArmDir 'build'
    }

    $log = Join-Path $ArmDir 'build.log'
    try {
        # cmake --preset resolves CMakePresets.json against the current directory.
        Push-Location -LiteralPath $source
        try {
            if (-not (Test-Path -LiteralPath (Join-Path $buildDir 'CMakeCache.txt'))) {
                $configure = @('--preset', 'windows-clang-cl', '-B', $buildDir, '--log-level=WARNING',
                    # CMake's default clang-cl flags plus /Z7; the linker then writes a full PDB.
                    '-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /GR /EHsc /Z7',
                    '-DCMAKE_EXE_LINKER_FLAGS_RELEASE=/DEBUG /OPT:REF /OPT:ICF')
                if ($DepsCache) { $configure += "-DFETCHCONTENT_BASE_DIR=$DepsCache" }
                & cmake @configure *>> $log
                if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath $log -Tail 20 | Out-Host; throw "Configure failed; full log: $log" }
            }
            & cmake --build $buildDir --target StratChessEvolved *>> $log
            if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath $log -Tail 20 | Out-Host; throw "Build failed; full log: $log" }
        }
        finally { Pop-Location }

        $flags = Select-String -LiteralPath (Join-Path $buildDir 'CMakeCache.txt') -Pattern '^CMAKE_CXX_FLAGS:\w+=(.*)$'
        if (-not $flags -or $flags.Matches[0].Groups[1].Value -notmatch '/Z7') { throw "$buildDir was not configured with /Z7; delete it and rerun." }

        $bin = Join-Path $ArmDir 'bin'
        New-Item -ItemType Directory -Force -Path $bin | Out-Null
        foreach ($extension in 'exe', 'pdb') {
            $file = Join-Path $buildDir "StratChessEvolved.$extension"
            if (-not (Test-Path -LiteralPath $file)) { throw "The build wrote no $file." }
            Copy-Item -LiteralPath $file -Destination $bin
        }
        return (Join-Path $bin 'StratChessEvolved.exe')
    }
    finally {
        if ($checkout) {
            & git -C $RepoRoot worktree remove --force $checkout 2>&1 | Out-Host
            if (Test-Path -LiteralPath $buildDir) { Remove-Item -LiteralPath $buildDir -Recurse -Force }
        }
    }
}

function Invoke-Wsl {
    <# A command in the profiling distro. --exec passes arguments verbatim, with no shell to mangle them. #>
    param([Parameter(Mandatory)][string[]]$Argument)
    & wsl.exe -d $WslDistro --exec @Argument
}

function ConvertTo-WslPath {
    param([Parameter(Mandatory)][string]$Path)
    $wslPath = Invoke-Wsl -Argument 'wslpath', '-a', $Path
    if ($LASTEXITCODE -ne 0 -or -not $wslPath) { throw "wslpath could not translate $Path." }
    return [string]$wslPath
}

function Export-RefArchive {
    <#
        The ref's source as a tar. A dirty worktree is snapshotted through a temporary index, so
        uncommitted and untracked (not ignored) files are built, as the Windows arm builds them.
    #>
    param([Parameter(Mandatory)][object]$Ref, [Parameter(Mandatory)][string]$Tar)

    $repo = if ($Ref.Tree) { $Ref.Tree } else { $RepoRoot }
    $treeish = $Ref.Commit
    if ($Ref.Dirty) {
        $env:GIT_INDEX_FILE = "$Tar.index"
        try {
            & git -C $repo read-tree HEAD | Out-Host
            & git -C $repo add -A | Out-Host
            $treeish = & git -C $repo write-tree
            if ($LASTEXITCODE -ne 0) { throw "Could not snapshot $repo." }
        }
        finally {
            Remove-Item Env:GIT_INDEX_FILE
            Remove-Item -LiteralPath "$Tar.index" -ErrorAction SilentlyContinue
        }
    }
    & git -C $repo archive --format=tar -o $Tar $treeish | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "git archive failed for $treeish." }
}

function Build-LinuxProfileVariant {
    <# Builds the GCC -g variant in WSL and copies its exe to $ArmDir\bin. #>
    param([Parameter(Mandatory)][object]$Ref, [Parameter(Mandatory)][string]$ArmDir, [Parameter(Mandatory)][string]$WorkName)

    $bin = Join-Path $ArmDir 'bin'
    New-Item -ItemType Directory -Force -Path $bin | Out-Null
    $tar = Join-Path $ArmDir 'source.tar'
    $script = Join-Path $ArmDir 'build.sh'
    $log = Join-Path $ArmDir 'build.log'
    [System.IO.File]::WriteAllText($script, $WslBuildScript)
    Export-RefArchive -Ref $Ref -Tar $tar
    try {
        Invoke-Wsl -Argument 'bash', (ConvertTo-WslPath $script), (ConvertTo-WslPath $tar), (ConvertTo-WslPath $bin), $WorkName *>> $log
        if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath $log -Tail 20 | Out-Host; throw "WSL build failed; full log: $log" }
    }
    finally { Remove-Item -LiteralPath $tar -ErrorAction SilentlyContinue }

    if (-not (Select-String -LiteralPath $log -Pattern '^CMAKE_CXX_FLAGS:\w+=-g$' -Quiet)) { throw "The WSL build was not configured with -g; see $log" }
    $exe = Join-Path $bin 'StratChessEvolved'
    if (-not (Test-Path -LiteralPath $exe)) { throw "The WSL build wrote no $exe." }
    return $exe
}

function Read-EngineLine {
    <# Engine output lines up to and including the first matching $Pattern. #>
    param([Parameter(Mandatory)][System.Diagnostics.Process]$Process, [Parameter(Mandatory)][string]$Pattern, [int]$TimeoutMs = 600000)

    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $lines = [System.Collections.Generic.List[string]]::new()
    while ($true) {
        $remaining = $TimeoutMs - [int]$timer.ElapsedMilliseconds
        $task = $Process.StandardOutput.ReadLineAsync()
        if ($remaining -le 0 -or -not $task.Wait([math]::Max(1, $remaining))) { throw "Engine gave no '$Pattern' within $($TimeoutMs / 1000) s." }
        $line = $task.GetAwaiter().GetResult()
        if ($null -eq $line) { throw "Engine exited before '$Pattern'." }
        $lines.Add($line)
        if ($line -match $Pattern) { return $lines }
    }
}

function Send-EngineCommand {
    param([Parameter(Mandatory)][System.Diagnostics.Process]$Process, [Parameter(Mandatory)][string]$Command)
    $Process.StandardInput.WriteLine($Command)
    $Process.StandardInput.Flush()
}

function Invoke-ProfiledRun {
    <#
        One engine process searching every position while VSDiagnostics samples it, or, on Linux,
        the engine under perf record. Writes $RunDir\session.diagsession or $RunDir\perf.data and
        returns nodes and best move per position.
    #>
    param(
        [Parameter(Mandatory)][string]$Exe,
        [Parameter(Mandatory)][object[]]$PositionList,
        [Parameter(Mandatory)][string]$RunDir,
        [switch]$OnLinux
    )

    New-Item -ItemType Directory -Force -Path $RunDir | Out-Null
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    if ($OnLinux) {
        # perf cannot write its data to /mnt/c ("Bad address"), so it records on ext4 and the
        # file is copied out after the run.
        $wslRun = "$script:WslWork-$(Split-Path $RunDir -Leaf)"
        Invoke-Wsl -Argument 'mkdir', '-p', $wslRun | Out-Host
        # perf records only its child, so the samples are the engine's without a PID filter.
        $psi.FileName = 'wsl.exe'
        foreach ($a in '-d', $WslDistro, '--cd', $wslRun, '--exec', 'perf', 'record', '-F', '1000', '-e', 'cycles:u',
            '-o', 'perf.data', '--', (ConvertTo-WslPath $Exe), 'uci') { $psi.ArgumentList.Add($a) }
    } else {
        $psi.FileName = $Exe
        $psi.Arguments = 'uci'
        $psi.WorkingDirectory = Split-Path $Exe -Parent
    }
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    # A Linux engine reads "uci`r" as an unknown command.
    $proc.StandardInput.NewLine = "`n"
    $stderr = $proc.StandardError.ReadToEndAsync()
    # VSDiagnostics session ids are 0-255; a random one avoids a session left over from a crash.
    $session = Get-Random -Minimum 1 -Maximum 256
    $collecting = $false
    try {
        Send-EngineCommand $proc 'uci'
        $null = Read-EngineLine $proc '^uciok' 30000
        Send-EngineCommand $proc 'setoption name Threads value 1'
        Send-EngineCommand $proc 'isready'
        $null = Read-EngineLine $proc '^readyok' 30000

        if (-not $OnLinux) {
            & $script:VsDiagnostics start $session "/attach:$($proc.Id)" "/loadConfig:$script:CollectorConfig" 2>&1 | Out-Host
            if ($LASTEXITCODE -ne 0) { throw "VSDiagnostics start failed (exit $LASTEXITCODE)." }
            $collecting = $true
            Start-Sleep -Seconds 2
        }

        $results = foreach ($position in $PositionList) {
            Send-EngineCommand $proc 'ucinewgame'
            Send-EngineCommand $proc 'isready'
            $null = Read-EngineLine $proc '^readyok' 30000
            Send-EngineCommand $proc "position fen $($position.Fen)"
            Send-EngineCommand $proc "go depth $Depth"
            $lines = @(Read-EngineLine $proc '^bestmove ')
            $nodes = $null
            foreach ($l in $lines) { if ($l -match ' nodes (\d+)') { $nodes = [int64]$Matches[1] } }
            [pscustomobject]@{ Name = $position.Name; Nodes = $nodes; Best = ($lines[-1] -split '\s+')[1] }
        }

        if (-not $OnLinux) {
            & $script:VsDiagnostics stop $session "/output:$(Join-Path $RunDir 'session.diagsession')" 2>&1 | Out-Host
            $collecting = $false
            if ($LASTEXITCODE -ne 0) { throw "VSDiagnostics stop failed (exit $LASTEXITCODE)." }
        }
        Send-EngineCommand $proc 'quit'
        if (-not $proc.WaitForExit(60000)) { throw 'Engine did not exit after quit.' }
        if ($OnLinux) {
            $stderr.Result | Set-Content -LiteralPath (Join-Path $RunDir 'perf-record.log') -Encoding utf8
            if ($proc.ExitCode -ne 0) { throw "perf record failed (exit $($proc.ExitCode)); see $RunDir\perf-record.log" }
            Invoke-Wsl -Argument 'cp', "$wslRun/perf.data", (ConvertTo-WslPath $RunDir) | Out-Host
            if ($LASTEXITCODE -ne 0) { throw "Could not copy $wslRun/perf.data to $RunDir." }
        }
        return @($results)
    }
    finally {
        if ($OnLinux) { Invoke-Wsl -Argument 'rm', '-rf', $wslRun | Out-Host }
        if ($collecting) { & $script:VsDiagnostics stop $session "/output:$(Join-Path $RunDir 'aborted.diagsession')" 2>&1 | Out-Host }
        if (-not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit() }
        $proc.Dispose()
    }
}

function Set-SymbolPath {
    <# The run's PDB first, then Microsoft's server, which resolves ntdll's SRWLock calls. #>
    param([Parameter(Mandatory)][string]$BinDir)
    $env:_NT_SYMBOL_PATH = "$BinDir;srv*$script:SymbolCache*https://msdl.microsoft.com/download/symbols"
}

function Invoke-RunAnalysis {
    <# Expands the run's session to an .etl and writes xperf's flat profile; returns the area shares. #>
    param([Parameter(Mandatory)][string]$RunDir, [Parameter(Mandatory)][string]$BinDir)

    $diag = Join-Path $RunDir 'session.diagsession'
    if (-not (Get-ChildItem -LiteralPath $RunDir -Recurse -Filter '*.etl')) {
        & $script:VsDiagnostics expandDiagSession $diag *> (Join-Path $RunDir 'expand.log')
        if ($LASTEXITCODE -ne 0) { throw "expandDiagSession failed for $diag; see $RunDir\expand.log" }
    }
    $etl = Get-ChildItem -LiteralPath $RunDir -Recurse -Filter '*.etl' | Sort-Object Length -Descending | Select-Object -First 1
    if (-not $etl) { throw "No .etl in the expanded $diag" }

    Set-SymbolPath -BinDir $BinDir
    $flat = Join-Path $RunDir 'xperf-profile.txt'
    # -o must come before -a: xperf hands every argument after -a to the action.
    & $script:Xperf -i $etl.FullName -symbols -o $flat -a profile -detail *> (Join-Path $RunDir 'xperf.log')
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $flat)) { throw "xperf profile failed; see $RunDir\xperf.log" }
    return [pscustomobject]@{
        Etl    = $etl.FullName
        Weight = ConvertFrom-XperfProfile -Line @(Get-Content -LiteralPath $flat)
    }
}

function Invoke-LinuxRunAnalysis {
    <# Writes perf's flat self-time report for the run; returns the symbol weights. #>
    param([Parameter(Mandatory)][string]$RunDir)

    $data = Join-Path $RunDir 'perf.data'
    if (-not (Test-Path -LiteralPath $data)) { throw "No perf.data in $RunDir" }
    $flat = Join-Path $RunDir 'perf-report.txt'
    $text = @(Invoke-Wsl -Argument 'perf', 'report', '-i', (ConvertTo-WslPath $data), '--stdio', '--no-children', '--sort', 'sym' 2> (Join-Path $RunDir 'perf-report.log'))
    if ($LASTEXITCODE -ne 0) { throw "perf report failed; see $RunDir\perf-report.log" }
    $text | Set-Content -LiteralPath $flat -Encoding utf8
    return [pscustomobject]@{ Etl = $null; Weight = ConvertFrom-PerfReport -Line $text }
}

function Get-CallerSplit {
    <# Writes xperf's butterfly report for the run and returns the callers of symbols matching $Pattern. #>
    param([Parameter(Mandatory)][string]$RunDir, [Parameter(Mandatory)][string]$Etl, [Parameter(Mandatory)][string]$BinDir, [Parameter(Mandatory)][string]$Pattern)

    Set-SymbolPath -BinDir $BinDir
    $report = Join-Path $RunDir 'xperf-stack.html'
    & $script:Xperf -i $Etl -symbols -o $report -a stack -butterfly 50 -process $EngineProcess *> (Join-Path $RunDir 'xperf-stack.log')
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $report)) { throw "xperf stack failed; see $RunDir\xperf-stack.log" }
    return ConvertFrom-XperfButterfly -Html (Get-Content -LiteralPath $report -Raw) -Pattern $Pattern
}

if (-not $Reanalyse -and (-not $Before -or -not $After)) { throw 'Give -Before and -After (a worktree path or a commit each), -Reanalyse, or -SelfTest.' }

$onLinux = [bool]$Linux
if ($Reanalyse) {
    $outPath = (Resolve-Path -LiteralPath $Reanalyse).Path
    $metadata = Get-Content -LiteralPath (Join-Path $outPath 'metadata.json') -Raw | ConvertFrom-Json
    $onLinux = $metadata.PSObject.Properties.Name -contains 'platform' -and $metadata.platform -eq 'linux'
}
if ($Callers -and $onLinux) { throw '-Callers needs xperf''s call stacks; the Linux arm records none.' }

if ($onLinux) {
    $toolCheck = Invoke-Wsl -Argument 'which', 'cmake', 'ninja', 'g++', 'perf', 'tar' 2>&1
    if ($LASTEXITCODE -ne 0) { throw "WSL distro $WslDistro lacks a tool (cmake, ninja, g++, perf, tar) or is not installed: $toolCheck" }
    $wslHome = Invoke-Wsl -Argument 'printenv', 'HOME'
} else {
    $vsRoot = Import-VsDevEnvironment
    $collector = Join-Path $vsRoot 'Team Tools\DiagnosticsHub\Collector'
    $script:VsDiagnostics = Join-Path $collector 'VSDiagnostics.exe'
    $script:CollectorConfig = Join-Path $collector 'AgentConfigs\CpuUsageBase.json'
    foreach ($file in $script:VsDiagnostics, $script:CollectorConfig) {
        if (-not (Test-Path -LiteralPath $file)) { throw "$file not found. Install the Visual Studio profiling tools (Diagnostics Hub)." }
    }
    $xperfCommand = Get-Command xperf -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    $script:Xperf = if ($xperfCommand) { $xperfCommand.Source } else { Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Windows Performance Toolkit\xperf.exe' }
    if (-not (Test-Path -LiteralPath $script:Xperf)) { throw 'xperf.exe not found. Install the Windows Performance Toolkit (Windows SDK).' }
}
$exeName = if ($onLinux) { 'StratChessEvolved' } else { 'StratChessEvolved.exe' }
$analysisName = if ($onLinux) { 'perf report' } else { 'xperf analysis' }

$profileRoot = Join-Path $RepoRoot 'build\cpu-profile'
# Shared by every run, so Microsoft's ntdll symbols download once per machine.
$script:SymbolCache = Join-Path $profileRoot 'symcache'

if ($Reanalyse) {
    $refs = [ordered]@{}
    $bin = @{}
    foreach ($arm in 'before', 'after') {
        $refs[$arm] = [pscustomobject]@{ Ref = $metadata.$arm.ref; Commit = $metadata.$arm.commit; Dirty = $metadata.$arm.dirty }
        $bin[$arm] = Join-Path $outPath $arm 'bin'
    }
    $Depth = $metadata.depth
    $positionCount = $metadata.positions
    $runs = [System.Collections.Generic.List[object]]::new()
    foreach ($dir in Get-ChildItem -LiteralPath (Join-Path $outPath 'runs') -Directory | Sort-Object { [int]($_.Name -split '-')[1] }, { $_.Name -ne 'before-1' }) {
        $arm, $index = $dir.Name -split '-'
        $result = @(Get-Content -LiteralPath (Join-Path $dir.FullName 'result.json') -Raw | ConvertFrom-Json)
        $runs.Add([pscustomobject]@{ Arm = $arm; Index = [int]$index; Dir = $dir.FullName; Result = $result; Analysis = $null })
    }
    Write-Host "Reanalysing: $outPath" -ForegroundColor Cyan
} else {
    if (-not $OutDir) { $OutDir = Join-Path $profileRoot (Get-Date -Format 'yyyyMMdd-HHmmss') }
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    $outPath = (Resolve-Path -LiteralPath $OutDir).Path
    if (@(Get-ChildItem -LiteralPath $outPath -Force).Count -gt 0) { throw "$outPath is not empty; use a fresh -OutDir." }
    # Prefix of the run's ext4 work directories, under build.sh's root, so leftovers sit in one place.
    if ($onLinux) { $script:WslWork = "$wslHome/strat-cpu-profile/$(Split-Path $outPath -Leaf)" }
    $positionList = @(Resolve-Positions -Path $Positions)
    $positionCount = $positionList.Count
    $refs = [ordered]@{ before = (Resolve-ProfileRef -Ref $Before -Arm 'Before'); after = (Resolve-ProfileRef -Ref $After -Arm 'After') }
    @{
        before = @{ ref = $Before; commit = $refs['before'].Commit; dirty = $refs['before'].Dirty }
        after  = @{ ref = $After; commit = $refs['after'].Commit; dirty = $refs['after'].Dirty }
        depth  = $Depth; beforeRuns = $BeforeRuns; positions = $positionCount; platform = $(if ($onLinux) { 'linux' } else { 'windows' })
    } | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $outPath 'metadata.json') -Encoding utf8
    $depsCache = Get-SharedDepsCache

    # Interleaved before/after/before, so drift over the window shows up in the before spread.
    $runPlan = @(@{ Arm = 'before'; Index = 1 }; @{ Arm = 'after'; Index = 1 })
    for ($i = 2; $i -le $BeforeRuns; $i++) { $runPlan += @{ Arm = 'before'; Index = $i } }

    $phases = @(
        @{ Name = 'build before'; Quiet = $false; Seconds = $RoughBuildSeconds; Rough = $true }
        @{ Name = 'build after'; Quiet = $false; Seconds = $RoughBuildSeconds; Rough = $true }
        @{ Name = "profile collection ($($runPlan.Count) runs)"; Quiet = $true; Seconds = $RoughRunSeconds * $runPlan.Count; Rough = $true }
        @{ Name = $analysisName; Quiet = $false; Seconds = $RoughAnalysisSeconds * $runPlan.Count; Rough = $true }
    )
    Write-Host "Output: $outPath" -ForegroundColor Cyan
    Write-PhasePlan -Phase $phases

    $bin = @{}
    $buildSeconds = $null
    foreach ($arm in $refs.Keys) {
        $ref = $refs[$arm]
        $estimate = if ($null -ne $buildSeconds) { $buildSeconds } else { $RoughBuildSeconds }
        Write-PhaseBanner -Name "build $arm ($($ref.Ref) @ $($ref.Commit.Substring(0, 9)))" -Quiet $false -Seconds $estimate -Rough ($null -eq $buildSeconds)
        if ($ref.Dirty) { Write-Host "  WARN  $($ref.Tree) has uncommitted changes; its commit does not describe it" -ForegroundColor Yellow }
        $timer = [System.Diagnostics.Stopwatch]::StartNew()
        $exe = if ($onLinux) {
            Build-LinuxProfileVariant -Ref $ref -ArmDir (Join-Path $outPath $arm) -WorkName "$(Split-Path $outPath -Leaf)-$arm"
        } else {
            Build-ProfileVariant -Ref $ref -ArmDir (Join-Path $outPath $arm) -DepsCache $depsCache
        }
        $bin[$arm] = Split-Path $exe -Parent
        if ($null -eq $buildSeconds) { $buildSeconds = $timer.Elapsed.TotalSeconds }
        Write-Host ("  built in {0:N0} s: {1}" -f $timer.Elapsed.TotalSeconds, $exe) -ForegroundColor DarkGray
    }

    $runs = [System.Collections.Generic.List[object]]::new()
    $runSeconds = $null
    for ($r = 0; $r -lt $runPlan.Count; $r++) {
        $remaining = $runPlan.Count - $r
        if ($r -eq 0) {
            Write-PhaseBanner -Name "profile collection ($remaining runs)" -Quiet $true -Seconds ($RoughRunSeconds * $remaining) -Rough $true
        } elseif ($r -eq 1) {
            Write-PhaseBanner -Name "profile collection ($remaining runs left)" -Quiet $true -Seconds ($runSeconds * $remaining)
        }
        $step = $runPlan[$r]
        $runDir = Join-Path $outPath "runs\$($step.Arm)-$($step.Index)"
        $timer = [System.Diagnostics.Stopwatch]::StartNew()
        $result = @(Invoke-ProfiledRun -Exe (Join-Path $bin[$step.Arm] $exeName) -PositionList $positionList -RunDir $runDir -OnLinux:$onLinux)
        if ($null -eq $runSeconds) { $runSeconds = $timer.Elapsed.TotalSeconds }
        Write-Host ("  {0}-{1}: {2:N0} nodes in {3:N0} s" -f $step.Arm, $step.Index, ($result | Measure-Object Nodes -Sum).Sum, $timer.Elapsed.TotalSeconds) -ForegroundColor DarkGray
        ConvertTo-Json -InputObject @($result) | Set-Content -LiteralPath (Join-Path $runDir 'result.json') -Encoding utf8
        $runs.Add([pscustomobject]@{ Arm = $step.Arm; Index = $step.Index; Dir = $runDir; Result = $result; Analysis = $null })
    }
}

Write-PhaseBanner -Name $analysisName -Quiet $false -Seconds ($RoughAnalysisSeconds * $runs.Count) -Rough $true
for ($r = 0; $r -lt $runs.Count; $r++) {
    $run = $runs[$r]
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $run.Analysis = if ($onLinux) { Invoke-LinuxRunAnalysis -RunDir $run.Dir } else { Invoke-RunAnalysis -RunDir $run.Dir -BinDir $bin[$run.Arm] }
    Write-Host ("  {0}-{1} analysed in {2:N0} s" -f $run.Arm, $run.Index, $timer.Elapsed.TotalSeconds) -ForegroundColor DarkGray
    # The first trace also pays the one-time symbol download, so this estimate errs long.
    if ($r -eq 0 -and $runs.Count -gt 1) {
        Write-PhaseBanner -Name "$analysisName ($($runs.Count - 1) traces left)" -Quiet $false -Seconds ($timer.Elapsed.TotalSeconds * ($runs.Count - 1))
    }
}

# Report
$baselineRunList = @($runs | Where-Object { $_.Arm -eq 'before' })
$afterRun = @($runs | Where-Object { $_.Arm -eq 'after' })[0]
$beforeShare = @($baselineRunList | ForEach-Object { Get-AreaShare -Weight $_.Analysis.Weight })
$afterShare = Get-AreaShare -Weight $afterRun.Analysis.Weight

$label = @{}
foreach ($arm in $refs.Keys) {
    $dirtyNote = if ($refs[$arm].Dirty) { ' + uncommitted changes' } else { '' }
    $label[$arm] = "``$($refs[$arm].Commit.Substring(0, 9))``$dirtyNote ($($refs[$arm].Ref))"
}

$report = [System.Collections.Generic.List[string]]::new()
$report.Add("### CPU profile: $($label['before']) → $($label['after'])")
$report.Add('')
$collection = if ($onLinux) { "perf record -F 1000 -e cycles:u, self cycles from perf report, GCC -g Release builds in WSL $WslDistro" } else { 'VSDiagnostics at 1 kHz, self time from xperf, /Z7 clang-cl Release builds' }
$report.Add("Depth $Depth, Threads=1, $positionCount positions, one process per run; $collection. " +
    "Before is the mean of $($baselineRunList.Count) run(s)" + $(if ($baselineRunList.Count -gt 1) { '; spread is max−min across them, and a Δ inside it is noise.' } else { '.' }))
$report.Add('')
foreach ($row in Format-AreaTable -BeforeShare $beforeShare -AfterShare $afterShare) { $report.Add($row) }

$reference = $baselineRunList[0].Result
$mismatch = foreach ($run in $runs | Select-Object -Skip 1) {
    for ($p = 0; $p -lt $reference.Count; $p++) {
        $a = $reference[$p]; $b = $run.Result[$p]
        if ($a.Nodes -ne $b.Nodes -or $a.Best -ne $b.Best) {
            "| $($a.Name) | $($run.Arm)-$($run.Index) | $($a.Nodes) $($a.Best) | $($b.Nodes) $($b.Best) |"
        }
    }
}
$report.Add('')
if ($mismatch) {
    $report.Add('**Search differs from before-1** — the arms did different work, so shares compare behaviour as well as cost:')
    $report.Add('')
    $report.Add('| Position | Run | before-1 nodes, best | this run |')
    $report.Add('|---|---|---|---|')
    foreach ($m in $mismatch) { $report.Add($m) }
} else {
    $report.Add("Node counts and best moves identical across all $($runs.Count) runs ($(($reference | Measure-Object Nodes -Sum).Sum) nodes).")
}

$beforeWeight = $baselineRunList[0].Analysis.Weight
$beforeTotal = ($beforeWeight.Values | Measure-Object -Sum).Sum
$report.Add('')
$report.Add('<details><summary>Top 25 symbols, after (before-1 share in brackets)</summary>')
$report.Add('')
$report.Add('```')
foreach ($entry in $afterRun.Analysis.Weight.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 25) {
    $was = if ($beforeWeight.ContainsKey($entry.Key)) { $beforeWeight[$entry.Key] / $beforeTotal * 100 } else { 0 }
    $name = Get-SymbolName -Symbol $entry.Key
    $report.Add(('{0,5:N1}%  ({1,4:N1}%)  {2,-15}  {3}' -f ($entry.Value / $afterShare.Total * 100), $was, (Get-SymbolArea -Symbol $entry.Key), $name.Substring(0, [math]::Min(100, $name.Length))))
}
$report.Add('```')
$report.Add('</details>')

if ($Callers) {
    Write-Host "  splitting '$Callers' by caller" -ForegroundColor DarkGray
    $split = @{}
    foreach ($run in $baselineRunList[0], $afterRun) {
        $split[$run.Arm] = Get-CallerSplit -RunDir $run.Dir -Etl $run.Analysis.Etl -BinDir $bin[$run.Arm] -Pattern $Callers
    }
    $report.Add('')
    $report.Add("Callers of ``$Callers``: each caller's share of the samples that reach the symbol through a caller, from xperf's butterfly view (before-1, after; hits in brackets):")
    $report.Add('')
    $report.Add('| Caller | Before | After |')
    $report.Add('|---|---:|---:|')
    $names = @(@($split['before'].Keys) + @($split['after'].Keys) | Sort-Object -Unique)
    if ($names.Count -eq 0) { $report.Add('| (no symbol matched, or the trace has no stacks for it) | | |') }
    $hitTotal = @{}
    foreach ($arm in 'before', 'after') { $hitTotal[$arm] = [math]::Max(1, ($split[$arm].Values | Measure-Object -Sum).Sum) }
    $rows = foreach ($n in $names) {
        $b = if ($split['before'].ContainsKey($n)) { $split['before'][$n] } else { 0 }
        $a = if ($split['after'].ContainsKey($n)) { $split['after'][$n] } else { 0 }
        [pscustomobject]@{ Name = (Get-SymbolName -Symbol $n); Before = $b; After = $a }
    }
    foreach ($row in $rows | Sort-Object After, Before -Descending) {
        $report.Add(('| {0} | {1:N1}% ({2:N0}) | {3:N1}% ({4:N0}) |' -f $row.Name, ($row.Before / $hitTotal['before'] * 100), $row.Before, ($row.After / $hitTotal['after'] * 100), $row.After))
    }
}

$reportPath = Join-Path $outPath 'report.md'
$report | Set-Content -LiteralPath $reportPath -Encoding utf8
Write-Host ''
$report | ForEach-Object { Write-Host $_ }
Write-Host ''
Write-Host "Done; machine free. Report and traces: $outPath" -ForegroundColor Green
if (-not $onLinux) { Write-Host 'Keep the directory while the traces may be reopened: each .etl resolves symbols only from its bin\ PDB.' -ForegroundColor DarkGray }
