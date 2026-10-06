<#
.SYNOPSIS
    Quiet-machine notices for measuring scripts: a phase plan printed at start, and a timestamped
    banner at each phase boundary saying whether the machine must be left alone, and until when.

.NOTES
    No param() block: this is a library. Dot-source it as `. (Join-Path $PSScriptRoot 'QuietWindow.ps1')`.
    Measure-CpuProfile.ps1 -SelfTest covers it; the link is its $SelfTestCoverers entry in
    Validate-PrePR.ps1.

    A phase is a hashtable: Name, Quiet (timing-sensitive, so background load skews the result),
    Seconds (estimate) and Rough (true while the estimate is a constant rather than a measurement).
#>

Set-StrictMode -Version Latest

function Format-PhaseDuration {
    <# '~45 s' under two minutes, else '~3 min'; '?' when there is no estimate. #>
    param([AllowNull()][object]$Seconds)

    if ($null -eq $Seconds) { return '~?' }
    $s = [math]::Ceiling([double]$Seconds)
    if ($s -lt 120) { return "~$s s" }
    return "~$([math]::Ceiling($s / 60)) min"
}

function Format-PhaseBanner {
    <#
        One boundary line. A quiet phase names the clock time it ends, rounded up to the minute,
        so a reader can walk away and know when to stop touching the machine's load.
    #>
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][bool]$Quiet,
        [AllowNull()][object]$Seconds,
        [bool]$Rough = $false,
        [Parameter(Mandatory)][datetime]$Now
    )

    $duration = Format-PhaseDuration -Seconds $Seconds
    $roughNote = if ($Rough) { ' (rough)' } else { '' }
    $stamp = $Now.ToString('HH:mm:ss', [cultureinfo]::InvariantCulture)
    if (-not $Quiet) { return "[$stamp] Machine free: $Name, $duration$roughNote" }

    if ($null -eq $Seconds) { return "[$stamp] QUIET NEEDED: $Name, $duration" }
    $until = $Now.AddSeconds([math]::Ceiling([double]$Seconds))
    if ($until.Second -gt 0 -or $until.Millisecond -gt 0) {
        $until = $until.AddSeconds(60 - $until.Second).AddMilliseconds(-$until.Millisecond)
    }
    return "[$stamp] QUIET NEEDED: $Name, $duration$roughNote, until ~$($until.ToString('HH:mm', [cultureinfo]::InvariantCulture))"
}

function Format-PhasePlan {
    <# The plan table: one row per phase, then the total and the quiet share of it. #>
    param([Parameter(Mandatory)][object[]]$Phase)

    $width = ($Phase | ForEach-Object { $_.Name.Length } | Measure-Object -Maximum).Maximum
    $lines = foreach ($p in $Phase) {
        $mode = if ($p.Quiet) { 'QUIET' } else { 'free ' }
        $roughNote = if ($p.Rough) { ' (rough)' } else { '' }
        "  {0}  {1}  {2}{3}" -f $mode, $p.Name.PadRight($width), (Format-PhaseDuration -Seconds $p.Seconds), $roughNote
    }
    $quietSeconds = ($Phase | Where-Object { $_.Quiet } | ForEach-Object { [double]$_.Seconds } | Measure-Object -Sum).Sum
    $totalSeconds = ($Phase | ForEach-Object { [double]$_.Seconds } | Measure-Object -Sum).Sum
    return @($lines) + "  total $(Format-PhaseDuration -Seconds $totalSeconds), of which quiet $(Format-PhaseDuration -Seconds $quietSeconds)"
}

function Write-PhasePlan {
    param([Parameter(Mandatory)][object[]]$Phase)
    Write-Host '==> Phase plan (QUIET = leave the machine idle; free = use it, the phase only runs slower)' -ForegroundColor Cyan
    Format-PhasePlan -Phase $Phase | ForEach-Object { Write-Host $_ }
}

function Write-PhaseBanner {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][bool]$Quiet,
        [AllowNull()][object]$Seconds,
        [bool]$Rough = $false
    )
    $colour = if ($Quiet) { 'Yellow' } else { 'Green' }
    Write-Host ''
    Write-Host (Format-PhaseBanner -Name $Name -Quiet $Quiet -Seconds $Seconds -Rough $Rough -Now (Get-Date)) -ForegroundColor $colour
}
