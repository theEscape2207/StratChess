<#
.SYNOPSIS
    What a diff-scoped guard checks: the changed files it watches, or everything.

.NOTES
    No param() block: this is a library. Dot-source it as `. (Join-Path $PSScriptRoot 'GuardScope.ps1')`.
    Test-ScriptTraps.ps1 -SelfTest covers it; the link is its $SelfTestCoverers entry in
    Validate-PrePR.ps1.

    A scope is one of three modes. DiffFailed and DetectorChanged mean check everything; a guard
    runs its own self-test first on DetectorChanged, so a broken detector fails instead of passing
    the whole tree. Scoped carries the changed files the guard watches, possibly none.
#>

Set-StrictMode -Version Latest

# Changing either changes what every scoped run checks.
$script:SharedDetectorFiles = @('Scripts/Get-ChangeTier.ps1', 'Scripts/GuardScope.ps1')

function Resolve-GuardScope {
    <#
      .SYNOPSIS
        A guard's scope from a Get-ChangeTier.ps1 result. -Watch holds -like patterns over
        repository-relative paths; -Detector names the guard's own files.
    #>
    param(
        [Parameter(Mandatory)][object]$Change,
        [Parameter(Mandatory)][string[]]$Detector,
        [Parameter(Mandatory)][string[]]$Watch
    )

    $changed = @($Change.ChangedFiles)
    $detectorChanged = @(@($Detector) + $script:SharedDetectorFiles | Where-Object { $changed -contains $_ })
    $mode = if ($Change.DiffFailed) { 'DiffFailed' }
            elseif ($detectorChanged.Count -gt 0) { 'DetectorChanged' }
            else { 'Scoped' }
    $watched = @($changed | Where-Object { $path = $_; @($Watch | Where-Object { $path -like $_ }).Count -gt 0 })

    return [pscustomobject]@{
        Mode            = $mode
        DetectorChanged = $detectorChanged
        Files           = $watched
    }
}

function Get-GuardScope {
    <# Resolve-GuardScope over the diff since -BaseRef. #>
    param(
        [Parameter(Mandatory)][string]$BaseRef,
        [Parameter(Mandatory)][string[]]$Detector,
        [Parameter(Mandatory)][string[]]$Watch
    )

    $change = & (Join-Path $PSScriptRoot 'Get-ChangeTier.ps1') -BaseRef $BaseRef
    return Resolve-GuardScope -Change $change -Detector $Detector -Watch $Watch
}
