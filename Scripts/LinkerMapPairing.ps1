<#
.SYNOPSIS
    Pairs one engine function across two linker maps, dot-sourced by New-OrderedBuildPair.ps1 and
    Compare-SearchEquivalence.ps1, so both decide the same way which functions only one build has.

.NOTES
    No param() block: this is a library. New-OrderedBuildPair.ps1 -SelfTest covers it; the link is
    its $SelfTestCoverers entry in Validate-PrePR.ps1. Works on Get-MapCodeSymbol records
    (LinkerMap.ps1), which the caller dot-sources itself.
#>

Set-StrictMode -Version Latest

# Objects lld-link's ThinLTO backend produced, named '<exe>.lto.<source>.obj': the engine's own
# code. Runtime-library objects ('msvcrt:...') are not COMDAT sections /order can move.
$script:EngineObjectMarker = '.lto.'

function Get-NormalizedSymbolName {
    <#
      .SYNOPSIS
        The name with per-build hashes removed, so the same function pairs across trees: the
        anonymous-namespace one (?A0x<hash>@), and the '.llvm.<n>' suffix ThinLTO gives a local it
        promotes across modules. The suffix goes entirely, so a function promoted in one build only
        still pairs with itself.
    #>
    param([Parameter(Mandatory)][string]$Name)
    return [regex]::Replace([regex]::Replace($Name, '\?A0x[0-9a-fA-F]{1,8}@', '?A0x@'), '\.llvm\.\d+$', '')
}

function Test-SectionLeader {
    <#
      .SYNOPSIS
        Whether a symbol starts its own section, the unit /order moves. Local labels ($ehgcr_...)
        and exception funclets (?dtor$12@..., ?catch$107@...) live inside their parent function's
        section, and their numbers differ between trees.
    #>
    param([Parameter(Mandatory)][string]$Name)
    return -not ($Name.StartsWith('$') -or $Name -match '^\?[a-z]+\$\d+@')
}

function Get-SymbolPlacement {
    <#
      .SYNOPSIS
        The map's code symbols with Key and Size added. Size is the gap to the next higher
        section leader, so it covers a function's funclets and the padding after it; the last
        one's is $null. Wrap the call site in @().
    #>
    param([Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Symbol)

    $leaderAddresses = @($Symbol | Where-Object { Test-SectionLeader -Name $_.Name } | ForEach-Object { $_.Address } | Sort-Object -Unique)

    $nextLeader = 0
    $placed = foreach ($entry in ($Symbol | Sort-Object -Property Address -Stable)) {
        while ($nextLeader -lt $leaderAddresses.Count -and $leaderAddresses[$nextLeader] -le $entry.Address) { $nextLeader++ }
        [pscustomobject]@{
            Name    = $entry.Name
            Key     = Get-NormalizedSymbolName -Name $entry.Name
            Address = $entry.Address
            Size    = if ($nextLeader -lt $leaderAddresses.Count) { $leaderAddresses[$nextLeader] - $entry.Address } else { $null }
            Object  = $entry.Object
        }
    }
    return @($placed)
}

function Get-UniqueEngineSymbol {
    <# Key -> placed symbol, for engine section leaders whose key occurs once. #>
    param([Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Placed)

    $byKey = @{}
    $duplicate = @{}
    foreach ($entry in $Placed) {
        if (-not $entry.Object.Contains($script:EngineObjectMarker)) { continue }
        if (-not (Test-SectionLeader -Name $entry.Name)) { continue }
        if ($byKey.ContainsKey($entry.Key)) { $duplicate[$entry.Key] = $true; continue }
        $byKey[$entry.Key] = $entry
    }
    foreach ($key in $duplicate.Keys) { $byKey.Remove($key) }
    return $byKey
}

function Get-UnpairedEngineFunction {
    <#
      .SYNOPSIS
        Engine functions only one map has, given each map's Get-UniqueEngineSymbol table. One
        only in the candidate is new code, or one the baseline inlined and the candidate calls:
        a per-node helper moved out of its caller's file stops being inlined, and node-identical
        search cannot show it.
    #>
    param(
        [Parameter(Mandatory)][hashtable]$BaselineByKey,
        [Parameter(Mandatory)][hashtable]$CandidateByKey
    )
    return [pscustomobject]@{
        OnlyBaseline  = @($BaselineByKey.Keys | Where-Object { -not $CandidateByKey.ContainsKey($_) } | ForEach-Object { $BaselineByKey[$_].Name } | Sort-Object)
        OnlyCandidate = @($CandidateByKey.Keys | Where-Object { -not $BaselineByKey.ContainsKey($_) } | ForEach-Object { $CandidateByKey[$_].Name } | Sort-Object)
    }
}
