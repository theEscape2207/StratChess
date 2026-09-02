<#
.SYNOPSIS
    Shared fixed-depth UCI driver, dot-sourced by Compare-SearchEquivalence.ps1 and
    Run-Bench.ps1 so the two cannot drift in how they drive and shut down an engine.

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
