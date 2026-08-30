<#
.SYNOPSIS
    Shared fixed-depth UCI driver, dot-sourced by Compare-SearchEquivalence.ps1 and
    Run-Bench.ps1 so the two cannot drift in how they drive and shut down an engine.

.NOTES
    No param() block: this is a library, not a script. Dot-source it as
    `. (Join-Path $PSScriptRoot 'UciDriver.ps1')`, the way build.ps1 and
    Get-BuildArtifact.ps1 take BuildFreshness.ps1.

    Nothing here is covered by a -SelfTest -- it spawns a real engine, so the callers'
    own runs are what exercise it. Issue #425 tracks a fake-engine fixture that would
    change that.
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
        [Parameter(Mandatory)][string]$Description
    )

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
            $remaining = 600000 - [int]$timer.ElapsedMilliseconds
            if ($remaining -le 0) {
                throw (Get-UciFailureMessage "Engine did not finish within 600s (depth $SearchDepth): $Description")
            }

            $lineTask = $proc.StandardOutput.ReadLineAsync()
            if (-not $lineTask.Wait($remaining)) {
                throw (Get-UciFailureMessage "Engine did not finish within 600s (depth $SearchDepth): $Description")
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

        $remaining = 600000 - [int]$timer.ElapsedMilliseconds
        if ($remaining -le 0 -or -not $proc.WaitForExit($remaining)) {
            throw (Get-UciFailureMessage "Engine did not exit within 600s after bestmove (depth $SearchDepth): $Description")
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
