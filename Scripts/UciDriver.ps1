<#
.SYNOPSIS
    Shared fixed-depth UCI driver, dot-sourced by Run-Bench.ps1,
    Compare-SearchEquivalence.ps1 and Compare-SearchProfile.ps1 so they cannot drift in
    how they request a search, decide it completed, and shut an engine down.

.NOTES
    No param() block: this is a library, not a script. Dot-source it as
    `. (Join-Path $PSScriptRoot 'UciDriver.ps1')`, the way build.ps1 and
    Get-BuildArtifact.ps1 take BuildFreshness.ps1.

    Test-UciDriver.ps1 -SelfTest covers this file, driving it against FakeUciEngine.ps1
    rather than a real engine. Validate-PrePR.ps1 runs that self-test when this file
    changes; the link is its $SelfTestCoverers entry, since a dot-sourced library has no
    param() block of its own to hang a -SelfTest switch on. Adding one is not the fix:
    Run-Bench.ps1 dot-sources this file above its own self-test block, so a $SelfTest
    parameter here would overwrite the caller's and silently turn `Run-Bench.ps1
    -SelfTest` into a no-op.
#>

Set-StrictMode -Version Latest

function Invoke-UciSearchToBestMove {
    <#
        Send a fixed-depth UCI request, keeping stdin open until the engine has
        answered it. Reading stdout line-by-line while stderr drains asynchronously
        avoids both the queued-quit race -- batching `quit` behind `go` loses the
        pending-stop race -- and pipe back-pressure deadlocks.
    #>
    param(
        [Parameter(Mandatory)][string]$ExePath,
        [Parameter(Mandatory)][string]$WorkDir,
        [Parameter(Mandatory)][string[]]$Commands,
        [Parameter(Mandatory)][int]$SearchDepth,
        [Parameter(Mandatory)][string]$Description,
        # Wall clock for the whole exchange, search and shutdown together. The default
        # is the ceiling both callers have always used; only the self-test lowers it,
        # so that a timeout case costs seconds rather than ten minutes.
        [ValidateRange(1, 3600000)][int]$TimeoutMs = 600000
    )

    $timeoutLabel = "$([math]::Round($TimeoutMs / 1000, 2))s"

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName               = $ExePath
    $psi.Arguments              = 'uci'
    $psi.WorkingDirectory       = $WorkDir
    $psi.RedirectStandardInput  = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true
    $psi.UseShellExecute        = $false

    $proc = [System.Diagnostics.Process]::Start($psi)
    $stderrTask = $proc.StandardError.ReadToEndAsync()
    $out = [System.Text.StringBuilder]::new()
    $timer = [System.Diagnostics.Stopwatch]::StartNew()

    function Get-UciFailureMessage {
        param([Parameter(Mandatory)][string]$Reason)

        if (-not $proc.HasExited) {
            $proc.Kill()
            $proc.WaitForExit()
        }
        # Waits for end of stream, not for the process: any child still holding the
        # engine's stderr handle keeps this blocked. A real engine has none.
        $stderr = $stderrTask.GetAwaiter().GetResult()
        return "$Reason`nEngine stderr:`n$stderr`nEngine output:`n$out"
    }

    try {
        foreach ($command in $Commands) {
            $proc.StandardInput.WriteLine($command)
        }
        $proc.StandardInput.Flush()

        $gotBestMove = $false
        while (-not $gotBestMove) {
            $remaining = $TimeoutMs - [int]$timer.ElapsedMilliseconds
            if ($remaining -le 0) {
                throw (Get-UciFailureMessage "Engine did not finish within $timeoutLabel (depth $SearchDepth): $Description")
            }

            $lineTask = $proc.StandardOutput.ReadLineAsync()
            if (-not $lineTask.Wait($remaining)) {
                throw (Get-UciFailureMessage "Engine did not finish within $timeoutLabel (depth $SearchDepth): $Description")
            }
            $line = $lineTask.GetAwaiter().GetResult()
            if ($null -eq $line) { break }
            [void]$out.AppendLine($line)
            if ($line -match '^bestmove \S+') { $gotBestMove = $true }
        }

        if (-not $gotBestMove) {
            throw (Get-UciFailureMessage "Engine exited before bestmove (depth $SearchDepth): $Description")
        }

        $proc.StandardInput.WriteLine('quit')
        $proc.StandardInput.Flush()
        $proc.StandardInput.Close()

        $remaining = $TimeoutMs - [int]$timer.ElapsedMilliseconds
        if ($remaining -le 0 -or -not $proc.WaitForExit($remaining)) {
            throw (Get-UciFailureMessage "Engine did not exit within $timeoutLabel after bestmove (depth $SearchDepth): $Description")
        }

        [void]$out.Append($proc.StandardOutput.ReadToEnd())
        $stderr = $stderrTask.GetAwaiter().GetResult()
        if ($proc.ExitCode -ne 0) {
            throw "Engine exited with code $($proc.ExitCode) (depth $SearchDepth): $Description`nEngine stderr:`n$stderr`nEngine output:`n$out"
        }
        return $out.ToString()
    }
    finally {
        if (-not $proc.HasExited) {
            $proc.Kill()
            $proc.WaitForExit()
        }
        $proc.Dispose()
    }
}

function Test-UciFixedDepthComplete {
    <#
        True when a transcript completed the requested depth with a best move. An early
        bestmove -- a search stopped, aborted or ended short -- is not a measurement.
    #>
    param(
        [Parameter(Mandatory)][AllowEmptyString()][string]$Output,
        [Parameter(Mandatory)][int]$SearchDepth
    )

    $lines = @($Output -split "`r?`n" | ForEach-Object { $_.Trim() })
    $reachedDepth = @($lines | Where-Object { $_ -match "^info depth $SearchDepth\b" }).Count -gt 0
    $hasBestMove  = @($lines | Where-Object { $_ -match '^bestmove \S+' }).Count -gt 0
    return $reachedDepth -and $hasBestMove
}

function Invoke-UciFixedDepthSearch {
    <#
        One fixed-depth search in a fresh engine process, so no transposition-table state
        carries over between positions. Returns the raw transcript, and throws unless the
        engine completed -SearchDepth with a best move; interpreting the transcript is the
        caller's job.
    #>
    param(
        [Parameter(Mandatory)][string]$ExePath,
        [Parameter(Mandatory)][string]$WorkDir,
        # Everything after 'position ': 'startpos', 'fen <fen>', either with 'moves ...'.
        [Parameter(Mandatory)][string]$Position,
        [Parameter(Mandatory)][ValidateRange(1, 1000)][int]$SearchDepth,
        [Parameter(Mandatory)][ValidateRange(1, 1024)][int]$Threads,
        [Parameter(Mandatory)][string]$Description,
        [ValidateRange(1, 3600000)][int]$TimeoutMs = 600000
    )

    $commands = @(
        'uci'
        'isready'
        "setoption name Threads value $Threads"
        "position $Position"
        "go depth $SearchDepth"
    )
    $out = Invoke-UciSearchToBestMove -ExePath $ExePath -WorkDir $WorkDir -Commands $commands `
        -SearchDepth $SearchDepth -Description $Description -TimeoutMs $TimeoutMs

    if (-not (Test-UciFixedDepthComplete -Output $out -SearchDepth $SearchDepth)) {
        throw "Fixed-depth search did not complete depth $($SearchDepth): $Description`nEngine output:`n$out"
    }
    return $out
}
