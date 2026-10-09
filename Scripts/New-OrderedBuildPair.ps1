<#
.SYNOPSIS
    Relink two built trees with a shared /ORDER so their unchanged functions sit at identical
    addresses, for a Compare-Bench.ps1 rerun that rules code placement out.

.DESCRIPTION
    Code placement alone moves node-identical builds by several percent of nps (#555). A
    Compare-Bench Speedup verdict, or a Slowdown or Unresolved one being escalated, is confirmed
    by rerunning the comparison on a pair whose hot code sits at the same addresses. This script
    produces that pair. It builds nothing: both trees must already be built with
    `build.ps1 main` (Release, clang-cl).

    For each tree it:
      1. reruns the tree's own link command, taken from `ninja -t commands`, into the output
         directory with a /MAP. The result must be byte-identical to the tree's shipping exe,
         which proves the relink is faithful and its map describes what was benchmarked.
      2. pairs the engine's functions across both maps. Anonymous-namespace names carry a
         per-tree hash (?A0x<hash>@), so names are compared with it normalised.
      3. pins every paired function whose size is equal in both maps, in baseline address
         order, through an order file written in each tree's own names. With equal sizes in an
         equal order, every pinned function lands at the same address in both images. A size
         includes the function's exception funclets, which live in its section.
      4. lists the hot functions whose own size changed right after them. The first one
         follows an identical prefix, so it still starts at the same address in both. Before
         each later one it lists spacers: cold pinned functions whose sizes add up to the
         previous hot function's size change, in the order file of the image where that
         function is smaller only, so both images should reach the next hot function at the
         same address. Sizes are gaps in the as-built maps, padding included, so step 5's
         check, not the arithmetic, decides. A spacer leaves both images' prefix, so it is
         neither pinned nor counted.
         Every other function goes after these.
      5. relinks with /order and checks the result. Every pinned function and every hot
         function must start at identical addresses in both, or the script fails. A change that
         resized two hot functions fails when no cold subset matches the earlier one's size
         change exactly.

    The hot functions are LinkerMap.ps1's AIPerplex::pvs and AIPerplex::quiescence, plus
    Board::DoMove and Board::DoNullMove, which run once per node and which a make/unmake change
    resizes (#776). The extra two are listed here, not in LinkerMap.ps1, whose list
    Test-CodeAlignment.ps1's fixtures assume holds two functions.

    The tree's shipping exe, map and PDB are never written; the script checks the exe's hash
    afterwards. The ordered exes are for comparison only, never a shipped layout.

    A tree without /Brepro in its link (before #580) fails step 1: its exe carries a link
    timestamp, so no relink reproduces it.

.PARAMETER BaselineTree
    Root of the baseline worktree, normally a detached worktree at the merge base.

.PARAMETER CandidateTree
    Root of the candidate worktree. It may be the baseline's, for an A/A pair.

.PARAMETER OutDir
    Empty or new directory for the pair. Defaults to a timestamped directory under %TEMP%.
    Receives baseline\ and candidate\, each with StratChessEvolved.exe, its map and order.txt,
    plus metadata.json.

.PARAMETER SelfTest
    Run the map-pairing, order-file, link-line and placement-check cases and exit. Pure: no
    build, no toolchain.

.EXAMPLE
    .\New-OrderedBuildPair.ps1 -BaselineTree C:\wt\base -CandidateTree C:\wt\cand
    .\Compare-Bench.ps1 -Baseline <out>\baseline\StratChessEvolved.exe -Candidate <out>\candidate\StratChessEvolved.exe -Control
#>

[CmdletBinding(DefaultParameterSetName = 'Run')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Run')]
    [string]$BaselineTree,

    [Parameter(Mandatory, ParameterSetName = 'Run')]
    [string]$CandidateTree,

    [Parameter(ParameterSetName = 'Run')]
    [string]$OutDir = '',

    [Parameter(Mandatory, ParameterSetName = 'SelfTest')]
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'LinkerMap.ps1')

# The shipping build's directory inside a tree: Release, clang-cl.
$script:ShippingBuildDir = 'build\windows-clang-cl'

# Objects lld-link's ThinLTO backend produced, named '<exe>.lto.<source>.obj': the engine's own
# code. Runtime-library objects ('msvcrt:...') are not COMDAT sections /order can move.
$script:EngineObjectMarker = '.lto.'

# Translation units off the search path. A spacer sits at different addresses in the two images,
# so it must be code the bench does not run per node.
$script:SpacerObjectPattern = '\.lto\.(ArgParse|Config|FENParser|Game|HumanPlayer|Logger|MoveFormatter|Perft|PlayerFactory|SearchTuningSchema|TacticalTestRunner|UCIHandler)\.cpp\.obj$'

# The functions whose placement the pair controls, in order-file order after the pinned prefix.
$script:OrderedHotFunction = @($script:MapHotFunction) + @(
    [pscustomobject]@{ Label = 'Board::DoMove'; MangledPrefix = '?DoMove@Board@@' }
    [pscustomobject]@{ Label = 'Board::DoNullMove'; MangledPrefix = '?DoNullMove@Board@@' }
)

function Get-NormalizedSymbolName {
    <# The name with anonymous-namespace hashes removed, so the same function pairs across trees. #>
    param([Parameter(Mandatory)][string]$Name)
    return [regex]::Replace($Name, '\?A0x[0-9a-fA-F]{8}@', '?A0x@')
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

function Select-SpacerSet {
    <#
      .SYNOPSIS
        Pool entries whose sizes sum to exactly $Bytes, or $null when no subset does. A subset-sum
        over the reachable totals, which stay few: $Bytes is a hot function's size change, a few
        cache lines. Larger entries are tried first, so the set tends to be short. Wrap a
        non-null result in @().
    #>
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Pool,
        [Parameter(Mandatory)][int64]$Bytes
    )

    # Each reachable total maps to the first set found for it.
    $reachable = @{}
    $reachable[[int64]0] = @()
    foreach ($entry in ($Pool | Sort-Object -Property @{ Expression = 'Size'; Descending = $true }, Key)) {
        $entrySize = [int64]$entry.Size
        if ($entrySize -le 0 -or $entrySize -gt $Bytes) { continue }
        foreach ($total in @($reachable.Keys)) {
            $next = [int64]($total + $entrySize)
            if ($next -le $Bytes -and -not $reachable.ContainsKey($next)) { $reachable[$next] = @($reachable[$total]) + $entry }
        }
        if ($reachable.ContainsKey($Bytes)) { return $reachable[$Bytes] }
    }
    return $null
}

function Get-OrderEntry {
    <#
      .SYNOPSIS
        The functions the order files list: paired by key, equal in size, in baseline address
        order. Then the hot functions whose size differs, marked Resized: the first of them
        follows an identical prefix, so it still starts at the same address in both images.
        Each later one is preceded by spacers (Spacer = the arm whose order file lists them)
        that make up the previous one's size change. Wrap the call site in @().
    #>
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$BaselinePlaced,
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$CandidatePlaced
    )

    $baselineByKey = Get-UniqueEngineSymbol -Placed $BaselinePlaced
    $candidateByKey = Get-UniqueEngineSymbol -Placed $CandidatePlaced

    $pinned = foreach ($entry in ($baselineByKey.Values | Sort-Object -Property Address -Stable)) {
        if (-not $candidateByKey.ContainsKey($entry.Key)) { continue }
        $other = $candidateByKey[$entry.Key]
        if ($null -eq $entry.Size -or $entry.Size -ne $other.Size) { continue }
        [pscustomobject]@{
            Key = $entry.Key; BaselineName = $entry.Name; CandidateName = $other.Name
            Size = $entry.Size; Object = $entry.Object; Resized = $false; Spacer = ''; SizeChange = 0
        }
    }
    $pinned = @($pinned)
    $pinnedKey = @{}
    foreach ($entry in $pinned) { $pinnedKey[$entry.Key] = $true }

    $resized = foreach ($hot in $script:OrderedHotFunction) {
        $inBaseline = Find-MapHotSymbol -Symbol @($baselineByKey.Values) -MangledPrefix $hot.MangledPrefix
        $inCandidate = Find-MapHotSymbol -Symbol @($candidateByKey.Values) -MangledPrefix $hot.MangledPrefix
        if ($null -eq $inBaseline -or $null -eq $inCandidate -or $pinnedKey.ContainsKey($inBaseline.Key)) { continue }
        $sizeChange = if ($null -eq $inBaseline.Size -or $null -eq $inCandidate.Size) { $null } else { [int64]$inCandidate.Size - [int64]$inBaseline.Size }
        [pscustomobject]@{
            Key = $inBaseline.Key; BaselineName = $inBaseline.Name; CandidateName = $inCandidate.Name
            Size = $null; Object = $inBaseline.Object; Resized = $true; Spacer = ''; SizeChange = $sizeChange
        }
    }
    $resizedList = @($resized)

    # Spacers between consecutive resized hot functions. The arm whose hot function is smaller lists
    # them, so both arms should reach the next hot function at the same address; the placement
    # check confirms it.
    $spacerKey = @{}
    $spacersAfter = @{}
    for ($i = 0; $i -lt $resizedList.Count - 1; $i++) {
        $sizeChange = $resizedList[$i].SizeChange
        if ($null -eq $sizeChange -or $sizeChange -eq 0) { continue }
        $pool = @($pinned | Where-Object { -not $spacerKey.ContainsKey($_.Key) -and $_.Object -match $script:SpacerObjectPattern })
        $spacerSet = Select-SpacerSet -Pool $pool -Bytes ([math]::Abs($sizeChange))
        if ($null -eq $spacerSet) { continue }
        $listedIn = if ($sizeChange -gt 0) { 'baseline' } else { 'candidate' }
        $spacersAfter[$i] = @(foreach ($entry in @($spacerSet)) {
                $spacerKey[$entry.Key] = $true
                $spacer = $entry.PSObject.Copy()
                $spacer.Spacer = $listedIn
                $spacer
            })
    }

    $entries = [System.Collections.Generic.List[object]]::new()
    foreach ($entry in $pinned) { if (-not $spacerKey.ContainsKey($entry.Key)) { $entries.Add($entry) } }
    for ($i = 0; $i -lt $resizedList.Count; $i++) {
        $entries.Add($resizedList[$i])
        if ($spacersAfter.ContainsKey($i)) { foreach ($entry in $spacersAfter[$i]) { $entries.Add($entry) } }
    }
    return $entries.ToArray()
}

function Get-ArmOrder {
    <# One arm's order file lines: every entry in its own names, minus the other arm's spacers. #>
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Entry,
        [Parameter(Mandatory)][ValidateSet('baseline', 'candidate')][string]$Arm
    )
    $nameField = if ($Arm -eq 'baseline') { 'BaselineName' } else { 'CandidateName' }
    return @($Entry | Where-Object { -not $_.Spacer -or $_.Spacer -eq $Arm } | ForEach-Object { $_.$nameField })
}

function Get-LinkCommand {
    <#
      .SYNOPSIS
        The engine's link command from `ninja -t commands StratChessEvolved.exe`'s last line,
        unwrapped from its `cmd.exe /C "cd . && ... && cd ."` shell.
    #>
    param([Parameter(Mandatory)][string]$NinjaLine)

    $match = [regex]::Match($NinjaLine, '^.*?/C "cd \. && (?<inner>.+) && cd \."$')
    if (-not $match.Success -or $match.Groups['inner'].Value -notmatch 'vs_link_exe .* -- \S*lld-link') {
        throw "Unrecognised link command (expected cmake -E vs_link_exe ... -- lld-link ...): $NinjaLine"
    }
    return $match.Groups['inner'].Value
}

function New-RelinkCommand {
    <#
      .SYNOPSIS
        The link command with every output redirected into $Directory and an optional /order.
        lld-link takes the last /out, /implib, /pdb and /MAP, so the overrides go at the end.
    #>
    param(
        [Parameter(Mandatory)][string]$LinkCommand,
        [Parameter(Mandatory)][string]$Directory,
        [string]$OrderFile = ''
    )

    $overrides = @(
        "`"/out:$Directory\StratChessEvolved.exe`""
        "`"/implib:$Directory\StratChessEvolved.lib`""
        "`"/pdb:$Directory\StratChessEvolved.pdb`""
        "`"/MAP:$Directory\StratChessEvolved.map`""
    )
    if ($OrderFile) { $overrides += "`"/order:@$OrderFile`"" }
    return "$LinkCommand $($overrides -join ' ')"
}

function Test-OrderedPlacement {
    <#
      .SYNOPSIS
        Verdict over the two ordered maps: hot functions at identical addresses, plus how many
        pinned functions matched.
    #>
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$BaselineSymbol,
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$CandidateSymbol,
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$OrderEntry
    )

    $hotResults = foreach ($hot in $script:OrderedHotFunction) {
        $inBaseline = Find-MapHotSymbol -Symbol $BaselineSymbol -MangledPrefix $hot.MangledPrefix
        $inCandidate = Find-MapHotSymbol -Symbol $CandidateSymbol -MangledPrefix $hot.MangledPrefix
        $found = ($null -ne $inBaseline) -and ($null -ne $inCandidate)
        [pscustomobject]@{
            Label            = $hot.Label
            MangledPrefix    = $hot.MangledPrefix
            BaselineAddress  = if ($inBaseline) { $inBaseline.Address } else { $null }
            CandidateAddress = if ($inCandidate) { $inCandidate.Address } else { $null }
            Ok               = $found -and ($inBaseline.Address -eq $inCandidate.Address)
        }
    }
    $hotList = @($hotResults)

    $baselineAddress = @{}
    foreach ($entry in $BaselineSymbol) { $baselineAddress[$entry.Name] = $entry.Address }
    $candidateAddress = @{}
    foreach ($entry in $CandidateSymbol) { $candidateAddress[$entry.Name] = $entry.Address }

    $equalSize = @($OrderEntry | Where-Object { -not $_.Resized -and -not $_.Spacer })
    $moved = @($equalSize | Where-Object {
            -not ($baselineAddress.ContainsKey($_.BaselineName) -and $candidateAddress.ContainsKey($_.CandidateName) -and
                $baselineAddress[$_.BaselineName] -eq $candidateAddress[$_.CandidateName])
        })
    $matched = $equalSize.Count - $moved.Count

    # Empty maps are a failure, never a vacuous pass: the hot lookups above fail on them too. A moved
    # pinned function fails as well: everything after it shifts, hot callees included.
    $ok = ($BaselineSymbol.Count -gt 0) -and ($CandidateSymbol.Count -gt 0) -and ($matched -eq $equalSize.Count) -and
        -not ($hotList | Where-Object { -not $_.Ok })
    return [pscustomobject]@{
        HotFunction  = $hotList
        PinnedCount  = $equalSize.Count
        MatchedCount = $matched
        FirstMoved   = if ($moved.Count -gt 0) { $moved[0].BaselineName } else { $null }
        Ok           = [bool]$ok
    }
}

function Invoke-SelfTest {
    $script:selfTestFailures = 0
    Write-Host '==> Self-test' -ForegroundColor Cyan

    function Assert-Equal {
        param([Parameter(Mandatory)][string]$Name, [AllowNull()][object]$Actual, [AllowNull()][object]$Expected)
        if ("$Actual" -ceq "$Expected") {
            Write-Host "  PASS  $Name" -ForegroundColor Green
        }
        else {
            Write-Host "  FAIL  $Name (expected '$Expected', got '$Actual')" -ForegroundColor Red
            $script:selfTestFailures++
        }
    }

    function New-Symbol {
        param([string]$Name, [uint64]$Address, [string]$Object = 'StratChessEvolved.exe.lto.AIPerplex.cpp.obj')
        return [pscustomobject]@{ Name = $Name; Address = $Address; Object = $Object }
    }

    $pvs = '?pvs@AIPerplex@@AEAAHAEAUThreadData@@HHHH_NAEAVTranspositionTable@@@Z'
    $qs = '?quiescence@AIPerplex@@AEAAHAEAUThreadData@@HHHHAEAVTranspositionTable@@@Z'
    $doMove = '?DoMove@Board@@QEAA_NAEBVMove@@@Z'
    $doNullMove = '?DoNullMove@Board@@QEAAXXZ'
    $boardObject = 'StratChessEvolved.exe.lto.Board.cpp.obj'

    Assert-Equal 'anonymous-namespace hashes normalise to one key' `
        (Get-NormalizedSymbolName '?f@?A0x12248362@@YAHXZ') (Get-NormalizedSymbolName '?f@?A0x8417fefe@@YAHXZ')
    Assert-Equal 'names outside an anonymous namespace are unchanged' (Get-NormalizedSymbolName $pvs) $pvs

    $placed = @(Get-SymbolPlacement -Symbol @(
            (New-Symbol 'c' 0x1100), (New-Symbol 'a' 0x1000), (New-Symbol 'a2' 0x1000), (New-Symbol 'b' 0x1040)))
    Assert-Equal 'sizes are the gap to the next address, sorted' (($placed | ForEach-Object { "$($_.Name)=$($_.Size)" }) -join ',') 'a=64,a2=64,b=192,c='

    # A function's funclets and labels sit inside its section, so its size runs past them.
    $placed = @(Get-SymbolPlacement -Symbol @(
            (New-Symbol '?f@@YAXXZ' 0x1000), (New-Symbol '?dtor$5@?0??f@@YAXXZ@4HA' 0x1080),
            (New-Symbol '$ehgcr_1_2' 0x1090), (New-Symbol '?g@@YAXXZ' 0x10c0), (New-Symbol '?h@@YAXXZ' 0x1100)))
    Assert-Equal "a function's size covers its funclets and labels" $placed[0].Size 192

    # Baseline and candidate differ in every way the pairing must handle.
    $baseline = @(
        (New-Symbol $pvs 0x1000)
        (New-Symbol '?changed@@YAXXZ' 0x1400)
        (New-Symbol '?anon@?A0x11111111@@YAXXZ' 0x1440)
        (New-Symbol '?gone@@YAXXZ' 0x1480)
        (New-Symbol '?dtor$7@?0??gone@@YAXXZ@4HA' 0x14a0)
        (New-Symbol '$$000000' 0x14c0)
        (New-Symbol '?dup@@YAXXZ' 0x1500)
        (New-Symbol '?dup@@YAXXZ' 0x1540)
        (New-Symbol $qs 0x1580)
        (New-Symbol '_crt_fn' 0x1600 'msvcrt:file_mode.obj')
        (New-Symbol '_crt_tail' 0x1640 'msvcrt:file_mode.obj')
    )
    $candidate = @(
        (New-Symbol $qs 0x0f00)
        (New-Symbol $pvs 0x1000)
        (New-Symbol '?changed@@YAXXZ' 0x1400)
        (New-Symbol '?dtor$9@?0??changed@@YAXXZ@4HA' 0x1480)
        (New-Symbol '?anon@?A0x22222222@@YAXXZ' 0x14c0)
        (New-Symbol '$$000000' 0x14e0)
        (New-Symbol '?dup@@YAXXZ' 0x1500)
        (New-Symbol '?dup@@YAXXZ' 0x1540)
        (New-Symbol '?new@@YAXXZ' 0x1580)
        (New-Symbol '_crt_fn' 0x1600 'msvcrt:file_mode.obj')
        (New-Symbol '_crt_tail' 0x1640 'msvcrt:file_mode.obj')
    )
    $orderEntries = @(Get-OrderEntry -BaselinePlaced @(Get-SymbolPlacement -Symbol $baseline) -CandidatePlaced @(Get-SymbolPlacement -Symbol $candidate))
    # pvs 0x400 both; changed 0x40 vs 0xc0; anon 0x40 both; qs 0x80 vs 0x100. pvs and anon pin; qs trails.
    Assert-Equal 'pins equal-size pairs in baseline order, then resized hot functions; drops the rest' `
        (($orderEntries | ForEach-Object { "$($_.BaselineName):$($_.Resized)" }) -join ',') "${pvs}:False,?anon@?A0x11111111@@YAXXZ:False,${qs}:True"
    Assert-Equal "each order file lists the tree's own names" `
        (($orderEntries | ForEach-Object { $_.CandidateName }) -join ',') "$pvs,?anon@?A0x22222222@@YAXXZ,$qs"

    # Both hot functions resized: pvs grows by 0x40 in the candidate, so the baseline lists a cold
    # 0x40-byte spacer before quiescence. ?hot is the right size but on the search path.
    $cold = 'StratChessEvolved.exe.lto.Config.cpp.obj'
    $spacerBaseline = @(
        (New-Symbol $pvs 0x1000), (New-Symbol $qs 0x1400), (New-Symbol '?hot@@YAXXZ' 0x1480),
        (New-Symbol '?big@@YAXXZ' 0x14c0 $cold), (New-Symbol '?small@@YAXXZ' 0x1540 $cold), (New-Symbol '?end@@YAXXZ' 0x1580 $cold))
    $spacerCandidate = @(
        (New-Symbol $pvs 0x1000), (New-Symbol $qs 0x1440), (New-Symbol '?hot@@YAXXZ' 0x1540),
        (New-Symbol '?big@@YAXXZ' 0x1580 $cold), (New-Symbol '?small@@YAXXZ' 0x1600 $cold), (New-Symbol '?end@@YAXXZ' 0x1640 $cold))
    $basePlaced = @(Get-SymbolPlacement -Symbol $spacerBaseline)
    $candPlaced = @(Get-SymbolPlacement -Symbol $spacerCandidate)
    $spaced = @(Get-OrderEntry -BaselinePlaced $basePlaced -CandidatePlaced $candPlaced)
    $describe = { param($entries) ($entries | ForEach-Object { "$($_.BaselineName):$($_.Resized):$($_.Spacer)" }) -join ',' }
    Assert-Equal 'a grown first hot function gets a cold spacer of its growth, listed by the baseline' (& $describe $spaced) `
        "?hot@@YAXXZ:False:,?big@@YAXXZ:False:,${pvs}:True:,?small@@YAXXZ:False:baseline,${qs}:True:"
    Assert-Equal 'the baseline order file lists the spacer' ((Get-ArmOrder -Entry $spaced -Arm baseline) -join ',') "?hot@@YAXXZ,?big@@YAXXZ,$pvs,?small@@YAXXZ,$qs"
    Assert-Equal 'the candidate order file leaves it out' ((Get-ArmOrder -Entry $spaced -Arm candidate) -join ',') "?hot@@YAXXZ,?big@@YAXXZ,$pvs,$qs"

    $shrunk = @(Get-OrderEntry -BaselinePlaced $candPlaced -CandidatePlaced $basePlaced)
    Assert-Equal 'a shrunk first hot function gets its spacer in the candidate order file' (& $describe $shrunk) `
        "?hot@@YAXXZ:False:,?big@@YAXXZ:False:,${pvs}:True:,?small@@YAXXZ:False:candidate,${qs}:True:"
    Assert-Equal 'there the candidate order file lists the spacer' ((Get-ArmOrder -Entry $shrunk -Arm candidate) -join ',') "?hot@@YAXXZ,?big@@YAXXZ,$pvs,?small@@YAXXZ,$qs"
    Assert-Equal 'and the baseline order file leaves it out' ((Get-ArmOrder -Entry $shrunk -Arm baseline) -join ',') "?hot@@YAXXZ,?big@@YAXXZ,$pvs,$qs"

    # ?small moved onto the search path: only ?big (0x80) stays cold, and it cannot make 0x40.
    $toHotPath = { param($symbols) @($symbols | ForEach-Object { if ($_.Name -eq '?small@@YAXXZ') { New-Symbol $_.Name $_.Address } else { $_ } }) }
    $unspaced = @(Get-OrderEntry -BaselinePlaced @(Get-SymbolPlacement -Symbol (& $toHotPath $spacerBaseline)) `
            -CandidatePlaced @(Get-SymbolPlacement -Symbol (& $toHotPath $spacerCandidate)))
    Assert-Equal 'FALSIFY: no cold subset matches the growth exactly, so no spacer; hot-path functions never qualify' (& $describe $unspaced) `
        "?hot@@YAXXZ:False:,?big@@YAXXZ:False:,?small@@YAXXZ:False:,${pvs}:True:,${qs}:True:"

    # Three resized hot functions, two gaps: pvs, DoMove and DoNullMove each grow by 0x40, and each
    # gap takes its own cold spacer. quiescence is absent, so it is skipped, never a gap.
    $gapBaseline = @(
        (New-Symbol $pvs 0x1000), (New-Symbol $doMove 0x1400 $boardObject), (New-Symbol $doNullMove 0x1480 $boardObject),
        (New-Symbol '?a@@YAXXZ' 0x14c0 $cold), (New-Symbol '?b@@YAXXZ' 0x1500 $cold), (New-Symbol '?end@@YAXXZ' 0x1540 $cold))
    $gapCandidate = @(
        (New-Symbol $pvs 0x1000), (New-Symbol $doMove 0x1440 $boardObject), (New-Symbol $doNullMove 0x1500 $boardObject),
        (New-Symbol '?a@@YAXXZ' 0x1580 $cold), (New-Symbol '?b@@YAXXZ' 0x15c0 $cold), (New-Symbol '?end@@YAXXZ' 0x1600 $cold))
    $gapped = @(Get-OrderEntry -BaselinePlaced @(Get-SymbolPlacement -Symbol $gapBaseline) -CandidatePlaced @(Get-SymbolPlacement -Symbol $gapCandidate))
    Assert-Equal 'each gap between resized hot functions, Board ones included, gets its own spacer' (& $describe $gapped) `
        "${pvs}:True:,?a@@YAXXZ:False:baseline,${doMove}:True:,?b@@YAXXZ:False:baseline,${doNullMove}:True:"

    $pool = @([pscustomobject]@{ Key = 'a'; Size = 0x40 }, [pscustomobject]@{ Key = 'b'; Size = 0x80 }, [pscustomobject]@{ Key = 'c'; Size = 0x40 })
    Assert-Equal 'spacers are taken largest first to an exact sum' ((@(Select-SpacerSet -Pool $pool -Bytes 0xc0) | ForEach-Object { $_.Key }) -join ',') 'b,a'
    Assert-Equal 'FALSIFY: an unreachable sum yields no spacer set' ($null -eq (Select-SpacerSet -Pool $pool -Bytes 0x20)) $true
    $trap = @([pscustomobject]@{ Key = 'x'; Size = 0xc0 }, [pscustomobject]@{ Key = 'y'; Size = 0x80 }, [pscustomobject]@{ Key = 'z'; Size = 0x80 })
    Assert-Equal 'an exact sum is found where largest-first would strand a remainder' ((@(Select-SpacerSet -Pool $trap -Bytes 0x100) | ForEach-Object { $_.Key }) -join ',') 'y,z'

    $spacedMap = @((New-Symbol $pvs 0x1000), (New-Symbol '?hot@@YAXXZ' 0x0f00), (New-Symbol '?big@@YAXXZ' 0x0f40), (New-Symbol $qs 0x1480),
        (New-Symbol $doMove 0x1500 $boardObject), (New-Symbol $doNullMove 0x1580 $boardObject))
    $spacedVerdict = Test-OrderedPlacement -BaselineSymbol $spacedMap -CandidateSymbol $spacedMap -OrderEntry $spaced
    Assert-Equal 'a spacer is neither pinned nor counted' "$($spacedVerdict.MatchedCount)/$($spacedVerdict.PinnedCount)" '2/2'

    $ninjaLine = 'C:\Windows\system32\cmd.exe /C "cd . && "C:\Program Files\CMake\bin\cmake.exe" -E vs_link_exe --intdir=CMakeFiles\X.dir --manifests  -- C:\LLVM\bin\lld-link.exe /nologo a.obj /out:StratChessEvolved.exe /MAP:C:/b/StratChessEvolved.map kernel32.lib && cd ."'
    $link = Get-LinkCommand -NinjaLine $ninjaLine
    Assert-Equal 'the link command is unwrapped from its cmd.exe shell' $link `
        '"C:\Program Files\CMake\bin\cmake.exe" -E vs_link_exe --intdir=CMakeFiles\X.dir --manifests  -- C:\LLVM\bin\lld-link.exe /nologo a.obj /out:StratChessEvolved.exe /MAP:C:/b/StratChessEvolved.map kernel32.lib'
    $threw = $false
    try { Get-LinkCommand -NinjaLine 'C:\Windows\system32\cmd.exe /C "cd . && link.exe /out:x.exe && cd ."' | Out-Null } catch { $threw = $true }
    Assert-Equal 'FALSIFY: a link command not through vs_link_exe and lld-link is refused' $threw $true

    $relink = New-RelinkCommand -LinkCommand $link -Directory 'C:\o' -OrderFile 'C:\o\order.txt'
    Assert-Equal 'outputs are redirected after the original ones, then /order' `
        $relink.Substring($link.Length) ' "/out:C:\o\StratChessEvolved.exe" "/implib:C:\o\StratChessEvolved.lib" "/pdb:C:\o\StratChessEvolved.pdb" "/MAP:C:\o\StratChessEvolved.map" "/order:@C:\o\order.txt"'
    Assert-Equal 'no /order without an order file' ((New-RelinkCommand -LinkCommand $link -Directory 'C:\o') -match '/order') $false

    $boardHot = @((New-Symbol $doMove 0x1600 $boardObject), (New-Symbol $doNullMove 0x1680 $boardObject))
    $orderedBaseline = @((New-Symbol $pvs 0x1000), (New-Symbol $qs 0x1400), (New-Symbol '?anon@?A0x11111111@@YAXXZ' 0x1500)) + $boardHot
    $orderedCandidate = @((New-Symbol $pvs 0x1000), (New-Symbol $qs 0x1400), (New-Symbol '?anon@?A0x22222222@@YAXXZ' 0x1500)) + $boardHot
    $verdict = Test-OrderedPlacement -BaselineSymbol $orderedBaseline -CandidateSymbol $orderedCandidate -OrderEntry $orderEntries
    Assert-Equal 'identical hot addresses pass, and pinned matches count across own names' "$($verdict.Ok) $($verdict.MatchedCount)/$($verdict.PinnedCount)" 'True 2/2'

    $shifted = @((New-Symbol $pvs 0x1000), (New-Symbol $qs 0x1440), (New-Symbol '?anon@?A0x22222222@@YAXXZ' 0x1500)) + $boardHot
    Assert-Equal 'FALSIFY: quiescence at a different address fails' `
        (Test-OrderedPlacement -BaselineSymbol $orderedBaseline -CandidateSymbol $shifted -OrderEntry $orderEntries).Ok $false
    $laterShifted = @((New-Symbol $pvs 0x1000), (New-Symbol $qs 0x1400), (New-Symbol '?anon@?A0x22222222@@YAXXZ' 0x1540)) + $boardHot
    Assert-Equal 'FALSIFY: a pinned function moved after matched hot functions fails' `
        (Test-OrderedPlacement -BaselineSymbol $orderedBaseline -CandidateSymbol $laterShifted -OrderEntry $orderEntries).Ok $false
    $doMoveMoved = @($orderedCandidate | ForEach-Object { if ($_.Name -eq $doMove) { New-Symbol $doMove 0x1640 $boardObject } else { $_ } })
    Assert-Equal 'FALSIFY: DoMove at a different address fails' `
        (Test-OrderedPlacement -BaselineSymbol $orderedBaseline -CandidateSymbol $doMoveMoved -OrderEntry $orderEntries).Ok $false
    $noDoNullMove = @($orderedCandidate | Where-Object { $_.Name -ne $doNullMove })
    Assert-Equal 'FALSIFY: DoNullMove missing from one map fails' `
        (Test-OrderedPlacement -BaselineSymbol $orderedBaseline -CandidateSymbol $noDoNullMove -OrderEntry $orderEntries).Ok $false
    Assert-Equal 'FALSIFY: a hot function missing from one map fails' `
        (Test-OrderedPlacement -BaselineSymbol $orderedBaseline -CandidateSymbol @((New-Symbol $pvs 0x1000)) -OrderEntry $orderEntries).Ok $false
    Assert-Equal 'FALSIFY: empty maps fail, never pass vacuously' `
        (Test-OrderedPlacement -BaselineSymbol @() -CandidateSymbol @() -OrderEntry @()).Ok $false

    $failures = $script:selfTestFailures
    if ($failures -gt 0) {
        Write-Host "$failures self-test case(s) FAILED." -ForegroundColor Red
        return $false
    }
    Write-Host 'Self-test PASSED.' -ForegroundColor Green
    return $true
}

if ($SelfTest) {
    if (Invoke-SelfTest) { exit 0 }
    exit 1
}

function Get-CacheValue {
    param([Parameter(Mandatory)][AllowEmptyString()][string[]]$CacheLine, [Parameter(Mandatory)][string]$Name)
    $row = $CacheLine | Where-Object { $_ -match "^$([regex]::Escape($Name)):[A-Z]+=" } | Select-Object -First 1
    if (-not $row) { return '' }
    return $row.Substring($row.IndexOf('=') + 1)
}

function Get-TreeLink {
    <# The checked facts about one tree: build directory, shipping exe hash, commit and link command. #>
    param([Parameter(Mandatory)][string]$Tree)

    $root = (Resolve-Path -LiteralPath $Tree).Path
    $buildDir = Join-Path $root $script:ShippingBuildDir
    $exe = Join-Path $buildDir 'StratChessEvolved.exe'
    $cache = Join-Path $buildDir 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf) -or -not (Test-Path -LiteralPath $cache -PathType Leaf)) {
        throw "No shipping build in $buildDir. Build the tree first: pwsh -File $root\build.ps1 main"
    }

    $cacheLine = @(Get-Content -LiteralPath $cache)
    $compiler = Get-CacheValue -CacheLine $cacheLine -Name 'CMAKE_CXX_COMPILER'
    $buildType = Get-CacheValue -CacheLine $cacheLine -Name 'CMAKE_BUILD_TYPE'
    if ($compiler -notmatch 'clang-cl' -or $buildType -ne 'Release') {
        throw "$buildDir is not a Release clang-cl build (compiler '$compiler', type '$buildType'). Only the shipping toolchain is measured."
    }
    $ninja = Get-CacheValue -CacheLine $cacheLine -Name 'CMAKE_MAKE_PROGRAM'
    if (-not $ninja -or -not (Test-Path -LiteralPath $ninja -PathType Leaf)) { throw "Ninja from $cache not found: '$ninja'" }

    Push-Location -LiteralPath $buildDir
    try { $ninjaLines = @(& $ninja -t commands StratChessEvolved.exe) }
    finally { Pop-Location }
    if ($LASTEXITCODE -ne 0 -or $ninjaLines.Count -eq 0) { throw "ninja -t commands failed in $buildDir" }

    $commit = (& git -C $root rev-parse HEAD)
    if ($LASTEXITCODE -ne 0) { throw "$root is not a git worktree" }
    $dirty = @(& git -C $root status --porcelain --untracked-files=no).Count -gt 0

    return [pscustomobject]@{
        Root        = $root
        BuildDir    = $buildDir
        ShippingExe = $exe
        ShippingSha = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
        Shipping    = Get-ShippingFingerprint -BuildDir $buildDir
        Commit      = [string]$commit
        Dirty       = $dirty
        LinkCommand = Get-LinkCommand -NinjaLine $ninjaLines[-1]
    }
}

function Get-ShippingFingerprint {
    <# Hashes of the tree's shipping exe, map and PDB, which a run must leave untouched. #>
    param([Parameter(Mandatory)][string]$BuildDir)
    $parts = foreach ($extension in 'exe', 'map', 'pdb') {
        $path = Join-Path $BuildDir "StratChessEvolved.$extension"
        if (Test-Path -LiteralPath $path -PathType Leaf) { "$extension=$((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash)" }
    }
    return ($parts -join ';')
}

function Invoke-Relink {
    <# Runs one relink from the tree's build directory under vcvars64, which supplies LIB. #>
    param(
        [Parameter(Mandatory)][object]$TreeLink,
        [Parameter(Mandatory)][string]$Directory,
        [string]$OrderFile = ''
    )

    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    $batch = Join-Path $Directory 'relink.cmd'
    $log = Join-Path $Directory 'relink.log'
    Set-Content -LiteralPath $batch -Encoding ascii -Value @(
        '@echo off'
        "call `"$script:VcVars`" >nul 2>&1 || exit /b 1"
        "cd /d `"$($TreeLink.BuildDir)`" || exit /b 1"
        (New-RelinkCommand -LinkCommand $TreeLink.LinkCommand -Directory $Directory -OrderFile $OrderFile)
    )
    & cmd.exe /d /c $batch *> $log
    if ($LASTEXITCODE -ne 0) {
        Get-Content -LiteralPath $log -Tail 20 | Out-Host
        throw "Relink failed in $($TreeLink.BuildDir) (exit $LASTEXITCODE); full log: $log"
    }
    return Join-Path $Directory 'StratChessEvolved.exe'
}

function Read-MapSymbol {
    param([Parameter(Mandatory)][string]$Directory)
    $symbols = @(Get-MapCodeSymbol -Line @(Get-Content -LiteralPath (Join-Path $Directory 'StratChessEvolved.map')))
    if ($symbols.Count -eq 0) { throw "No code symbols in $Directory\StratChessEvolved.map" }
    return $symbols
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw "vswhere.exe not found at $vswhere" }
$vsRoot = (& $vswhere -latest -property installationPath) | Select-Object -First 1
$script:VcVars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $script:VcVars)) { throw "vcvars64.bat not found at $script:VcVars" }

if (-not $OutDir) { $OutDir = Join-Path ([System.IO.Path]::GetTempPath()) "ordered-build-pair-$(Get-Date -Format yyyyMMdd-HHmmss)" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$outPath = (Resolve-Path -LiteralPath $OutDir).Path
if (@(Get-ChildItem -LiteralPath $outPath -Force).Count -gt 0) { throw "$outPath is not empty; use a fresh -OutDir." }

$arms = [ordered]@{ baseline = (Get-TreeLink -Tree $BaselineTree); candidate = (Get-TreeLink -Tree $CandidateTree) }

Write-Host '==> Relinking as built' -ForegroundColor Cyan
$placed = @{}
foreach ($arm in $arms.Keys) {
    $tree = $arms[$arm]
    if ($tree.Dirty) { Write-Host "  WARN  $arm tree has uncommitted changes; its commit does not describe it" -ForegroundColor Yellow }
    $plainDir = Join-Path $outPath "$arm\as-built"
    $plainExe = Invoke-Relink -TreeLink $tree -Directory $plainDir
    if ((Get-FileHash -LiteralPath $plainExe -Algorithm SHA256).Hash -ne $tree.ShippingSha) {
        throw "The $arm relink does not reproduce $($tree.ShippingExe). Its objects are out of step with the exe (rebuild with build.ps1 main), or the tree predates /Brepro."
    }
    $placed[$arm] = @(Get-SymbolPlacement -Symbol (Read-MapSymbol -Directory $plainDir))
    Write-Host "  $arm  $($tree.Commit.Substring(0, 12))  byte-identical to the shipping exe" -ForegroundColor DarkGray
}

$orderEntries = @(Get-OrderEntry -BaselinePlaced $placed['baseline'] -CandidatePlaced $placed['candidate'])
if ($orderEntries.Count -eq 0) { throw 'No engine function pairs across the two maps; nothing to pin.' }

$candidateKey = Get-UniqueEngineSymbol -Placed $placed['candidate']
$pairedCount = @((Get-UniqueEngineSymbol -Placed $placed['baseline']).Keys | Where-Object { $candidateKey.ContainsKey($_) }).Count
$resizedHot = @($orderEntries | Where-Object { $_.Resized })
$spacers = @($orderEntries | Where-Object { $_.Spacer })
$pinnedOnlyCount = $orderEntries.Count - $resizedHot.Count - $spacers.Count
$resizedNote = if ($resizedHot.Count -gt 0) { "; resized hot, placed next: $(($resizedHot | ForEach-Object { $_.BaselineName.Split('@')[0..1] -join '@' }) -join ', ')" } else { '' }
Write-Host "==> Relinking with a shared order ($pinnedOnlyCount of $pairedCount paired functions equal in size and pinned$resizedNote)" -ForegroundColor Cyan
foreach ($spacer in $spacers) {
    Write-Host ('  spacer in {0}: {1} ({2} bytes)' -f $spacer.Spacer, $spacer.BaselineName, $spacer.Size) -ForegroundColor DarkGray
}
$ordered = @{}
foreach ($arm in $arms.Keys) {
    $armDir = Join-Path $outPath $arm
    $orderFile = Join-Path $armDir 'order.txt'
    Set-Content -LiteralPath $orderFile -Encoding ascii -Value @(Get-ArmOrder -Entry $orderEntries -Arm $arm)
    Invoke-Relink -TreeLink $arms[$arm] -Directory $armDir -OrderFile $orderFile | Out-Null
    $ordered[$arm] = @(Read-MapSymbol -Directory $armDir)
}

foreach ($arm in $arms.Keys) {
    if ((Get-ShippingFingerprint -BuildDir $arms[$arm].BuildDir) -ne $arms[$arm].Shipping) {
        throw "The $arm shipping exe, map or PDB changed during the run: $($arms[$arm].BuildDir)"
    }
}

$verdict = Test-OrderedPlacement -BaselineSymbol $ordered['baseline'] -CandidateSymbol $ordered['candidate'] -OrderEntry $orderEntries

Write-Host '==> Hot-function placement' -ForegroundColor Cyan
foreach ($hot in $verdict.HotFunction) {
    if ($hot.Ok) {
        Write-Host ('  PASS  {0} at 0x{1:x} in both' -f $hot.Label, $hot.BaselineAddress) -ForegroundColor Green
        continue
    }
    $sizes = foreach ($arm in $arms.Keys) {
        $entry = Find-MapHotSymbol -Symbol $placed[$arm] -MangledPrefix $hot.MangledPrefix
        if ($entry) { "$arm $($entry.Size) bytes" } else { "$arm absent" }
    }
    Write-Host ('  FAIL  {0}: baseline 0x{1:x}, candidate 0x{2:x} (as built: {3})' -f $hot.Label, $hot.BaselineAddress, $hot.CandidateAddress, ($sizes -join ', ')) -ForegroundColor Red
}
if ($verdict.MatchedCount -eq $verdict.PinnedCount) {
    Write-Host "  PASS  $($verdict.MatchedCount) of $($verdict.PinnedCount) pinned functions at identical addresses" -ForegroundColor Green
}
else {
    # A size is the gap to the next function, so a neighbour with another alignment can hide a real difference.
    Write-Host "  FAIL  $($verdict.MatchedCount) of $($verdict.PinnedCount) pinned functions at identical addresses; first moved: $($verdict.FirstMoved)" -ForegroundColor Red
}

$metadata = [ordered]@{
    baseline     = [ordered]@{ tree = $arms['baseline'].Root; commit = $arms['baseline'].Commit; dirty = $arms['baseline'].Dirty }
    candidate    = [ordered]@{ tree = $arms['candidate'].Root; commit = $arms['candidate'].Commit; dirty = $arms['candidate'].Dirty }
    pairedCount  = $pairedCount
    pinnedCount  = $verdict.PinnedCount
    matchedCount = $verdict.MatchedCount
    resizedHot   = @($resizedHot | ForEach-Object { $_.BaselineName })
    spacers      = @($spacers | ForEach-Object { [ordered]@{ name = $_.BaselineName; size = $_.Size; listedIn = $_.Spacer } })
    hotFunction  = @($verdict.HotFunction | ForEach-Object {
            [ordered]@{ label = $_.Label; baselineAddress = $_.BaselineAddress; candidateAddress = $_.CandidateAddress; ok = $_.Ok } })
    ok           = $verdict.Ok
}
$metadata | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $outPath 'metadata.json') -Encoding utf8

if (-not $verdict.Ok) {
    Write-Host ''
    Write-Host 'Placement NOT equalised, so this pair cannot rule placement out. When the change' -ForegroundColor Red
    Write-Host 'resizes several hot functions, a later one shares an address only when cold spacers' -ForegroundColor Red
    Write-Host 'add up to the earlier size change exactly; a moved pinned function means an equal gap' -ForegroundColor Red
    Write-Host 'in the as-built map hid a size difference.' -ForegroundColor Red
    exit 1
}

Write-Host ''
Write-Host 'PASS: ordered pair ready for Compare-Bench.ps1:' -ForegroundColor Green
Write-Host "  -Baseline  $outPath\baseline\StratChessEvolved.exe"
Write-Host "  -Candidate $outPath\candidate\StratChessEvolved.exe"
exit 0
