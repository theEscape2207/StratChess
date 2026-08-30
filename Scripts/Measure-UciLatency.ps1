<#
.SYNOPSIS
    Measure the round-trip latency of one UCI command, against a control case.

.DESCRIPTION
    Answers "how long does the engine take to handle command X", which is a different question from
    Run-Bench.ps1's "how many nodes per second does the search manage". Use this for protocol-level
    costs — 'ucinewgame', 'setoption', a 'position' with a long move list — and Run-Bench.ps1 for
    search speed.

    ## Method

    Each repetition has two phases:

      1. Reset + setup: 'ucinewgame', then each -Setup line, then 'isready' read to 'readyok'.
      2. Timed:         the command under test, then read to the completion marker.

    The fence in phase 1 is not decoration. A successful 'position' produces NO response, so the
    only way to know it landed — and the only way to know no unread output is pending when the
    stopwatch starts — is to ask a question the engine must answer.

    ## What the control case is for

    The control runs the identical reset phase, then times an 'isready'/'readyok' round trip with
    the command under test omitted. Harness and IPC overhead appear in both cases, so the
    DIFFERENCE is the command's own cost. A raw figure from the measured case alone includes
    whatever this script and the pipe cost, which on a sub-millisecond command is most of it.

    When -CompletionMarker is not 'readyok' the control is still the 'isready' round trip, so it
    reports harness overhead rather than a baseline that can be subtracted; the report says so and
    prints no delta.

    ## Two traps this script is built to avoid

    DRIVE PAST MOVE 1. The default -Setup plays two moves, so every timed repetition runs from move
    2. GetMove() used to clear the transposition table at fullMoveCount == 1, which is what made
    issue #213's per-move timing wrong — it measured a TT clear and reported ~23 ms of per-move
    overhead that did not exist. Issue #259 removed that particular clear, but "the first move is
    not a representative code path" is the general hazard, and a probe that silently starts from
    the initial position will keep re-finding it.

    'go' IS ASYNCHRONOUS. The command loop is single-threaded and handles commands in order, which
    is what makes 'readyok' a true completion signal for 'ucinewgame', 'position' and 'setoption'.
    But cmd_go() starts the search on its own thread and returns immediately, and 'isready' is not
    refused during a search — so a 'readyok' after 'go' comes back mid-search and times nothing.
    Measure a search with -CompletionMarker bestmove.

    ## Repetition independence

    Repetitions share one engine process, so without the per-repetition reset the transposition
    table carries over and repetition N is faster than N-1 — the median then describes the harness,
    not the engine. Two consequences worth stating: measuring 'ucinewgame' this way measures a
    'ucinewgame' that follows another one, which is the steady state a GUI produces anyway; and a
    'go' measured this way is deliberately a cold-table search. -NoReset opts out for anyone
    measuring warm-state behaviour on purpose.

    Each case runs in a FRESH ENGINE PROCESS, as Run-Bench.ps1 does, so process startup stays
    outside the measured window and no state crosses between cases.

.PARAMETER Exe
    Path to StratChessEvolved.exe. Defaults to the shipping build via Get-BuildArtifact.ps1.

.PARAMETER Command
    The UCI command under test, e.g. 'ucinewgame' or 'go movetime 200'. Required.

.PARAMETER Setup
    Commands establishing the position, sent once per repetition before the timed window.
    Defaults to a position two moves in — see the drive-past-move-1 trap above.

.PARAMETER Repetitions
    Timed repetitions per case, after the warm-up. Default 15.

.PARAMETER WarmUp
    Leading repetitions to discard. Default 1.

.PARAMETER CompletionMarker
    Output line prefix that ends the timed window. Default 'readyok', in which case 'isready' is
    sent after the command under test. Use 'bestmove' for a 'go'; nothing is appended then.

.PARAMETER NoReset
    Send setup once after the handshake instead of resetting before every repetition.

.PARAMETER Threads
    UCI Threads option. Default 1.

.PARAMETER TimeoutMs
    Per-read timeout. Default 30000. Exceeding it aborts naming the phase and the last line read,
    rather than hanging.

.PARAMETER Csv
    Optional path for the per-repetition samples, for plotting or a distribution check.

.EXAMPLE
    # Reproduces the measurement in issue #266.
    .\Measure-UciLatency.ps1 -Command ucinewgame

.EXAMPLE
    # Calibration: a command of known duration. The report should read ~200 ms.
    .\Measure-UciLatency.ps1 -Command 'go movetime 200' -CompletionMarker bestmove
#>
[CmdletBinding()]
param(
    [string]$Exe = '',

    [Parameter(Mandatory = $true)]
    [string]$Command,

    [string[]]$Setup = @('position startpos moves e2e4 e7e5'),

    [ValidateRange(1, 10000)]
    [int]$Repetitions = 15,

    [ValidateRange(0, 1000)]
    [int]$WarmUp = 1,

    [string]$CompletionMarker = 'readyok',

    [switch]$NoReset,

    [ValidateRange(1, 32)]
    [int]$Threads = 1,

    [ValidateRange(100, 600000)]
    [int]$TimeoutMs = 30000,

    [string]$Csv = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# --- engine plumbing --------------------------------------------------------

function Start-Engine {
    param([string]$ExePath, [string]$WorkDir)

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName               = $ExePath
    $psi.Arguments              = 'uci'
    $psi.WorkingDirectory       = $WorkDir
    $psi.RedirectStandardInput  = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true
    $psi.UseShellExecute        = $false
    return [System.Diagnostics.Process]::Start($psi)
}

function Send-Command {
    param([System.Diagnostics.Process]$Proc, [string]$Text)
    $Proc.StandardInput.Write($Text + "`n")
    $Proc.StandardInput.Flush()
}

function Wait-ForPrefix {
    <#
        Reads lines until one starts with $Prefix, discarding the rest — 'go' emits an 'info' line
        before 'bestmove', and discarding it here is what keeps the next timed window clean.
        Throws on timeout or on the engine exiting mid-probe, naming the phase either way.
    #>
    param(
        [System.Diagnostics.Process]$Proc,
        [string]$Prefix,
        [int]$Timeout,
        [string]$Phase
    )

    $last = '(nothing)'
    while ($true) {
        $task = $Proc.StandardOutput.ReadLineAsync()
        if (-not $task.Wait($Timeout)) {
            throw "Timed out after ${Timeout}ms waiting for '$Prefix' during $Phase. Last line read: $last"
        }
        $line = $task.Result
        if ($null -eq $line) {
            throw "Engine exited while waiting for '$Prefix' during $Phase. Last line read: $last"
        }
        $last = $line
        if ($line.StartsWith($Prefix)) { return $line }
    }
}

function Invoke-Setup {
    <# Reset (unless -NoReset), send the setup lines, and fence with isready/readyok. #>
    param([System.Diagnostics.Process]$Proc, [bool]$Reset)

    if ($Reset) { Send-Command $Proc 'ucinewgame' }
    foreach ($line in $Setup) { Send-Command $Proc $line }
    Send-Command $Proc 'isready'
    Wait-ForPrefix -Proc $Proc -Prefix 'readyok' -Timeout $TimeoutMs -Phase 'setup' | Out-Null
}

function Invoke-Case {
    <#
        One case in a fresh process: handshake, then (WarmUp + Repetitions) rounds of
        [reset + setup] followed by a timed window. Returns the kept samples in milliseconds.

        -IncludeCommand $false is the control: the same round trip with the command under test
        left out.
    #>
    param(
        [string]$ExePath,
        [string]$WorkDir,
        [bool]$IncludeCommand
    )

    $proc = Start-Engine -ExePath $ExePath -WorkDir $WorkDir
    $samples = [System.Collections.Generic.List[double]]::new()

    try {
        Send-Command $proc 'uci'
        Wait-ForPrefix -Proc $proc -Prefix 'uciok' -Timeout $TimeoutMs -Phase 'handshake' | Out-Null
        Send-Command $proc "setoption name Threads value $Threads"
        Send-Command $proc 'isready'
        Wait-ForPrefix -Proc $proc -Prefix 'readyok' -Timeout $TimeoutMs -Phase 'handshake' | Out-Null

        # With -NoReset the setup is established once and repetitions run against whatever state
        # the previous one left behind — which is the point of the switch.
        if ($NoReset) { Invoke-Setup -Proc $proc -Reset $false }

        # The control never waits on the command's own marker: with -CompletionMarker bestmove
        # there would be no bestmove to wait for. It stays the isready round trip, and the report
        # labels it as overhead rather than a subtractable baseline.
        $marker = if ($IncludeCommand) { $CompletionMarker } else { 'readyok' }

        for ($i = 0; $i -lt ($WarmUp + $Repetitions); $i++) {
            if (-not $NoReset) { Invoke-Setup -Proc $proc -Reset $true }

            $sw = [System.Diagnostics.Stopwatch]::StartNew()
            if ($IncludeCommand) { Send-Command $proc $Command }
            if ($marker -eq 'readyok') { Send-Command $proc 'isready' }
            Wait-ForPrefix -Proc $proc -Prefix $marker -Timeout $TimeoutMs -Phase 'timed window' | Out-Null
            $sw.Stop()

            if ($i -ge $WarmUp) { $samples.Add($sw.Elapsed.TotalMilliseconds) }
        }

        Send-Command $proc 'quit'
        if (-not $proc.WaitForExit(10000)) { $proc.Kill() }
    }
    finally {
        if (-not $proc.HasExited) { $proc.Kill() }
    }

    return $samples
}

# --- statistics -------------------------------------------------------------

function Get-Median {
    param([double[]]$Values)
    $sorted = $Values | Sort-Object
    $n = $sorted.Count
    if ($n % 2 -eq 1) { return $sorted[[int](($n - 1) / 2)] }
    return ($sorted[$n / 2 - 1] + $sorted[$n / 2]) / 2
}

# --- main -------------------------------------------------------------------

if (-not $Exe) {
    $Exe = & (Join-Path $PSScriptRoot 'Get-BuildArtifact.ps1')
}
$exePath = (Resolve-Path $Exe).Path
$workDir = Split-Path -Parent $exePath

# Run from the engine's own directory, as Run-Bench.ps1 does, so any file the engine writes lands
# where it expects.
$looksLikeSearch = $Command.TrimStart().StartsWith('go')
$setupHasMoves   = @($Setup | Where-Object { $_ -match '\bmoves\b' }).Count -gt 0

Write-Host ""
Write-Host "Engine    : $exePath"
Write-Host "Command   : $Command"
Write-Host "Setup     : $(if ($Setup) { $Setup -join ' ; ' } else { '(none)' })"
Write-Host "Marker    : $CompletionMarker    Threads: $Threads    n: $Repetitions (+$WarmUp warm-up)"
Write-Host "Reset     : $(if ($NoReset) { 'off — repetitions share accumulated state' } else { 'ucinewgame + setup before every repetition' })"
Write-Host ""

if ($looksLikeSearch -and $CompletionMarker -eq 'readyok') {
    Write-Host "WARNING: 'go' returns immediately and 'isready' is answered during a search, so"     -ForegroundColor Yellow
    Write-Host "         'readyok' will come back mid-search and time nothing. Use"                  -ForegroundColor Yellow
    Write-Host "         -CompletionMarker bestmove."                                                -ForegroundColor Yellow
    Write-Host ""
}
if ($looksLikeSearch -and -not $setupHasMoves) {
    Write-Host "WARNING: measuring a search from a position with no moves played. The first move"    -ForegroundColor Yellow
    Write-Host "         is not a representative code path — that is what made issue #213's"         -ForegroundColor Yellow
    Write-Host "         measurement wrong. Pass a -Setup that drives past move 1."                  -ForegroundColor Yellow
    Write-Host ""
}

Write-Host "Running control case ..."
$control = @(Invoke-Case -ExePath $exePath -WorkDir $workDir -IncludeCommand $false)
Write-Host "Running measured case ..."
$measured = @(Invoke-Case -ExePath $exePath -WorkDir $workDir -IncludeCommand $true)

$controlStats  = [pscustomobject]@{
    Case = 'control (isready)'; Median = Get-Median $control;  Min = ($control  | Measure-Object -Minimum).Minimum; Max = ($control  | Measure-Object -Maximum).Maximum
}
$measuredStats = [pscustomobject]@{
    Case = $Command;            Median = Get-Median $measured; Min = ($measured | Measure-Object -Minimum).Minimum; Max = ($measured | Measure-Object -Maximum).Maximum
}

Write-Host ""
Write-Host ("{0,-34} {1,10} {2,10} {3,10}" -f 'case', 'median ms', 'min ms', 'max ms')
Write-Host ('-' * 66)
foreach ($row in @($controlStats, $measuredStats)) {
    Write-Host ("{0,-34} {1,10:N2} {2,10:N2} {3,10:N2}" -f $row.Case, $row.Median, $row.Min, $row.Max)
}
Write-Host ('-' * 66)

if ($CompletionMarker -eq 'readyok') {
    Write-Host ("{0,-34} {1,10:N2}" -f 'difference (the measurement)', ($measuredStats.Median - $controlStats.Median))
} else {
    Write-Host "The control is an 'isready' round trip, not this marker's baseline: read it as"
    Write-Host "harness overhead, and the measured median as the command's own cost."
}
Write-Host ""

if ($controlStats.Median -gt 1.0) {
    Write-Host "WARNING: the control alone reads above 1 ms, so harness overhead dominates and any"  -ForegroundColor Yellow
    Write-Host "         sub-millisecond figure here is not trustworthy."                            -ForegroundColor Yellow
    Write-Host ""
}

if ($Csv) {
    $rows = [System.Collections.Generic.List[object]]::new()
    for ($i = 0; $i -lt $control.Count;  $i++) { $rows.Add([pscustomobject]@{ Case = 'control'; Sample = $i + 1; Ms = $control[$i] }) }
    for ($i = 0; $i -lt $measured.Count; $i++) { $rows.Add([pscustomobject]@{ Case = $Command;  Sample = $i + 1; Ms = $measured[$i] }) }
    $rows | Export-Csv -Path $Csv -NoTypeInformation
    Write-Host "CSV written: $Csv"
    Write-Host ""
}
