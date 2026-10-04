<#
.SYNOPSIS
    Linker-map reader, dot-sourced by Test-CodeAlignment.ps1 and New-OrderedBuildPair.ps1 so
    both read code addresses the same way.

.NOTES
    No param() block: this is a library. Test-CodeAlignment.ps1 -SelfTest covers it; the link is
    its $SelfTestCoverers entry in Validate-PrePR.ps1.
#>

Set-StrictMode -Version Latest

# The functions placement experiments are about. Matched on the mangled name's prefix, never
# with -like: '?' is a single-character wildcard in a PowerShell wildcard pattern, so
# '?pvs@AIPerplex@@*' would also match names that merely begin with some character followed by
# 'pvs@'. The exception-handling funclets the compiler emits -- '?dtor$254@?0??pvs@AIPerplex@@...'
# -- contain the hot function's mangled name as a substring and are not it; a StartsWith test
# excludes them, and Test-CodeAlignment.ps1 -SelfTest asserts that it does.
$script:MapHotFunction = @(
    [pscustomobject]@{ Label = 'AIPerplex::pvs';        MangledPrefix = '?pvs@AIPerplex@@' }
    [pscustomobject]@{ Label = 'AIPerplex::quiescence'; MangledPrefix = '?quiescence@AIPerplex@@' }
)

# A publics-by-value row: ' 0001:00009700  ?pvs@... 000000014000a700  <obj>'. Segment 0001 is
# .text in this link; the third field is the loaded address, which is what alignment and
# placement are properties of.
$script:MapSymbolRowPattern = '^\s*(?<seg>[0-9a-fA-F]{4}):(?<off>[0-9a-fA-F]{8})\s+(?<name>\S+)\s+(?<addr>[0-9a-fA-F]{16})\s+(?<obj>\S.*?)\s*$'

function Get-MapCodeSymbol {
    <#
      .SYNOPSIS
        One record per code symbol in the map: mangled Name, loaded Address and Object.
        Wrap the call site in @(): one symbol unrolls on return.
    #>
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Line
    )

    $symbols = [System.Collections.Generic.List[object]]::new()
    foreach ($text in $Line) {
        $match = [regex]::Match($text, $script:MapSymbolRowPattern)
        if (-not $match.Success) { continue }
        if ($match.Groups['seg'].Value -ne '0001') { continue }

        $symbols.Add([pscustomobject]@{
            Name    = $match.Groups['name'].Value
            Address = [System.Convert]::ToUInt64($match.Groups['addr'].Value, 16)
            Object  = $match.Groups['obj'].Value
        })
    }

    return $symbols
}

function Find-MapHotSymbol {
    <#
      .SYNOPSIS
        The first symbol whose name starts with $MangledPrefix, or $null.
    #>
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Symbol,
        [Parameter(Mandatory)][string]$MangledPrefix
    )

    # StartsWith, not -like: see the note on $script:MapHotFunction.
    foreach ($candidate in $Symbol) {
        if ($candidate.Name.StartsWith($MangledPrefix, [System.StringComparison]::Ordinal)) { return $candidate }
    }
    return $null
}
