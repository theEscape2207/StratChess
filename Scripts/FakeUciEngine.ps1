<#
.SYNOPSIS
    A UCI engine that speaks just enough protocol to be driven by UciDriver.ps1, and
    misbehaves on demand so the driver's failure paths can be tested without a build.

.DESCRIPTION
    Launched through FakeUciEngine.cmd, never directly: Invoke-UciSearchToBestMove sets
    ProcessStartInfo.FileName to the path it is given and the arguments to a fixed 'uci',
    and .NET refuses a .ps1 as an executable ("not a valid application for this OS
    platform"). A .cmd shim is accepted, ignores the inherited 'uci' argument, and costs
    two lines.

    The misbehaviour is chosen by the STRAT_FAKE_UCI_MODE environment variable rather
    than a parameter, for the same reason: the shim cannot forward one, because the
    driver overwrites the argument string. ProcessStartInfo inherits the parent
    environment, so the caller just sets the variable before invoking the driver.

    Modes, each named after the driver path it is there to reach:

      ok                     Answer normally. Aborts without a bestmove if anything
                             arrives on stdin during the search, which is how a real
                             engine treats a stop -- and what makes the queued-quit
                             race observable.
      no-bestmove            Search forever, never answer.
      exit-before-bestmove   Exit 0 mid-search, closing stdout.
      nonzero-exit           Answer normally, then exit 3 on quit.
      stderr-flood           Fill the stderr pipe before answering, so a driver that
                             does not drain it deadlocks.
      ignore-quit            Answer normally, then never exit.

.NOTES
    Every wait is bounded by a self-imposed lifetime, STRAT_FAKE_UCI_LIFETIME_MS,
    defaulting to 30 s. See the comment on $deadline for why it exists and why the
    caller should keep it short.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Mode = if ($env:STRAT_FAKE_UCI_MODE) { $env:STRAT_FAKE_UCI_MODE } else { 'ok' }

# How long a mode that "waits forever" actually waits. It has to end on its own: the
# driver kills the process it started, which is cmd.exe, not this pwsh underneath it.
# It also has to end SOON, because the driver's failure path reads the engine's stderr
# to end-of-stream, and this process holds that handle open for as long as it lives --
# so an over-generous lifetime here is added to every timeout case's wall clock.
$lifetimeMs = if ($env:STRAT_FAKE_UCI_LIFETIME_MS) { [int]$env:STRAT_FAKE_UCI_LIFETIME_MS } else { 30000 }
$deadline = (Get-Date).AddMilliseconds($lifetimeMs)

function Write-EngineLine {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Line)
    [Console]::Out.WriteLine($Line)
    [Console]::Out.Flush()
}

# NOT [Console]::In. That is a synchronized wrapper whose ReadLineAsync runs the read
# synchronously and hands back an already-completed task, so Wait(300) blocks until a
# line arrives -- the timed poll below silently becomes an untimed one. A StreamReader
# over the raw handle is genuinely asynchronous.
$script:Stdin = [System.IO.StreamReader]::new([Console]::OpenStandardInput())

# One outstanding ReadLineAsync, kept across calls: a poll that finds nothing must not
# discard the read it started, or the line it was waiting for is lost to the next call.
$script:PendingRead = $null

function Read-EngineCommand {
    <#
        Returns HasLine/Line. HasLine is false both for "nothing yet" (WaitMs elapsed)
        and for end of input, which Line distinguishes: $null means stdin closed.
    #>
    param([int]$WaitMs = -1)

    if ($null -eq $script:PendingRead) { $script:PendingRead = $script:Stdin.ReadLineAsync() }

    $budget = [int][math]::Floor([math]::Max(0, ($deadline - (Get-Date)).TotalMilliseconds))
    $wait = if ($WaitMs -lt 0) { $budget } else { [math]::Min($WaitMs, $budget) }

    if (-not $script:PendingRead.Wait($wait)) {
        return [pscustomobject]@{ HasLine = $false; Line = '' }
    }
    $line = $script:PendingRead.GetAwaiter().GetResult()
    $script:PendingRead = $null
    if ($null -eq $line) { return [pscustomobject]@{ HasLine = $false; Line = $null } }
    return [pscustomobject]@{ HasLine = $true; Line = $line }
}

function Wait-Forever {
    # Bounded by $deadline, whatever the caller intends by "forever".
    while ((Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 50 }
    exit 0
}

function Invoke-Go {
    Write-EngineLine 'info depth 1 score cp 24 nodes 21 time 3 pv e2e4'

    switch ($Mode) {
        'no-bestmove' { Wait-Forever }

        'exit-before-bestmove' { exit 0 }

        'stderr-flood' {
            # Comfortably past the pipe buffer, so an undrained stderr blocks here and
            # the bestmove below is never written.
            [Console]::Error.Write(('x' * 4096 + "`n") * 256)
            [Console]::Error.Flush()
        }

        default {
            # The "search". Anything arriving on stdin during it is a stop, and a real
            # engine that has been stopped answers the stop, not the go.
            $stop = Read-EngineCommand -WaitMs 300
            if ($stop.HasLine -or $null -eq $stop.Line) { exit 0 }
        }
    }

    Write-EngineLine 'info depth 2 score cp 12 nodes 97 time 5 pv e2e4 e7e5'
    Write-EngineLine 'bestmove e2e4'
}

while ((Get-Date) -lt $deadline) {
    $next = Read-EngineCommand
    if (-not $next.HasLine) {
        if ($null -eq $next.Line) { break }   # stdin closed
        continue
    }

    switch -Regex ($next.Line) {
        '^uci$'      { Write-EngineLine 'id name FakeUciEngine'; Write-EngineLine 'uciok' }
        '^isready$'  { Write-EngineLine 'readyok' }
        '^go\b'      { Invoke-Go }
        '^quit$'     {
            if ($Mode -eq 'ignore-quit') { Wait-Forever }
            exit $(if ($Mode -eq 'nonzero-exit') { 3 } else { 0 })
        }
    }
}

if ($Mode -eq 'ignore-quit') { Wait-Forever }
exit 0
