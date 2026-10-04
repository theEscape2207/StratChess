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
         follows an identical prefix, so it still starts at the same address in both. Every
         other function goes after these.
      5. relinks with /order and checks the result. AIPerplex::pvs and AIPerplex::quiescence
         must start at identical addresses in both, or the script fails. That happens when the
         change resized both: only the first can follow the identical prefix.

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

function Get-PinnedFunction {
    <#
      .SYNOPSIS
        The functions both order files list: paired by key, equal in size, in baseline address
        order. Then the hot functions whose size differs, marked Resized: the first of them
        follows an identical prefix, so it still starts at the same address in both images.
        Wrap the call site in @().
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
        [pscustomobject]@{ Key = $entry.Key; BaselineName = $entry.Name; CandidateName = $other.Name; Resized = $false }
    }
    $pinnedKey = @{}
    foreach ($entry in $pinned) { $pinnedKey[$entry.Key] = $true }

    $resized = foreach ($hot in $script:MapHotFunction) {
        $inBaseline = Find-MapHotSymbol -Symbol @($baselineByKey.Values) -MangledPrefix $hot.MangledPrefix
        $inCandidate = Find-MapHotSymbol -Symbol @($candidateByKey.Values) -MangledPrefix $hot.MangledPrefix
        if ($null -eq $inBaseline -or $null -eq $inCandidate -or $pinnedKey.ContainsKey($inBaseline.Key)) { continue }
        [pscustomobject]@{ Key = $inBaseline.Key; BaselineName = $inBaseline.Name; CandidateName = $inCandidate.Name; Resized = $true }
    }
    return @(@($pinned) + @($resized))
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
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Pinned
    )

    $hotResults = foreach ($hot in $script:MapHotFunction) {
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

    $equalSize = @($Pinned | Where-Object { -not $_.Resized })
    $matched = @($equalSize | Where-Object {
            $baselineAddress.ContainsKey($_.BaselineName) -and $candidateAddress.ContainsKey($_.CandidateName) -and
            $baselineAddress[$_.BaselineName] -eq $candidateAddress[$_.CandidateName]
        }).Count

    # Empty maps are a failure, never a vacuous pass: the hot lookups above fail on them too.
    $ok = ($BaselineSymbol.Count -gt 0) -and ($CandidateSymbol.Count -gt 0) -and -not ($hotList | Where-Object { -not $_.Ok })
    return [pscustomobject]@{
        HotFunction  = $hotList
        PinnedCount  = $equalSize.Count
        MatchedCount = $matched
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
    $pinned = @(Get-PinnedFunction -BaselinePlaced @(Get-SymbolPlacement -Symbol $baseline) -CandidatePlaced @(Get-SymbolPlacement -Symbol $candidate))
    # pvs 0x400 both; changed 0x40 vs 0xc0; anon 0x40 both; qs 0x80 vs 0x100. pvs and anon pin; qs trails.
    Assert-Equal 'pins equal-size pairs in baseline order, then resized hot functions; drops the rest' `
        (($pinned | ForEach-Object { "$($_.BaselineName):$($_.Resized)" }) -join ',') "${pvs}:False,?anon@?A0x11111111@@YAXXZ:False,${qs}:True"
    Assert-Equal "each order file lists the tree's own names" `
        (($pinned | ForEach-Object { $_.CandidateName }) -join ',') "$pvs,?anon@?A0x22222222@@YAXXZ,$qs"

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

    $orderedBaseline = @((New-Symbol $pvs 0x1000), (New-Symbol $qs 0x1400), (New-Symbol '?anon@?A0x11111111@@YAXXZ' 0x1500))
    $orderedCandidate = @((New-Symbol $pvs 0x1000), (New-Symbol $qs 0x1400), (New-Symbol '?anon@?A0x22222222@@YAXXZ' 0x1500))
    $verdict = Test-OrderedPlacement -BaselineSymbol $orderedBaseline -CandidateSymbol $orderedCandidate -Pinned $pinned
    Assert-Equal 'identical hot addresses pass, and pinned matches count across own names' "$($verdict.Ok) $($verdict.MatchedCount)/$($verdict.PinnedCount)" 'True 2/2'

    $shifted = @((New-Symbol $pvs 0x1000), (New-Symbol $qs 0x1440), (New-Symbol '?anon@?A0x22222222@@YAXXZ' 0x1500))
    Assert-Equal 'FALSIFY: quiescence at a different address fails' `
        (Test-OrderedPlacement -BaselineSymbol $orderedBaseline -CandidateSymbol $shifted -Pinned $pinned).Ok $false
    Assert-Equal 'FALSIFY: a hot function missing from one map fails' `
        (Test-OrderedPlacement -BaselineSymbol $orderedBaseline -CandidateSymbol @((New-Symbol $pvs 0x1000)) -Pinned $pinned).Ok $false
    Assert-Equal 'FALSIFY: empty maps fail, never pass vacuously' `
        (Test-OrderedPlacement -BaselineSymbol @() -CandidateSymbol @() -Pinned @()).Ok $false

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
        Commit      = [string]$commit
        Dirty       = $dirty
        LinkCommand = Get-LinkCommand -NinjaLine $ninjaLines[-1]
    }
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

$pinned = @(Get-PinnedFunction -BaselinePlaced $placed['baseline'] -CandidatePlaced $placed['candidate'])
if ($pinned.Count -eq 0) { throw 'No engine function pairs across the two maps; nothing to pin.' }

$resizedHot = @($pinned | Where-Object { $_.Resized })
$resizedNote = if ($resizedHot.Count -gt 0) { "; resized hot, placed next: $(($resizedHot | ForEach-Object { $_.BaselineName.Split('@')[0..1] -join '@' }) -join ', ')" } else { '' }
Write-Host "==> Relinking with a shared order ($($pinned.Count - $resizedHot.Count) equal-size functions pinned$resizedNote)" -ForegroundColor Cyan
$ordered = @{}
foreach ($arm in $arms.Keys) {
    $armDir = Join-Path $outPath $arm
    $orderFile = Join-Path $armDir 'order.txt'
    $nameField = if ($arm -eq 'baseline') { 'BaselineName' } else { 'CandidateName' }
    Set-Content -LiteralPath $orderFile -Encoding ascii -Value @($pinned | ForEach-Object { $_.$nameField })
    Invoke-Relink -TreeLink $arms[$arm] -Directory $armDir -OrderFile $orderFile | Out-Null
    $ordered[$arm] = @(Read-MapSymbol -Directory $armDir)
}

foreach ($arm in $arms.Keys) {
    if ((Get-FileHash -LiteralPath $arms[$arm].ShippingExe -Algorithm SHA256).Hash -ne $arms[$arm].ShippingSha) {
        throw "The $arm shipping exe changed during the run: $($arms[$arm].ShippingExe)"
    }
}

$verdict = Test-OrderedPlacement -BaselineSymbol $ordered['baseline'] -CandidateSymbol $ordered['candidate'] -Pinned $pinned

Write-Host '==> Hot-function placement' -ForegroundColor Cyan
foreach ($hot in $verdict.HotFunction) {
    if ($hot.Ok) {
        Write-Host ('  PASS  {0} at 0x{1:x} in both' -f $hot.Label, $hot.BaselineAddress) -ForegroundColor Green
        continue
    }
    $sizes = foreach ($arm in $arms.Keys) {
        $entry = $placed[$arm] | Where-Object { $_.Name.StartsWith($hot.MangledPrefix, [System.StringComparison]::Ordinal) } | Select-Object -First 1
        if ($entry) { "$arm $($entry.Size) bytes" } else { "$arm absent" }
    }
    Write-Host ('  FAIL  {0}: baseline 0x{1:x}, candidate 0x{2:x} (as built: {3})' -f $hot.Label, $hot.BaselineAddress, $hot.CandidateAddress, ($sizes -join ', ')) -ForegroundColor Red
}
Write-Host "  $($verdict.MatchedCount) of $($verdict.PinnedCount) pinned functions at identical addresses" -ForegroundColor DarkGray

$metadata = [ordered]@{
    baseline     = [ordered]@{ tree = $arms['baseline'].Root; commit = $arms['baseline'].Commit; dirty = $arms['baseline'].Dirty }
    candidate    = [ordered]@{ tree = $arms['candidate'].Root; commit = $arms['candidate'].Commit; dirty = $arms['candidate'].Dirty }
    pinnedCount  = $verdict.PinnedCount
    matchedCount = $verdict.MatchedCount
    hotFunction  = @($verdict.HotFunction | ForEach-Object {
            [ordered]@{ label = $_.Label; baselineAddress = $_.BaselineAddress; candidateAddress = $_.CandidateAddress; ok = $_.Ok } })
    ok           = $verdict.Ok
}
$metadata | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $outPath 'metadata.json') -Encoding utf8

if (-not $verdict.Ok) {
    Write-Host ''
    Write-Host 'Placement NOT equalised. When the change resizes several hot functions, only the' -ForegroundColor Red
    Write-Host 'first can start at a shared address, so this pair cannot rule placement out.' -ForegroundColor Red
    exit 1
}

Write-Host ''
Write-Host 'PASS: ordered pair ready for Compare-Bench.ps1:' -ForegroundColor Green
Write-Host "  -Baseline  $outPath\baseline\StratChessEvolved.exe"
Write-Host "  -Candidate $outPath\candidate\StratChessEvolved.exe"
exit 0
