<#
.SYNOPSIS
    Self-test for UciDriver.ps1, driving it against FakeUciEngine.ps1 instead of a real
    engine so its failure paths are reachable without a build.

.DESCRIPTION
    UciDriver.ps1 is dot-sourced by Compare-SearchEquivalence.ps1 and Run-Bench.ps1 and
    has no param() block to hang a -SelfTest switch on, so its cases live here.
    Validate-PrePR.ps1 runs this script when either file changes, via its
    $SelfTestCoverers map.

    Two halves. The first drives the real driver against each misbehaviour and asserts
    the diagnostic it produces. The second mutates a copy of the driver -- deleting the
    stderr drain, deleting the end-of-output break, batching `quit` behind `go` -- and
    asserts each case then FAILS, because a test that only ever passes proves nothing
    about what it is watching for. Every mutation first asserts that the text it means
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
        [Parameter(Mandatory)][int]$TimeoutMs
    )

    . $Driver   # defines Invoke-UciSearchToBestMove in this function's scope

    $env:STRAT_FAKE_UCI_MODE = $Mode
    # The fixture outlives the kill (the driver kills cmd.exe, not the pwsh under it)
    # and holds the engine's stderr open, which the failure path waits on. Just past
    # the driver's own ceiling, so a timeout case costs its timeout and not much more.
    $env:STRAT_FAKE_UCI_LIFETIME_MS = $TimeoutMs + 2000

    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $out = Invoke-UciSearchToBestMove -ExePath $FakeEngine -WorkDir $PSScriptRoot `
            -Commands @('uci', 'position startpos', 'go depth 2') -SearchDepth 2 `
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
)

foreach ($case in $cases) {
    $r = Invoke-DriverCase -Driver $DriverPath -Mode $case.Mode -TimeoutMs $case.TimeoutMs
    Assert-Case $case.Name `
        (($r.Threw -eq $case.Throws) -and ($r.Message -match $case.Expect)) `
        "threw=$($r.Threw) message=$($r.Message -replace '\s+', ' ')"

    if ($case.Throws) {
        Assert-Case "$($case.Mode): the failure carries the engine transcript" `
            (($r.Message -match 'Engine stderr:') -and ($r.Message -match 'Engine output:'))
        Assert-Case "$($case.Mode): -TimeoutMs is honoured, not the 600s default" `
            ($r.ElapsedMs -lt ($case.TimeoutMs + 15000)) "took $($r.ElapsedMs) ms"
    }
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
       Find = 'foreach ($command in $Commands) {'
       Repl = 'foreach ($command in (@($Commands) + @(''quit''))) {'
       Mode = 'ok'; TimeoutMs = 20000; Expect = 'exited before bestmove' }
)

foreach ($m in $mutations) {
    $mutant = New-MutatedDriver -Find $m.Find -Replace $m.Repl -Tag $m.Tag
    if ($null -eq $mutant) {
        Assert-Case $m.Name $false "UciDriver.ps1 no longer contains '$($m.Find)' — the mutation is vacuous"
        continue
    }
    try {
        $r = Invoke-DriverCase -Driver $mutant -Mode $m.Mode -TimeoutMs $m.TimeoutMs
        Assert-Case $m.Name ($r.Threw -and ($r.Message -match $m.Expect)) `
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
