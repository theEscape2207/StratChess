<#
.SYNOPSIS
    Self-test for UciDriver.ps1, driving it against FakeUciEngine.ps1 instead of a real
    engine so its failure paths are reachable without a build.

.DESCRIPTION
    UciDriver.ps1 is dot-sourced by Run-Bench.ps1 and both Compare-Search*.ps1 scripts, and
    has no param() block to hang a -SelfTest switch on, so its cases live here.
    Validate-PrePR.ps1 runs this script when the driver or the fake engine changes, via
    its $SelfTestCoverers map.

    Two halves. The first drives the real driver against each misbehaviour and asserts
    the diagnostic it produces. The second mutates a copy of the driver -- deleting the
    stderr drain, the end-of-output break or the completion check, batching `quit`
    behind `go` -- and asserts the matching case then goes wrong, because a test that
    only ever passes proves nothing about what it is watching for. Every mutation first asserts that the text it means
    to replace is still present, so a later refactor breaks the test loudly instead of
    quietly making it vacuous.

.PARAMETER SelfTest
    Run the cases and exit. The script has no other mode.

.OUTPUTS
    Exit code 0 when every case passes, 1 otherwise.

.HOW TO INVOKE (from bash, cmd, or PowerShell)
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\Test-UciDriver.ps1 -SelfTest
#>
[CmdletBinding()]
param(
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$DriverPath = Join-Path $PSScriptRoot 'UciDriver.ps1'
$FakeEngine = Join-Path $PSScriptRoot 'FakeUciEngine.cmd'

if (-not $SelfTest) {
    Write-Host 'Test-UciDriver.ps1 has only one mode. Re-run it with -SelfTest.' -ForegroundColor Yellow
    exit 1
}

# ---------------------------------------------------------------------------
# Running one exchange
# ---------------------------------------------------------------------------

function Invoke-DriverCase {
    <#
        One driver call against the fake engine in the given mode. Never throws:
        the thrown message IS the result for most cases, so it is returned rather
        than raised. $Driver is a parameter so a mutated copy can be run the same way.
    #>
    param(
        [Parameter(Mandatory)][string]$Driver,
        [Parameter(Mandatory)][string]$Mode,
        [Parameter(Mandatory)][int]$TimeoutMs,
        # $null for the depth the fake engine completes.
        [AllowNull()][object]$Depth
    )

    . $Driver   # defines the driver's functions in this function's scope

    $env:STRAT_FAKE_UCI_MODE = $Mode
    # The fixture outlives the kill (the driver kills cmd.exe, not the pwsh under it)
    # and holds the engine's stderr open, which the failure path waits on. Just past
    # the driver's own ceiling, so a timeout case costs its timeout and not much more.
    $env:STRAT_FAKE_UCI_LIFETIME_MS = $TimeoutMs + 2000

    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $out = Invoke-UciFixedDepthSearch -ExePath $FakeEngine -WorkDir $PSScriptRoot `
            -Position 'startpos moves e2e4' -SearchDepth ($Depth ?? 2) -Threads 3 `
            -Description "fake engine, mode $Mode" -TimeoutMs $TimeoutMs
        return [pscustomobject]@{ Threw = $false; Message = [string]$out; ElapsedMs = $timer.ElapsedMilliseconds }
    } catch {
        return [pscustomobject]@{ Threw = $true; Message = $_.Exception.Message; ElapsedMs = $timer.ElapsedMilliseconds }
    } finally {
        $env:STRAT_FAKE_UCI_MODE = $null
        $env:STRAT_FAKE_UCI_LIFETIME_MS = $null
    }
}

function New-MutatedDriver {
    <#
        A copy of the driver with one exact substring replaced. Returns the path, or
        $null when the substring is absent -- which the caller must treat as a failure,
        not as a skip.
    #>
    param(
        [Parameter(Mandatory)][string]$Find,
        [Parameter(Mandatory)][AllowEmptyString()][string]$Replace,
        [Parameter(Mandatory)][string]$Tag
    )

    $source = Get-Content -LiteralPath $DriverPath -Raw
    if (-not $source.Contains($Find)) { return $null }

    $path = Join-Path ([System.IO.Path]::GetTempPath()) "UciDriver.mutant.$Tag.ps1"
    Set-Content -LiteralPath $path -Value $source.Replace($Find, $Replace) -Encoding utf8
    return $path
}

# ---------------------------------------------------------------------------
# Cases
# ---------------------------------------------------------------------------

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

# Every timeout case asserts its wall clock too. Without that, a -TimeoutMs that was
# ignored would still throw the right message -- ten minutes later.
$cases = @(
    @{ Name = 'a well-behaved engine is driven to bestmove'
       Mode = 'ok';                   TimeoutMs = 20000; Throws = $false; Expect = 'bestmove e2e4' }
    @{ Name = 'an engine that never answers is timed out'
       Mode = 'no-bestmove';          TimeoutMs = 2500;  Throws = $true;  Expect = 'did not finish within 2\.5s' }
    @{ Name = 'an engine that exits mid-search is named, not timed out'
       Mode = 'exit-before-bestmove'; TimeoutMs = 20000; Throws = $true;  Expect = 'exited before bestmove' }
    @{ Name = 'a non-zero exit after bestmove is a failure'
       Mode = 'nonzero-exit';         TimeoutMs = 20000; Throws = $true;  Expect = 'exited with code 3' }
    @{ Name = 'a stderr flood does not deadlock the exchange'
       Mode = 'stderr-flood';         TimeoutMs = 20000; Throws = $false; Expect = 'bestmove e2e4' }
    @{ Name = 'an engine that ignores quit is timed out after bestmove'
       Mode = 'ignore-quit';          TimeoutMs = 3000;  Throws = $true;  Expect = 'did not exit within 3s after bestmove' }
    @{ Name = 'the request is uci, isready, threads, position, depth, in order'
       Mode = 'ok';                   TimeoutMs = 20000; Throws = $false
       Expect = '(?s)got uci\r?\n.*got isready\r?\n.*got setoption name Threads value 3\r?\n.*got position startpos moves e2e4\r?\n.*got go depth 2\r?\n' }
    @{ Name = 'bestmove before the requested depth is refused'
       Mode = 'ok'; Depth = 3;        TimeoutMs = 20000; Throws = $true;  Expect = 'did not complete depth 3' }
)

foreach ($case in $cases) {
    $r = Invoke-DriverCase -Driver $DriverPath -Mode $case.Mode -TimeoutMs $case.TimeoutMs -Depth $case['Depth']
    Assert-Case $case.Name `
        (($r.Threw -eq $case.Throws) -and ($r.Message -match $case.Expect)) `
        "threw=$($r.Threw) message=$($r.Message -replace '\s+', ' ')"

    if ($case.Throws) {
        Assert-Case "$($case.Name): the failure carries the engine transcript" `
            (($r.Message -match 'Engine stderr:') -and ($r.Message -match 'Engine output:'))
        Assert-Case "$($case.Name): -TimeoutMs is honoured, not the 600s default" `
            ($r.ElapsedMs -lt ($case.TimeoutMs + 15000)) "took $($r.ElapsedMs) ms"
    }
}

# The completion rule on its own, for transcripts the fake engine never produces.
. $DriverPath
$completion = @(
    @{ Name = 'CRLF transcript with the depth and a bestmove is complete'
       Output = "info depth 1 pv a2a3`r`ninfo depth 2 pv a2a3`r`nbestmove a2a3`r`n"; Complete = $true }
    @{ Name = 'a deeper-only depth line does not satisfy the requested depth'
       Output = "info depth 20 pv a2a3`nbestmove a2a3"; Complete = $false }
    @{ Name = 'the requested depth without a bestmove is incomplete'
       Output = 'info depth 2 pv a2a3'; Complete = $false }
    @{ Name = 'an aborted depth-0 search is incomplete'
       Output = "info depth 0 score cp 0 nodes 0 time 0 pv a2a4`nbestmove a2a4"; Complete = $false }
)
foreach ($c in $completion) {
    Assert-Case $c.Name ((Test-UciFixedDepthComplete -Output $c.Output -SearchDepth 2) -eq $c.Complete)
}

# ---------------------------------------------------------------------------
# Falsification: each case above must fail against a driver broken in the one
# way that case exists to watch for.
# ---------------------------------------------------------------------------

$mutations = @(
    @{ Name = 'FALSIFY: without the async stderr drain, the flood deadlocks'
       Tag  = 'nodrain'
       Find = '$stderrTask = $proc.StandardError.ReadToEndAsync()'
       Repl = '$stderrTask = [System.Threading.Tasks.Task]::FromResult([string]'''')'
       Mode = 'stderr-flood'; TimeoutMs = 3000; Expect = 'did not finish within' }

    @{ Name = 'FALSIFY: without the end-of-output break, an early exit times out unnamed'
       Tag  = 'nobreak'
       Find = 'if ($null -eq $line) { break }'
       Repl = 'if ($null -eq $line) { $line = '''' }'
       Mode = 'exit-before-bestmove'; TimeoutMs = 3000; Expect = 'did not finish within' }

    @{ Name = 'FALSIFY: quit batched behind go loses the pending-stop race'
       Tag  = 'queuedquit'
       Find = 'foreach ($command in $commands) {'
       Repl = 'foreach ($command in (@($commands) + @(''quit''))) {'
       Mode = 'ok'; TimeoutMs = 20000; Expect = 'exited before bestmove' }

    @{ Name = 'FALSIFY: without the completion check, a short search is returned as a result'
       Tag  = 'nocompletion'
       Find = 'if (-not (Test-UciFixedDepthComplete -Output $transcript -SearchDepth $SearchDepth)) {'
       Repl = 'if ($false) {'
       Mode = 'ok'; Depth = 3; TimeoutMs = 20000; Expect = 'bestmove e2e4'; Returns = $true }
)

foreach ($m in $mutations) {
    $mutant = New-MutatedDriver -Find $m.Find -Replace $m.Repl -Tag $m.Tag
    if ($null -eq $mutant) {
        Assert-Case $m.Name $false "UciDriver.ps1 no longer contains '$($m.Find)' — the mutation is vacuous"
        continue
    }
    try {
        $r = Invoke-DriverCase -Driver $mutant -Mode $m.Mode -TimeoutMs $m.TimeoutMs -Depth $m['Depth']
        # Most mutants break loudly; a missing check instead lets a bad result through.
        $broke = if ($m.ContainsKey('Returns')) { -not $r.Threw } else { $r.Threw }
        Assert-Case $m.Name ($broke -and ($r.Message -match $m.Expect)) `
            "threw=$($r.Threw) message=$($r.Message -replace '\s+', ' ')"
    } finally {
        Remove-Item -LiteralPath $mutant -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ''
if ($failures -gt 0) {
    Write-Host "$failures self-test case(s) FAILED." -ForegroundColor Red
    exit 1
}
Write-Host 'All self-test cases passed.' -ForegroundColor Green
exit 0
