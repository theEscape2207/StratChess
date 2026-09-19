<#
.SYNOPSIS
    Fail when the shipping engine's hot functions are not on a 64-byte boundary.

.DESCRIPTION
    `-falign-functions=64` starts every function on a cache line, which is the smaller
    half of the nps swing that makes two node-identical builds read differently (#555,
    #578). It has two silent ways to stop working:

      the flag    -- clang-cl accepts GNU-spelled flags and drops them before the
                     frontend sees them (#84), so a spelling that stops being
                     translated is not a build error.
      the codegen -- it works only because it lands in the IR as `align 64` per
                     definition, which link-time codegen honours. The shipping target
                     links with ThinLTO, so that codegen runs inside lld-link, and a
                     toolchain that stopped honouring it would also be silent.

    Both leave a green build, a correct engine, and layout variance quietly back to
    where #555/#556 found it -- at which point the next person re-derives #555 from
    scratch. A prose note cannot fail, so this does.

    The assertion is over the linker map of the shipping executable, which
    CMakeLists.txt emits on every link. Two parts:

      hot functions -- AIPerplex::pvs and AIPerplex::quiescence at address %64 == 0.
                       This is the crisp half: a build with the flag dropped fails it
                       immediately (measured at %64 = 16 and 48 without the flag).
      share         -- the proportion of code symbols at >=64-byte alignment, reported
                       and warned on, never failed on. It moves with the code: 92.7%
                       with the flag against 22.9% without, so it is a corroborating
                       number rather than a threshold worth defending.

    Reading a map rather than relinking with /MAP on demand is deliberate. A map
    produced by a differently-configured build proves nothing about the shipping one,
    and reconstructing it costs a reconfigure plus a relink where a file read costs
    milliseconds. The executable is byte-identical with /MAP and without.

.PARAMETER MapPath
    Linker map to read. Defaults to the shipping build's StratChessEvolved.map,
    resolved through Get-BuildArtifact.ps1 so it follows the same preset and
    configuration defaults as every other consumer of the build tree.

.PARAMETER Config
    Build configuration whose map to read. Defaults to Release -- the configuration
    that ships and the only one whose layout is ever measured.

.PARAMETER SelfTest
    Run synthetic parser and detector tests and exit. Pure text: no build, no
    toolchain, no map on disk.

.HOW TO INVOKE
    pwsh -File Scripts/Test-CodeAlignment.ps1
    pwsh -File Scripts/Test-CodeAlignment.ps1 -SelfTest
#>

[CmdletBinding(DefaultParameterSetName = 'Run')]
param(
    [Parameter(ParameterSetName = 'Run')]
    [string]$MapPath,

    [Parameter(ParameterSetName = 'Run')]
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',

    [Parameter(Mandatory, ParameterSetName = 'SelfTest')]
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# The two functions the whole exercise is about. Matched on the mangled name's
# prefix, never with -like: '?' is a single-character wildcard in a PowerShell
# wildcard pattern, so '?pvs@AIPerplex@@*' would also match names that merely begin
# with some character followed by 'pvs@'. The exception-handling funclets the
# compiler emits -- '?dtor$254@?0??pvs@AIPerplex@@...' -- contain the hot function's
# mangled name as a substring and are not it; a StartsWith test excludes them, and
# the self-test below asserts that it does.
$script:HotFunction = @(
    [pscustomobject]@{ Label = 'AIPerplex::pvs';        MangledPrefix = '?pvs@AIPerplex@@' }
    [pscustomobject]@{ Label = 'AIPerplex::quiescence'; MangledPrefix = '?quiescence@AIPerplex@@' }
)

# Share below which the run warns. Not a failure threshold: the share moves with the
# code. Measured 92.7% with the flag, 22.9% without, so anything under this is far
# closer to "flag gone" than to ordinary drift.
$script:ShareWarnBelow = 0.60

# A publics-by-value row: ' 0001:00009700  ?pvs@... 000000014000a700  <obj>'. Segment
# 0001 is .text in this link; the third field is the loaded address, which is what
# alignment is a property of.
$script:SymbolRowPattern = '^\s*(?<seg>[0-9a-fA-F]{4}):(?<off>[0-9a-fA-F]{8})\s+(?<name>\S+)\s+(?<addr>[0-9a-fA-F]{16})\s'

function Get-MapCodeSymbol {
    <#
      .SYNOPSIS
        One record per code symbol in the map: mangled Name and loaded Address.
    #>
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Line
    )

    $symbols = [System.Collections.Generic.List[object]]::new()
    foreach ($text in $Line) {
        $match = [regex]::Match($text, $script:SymbolRowPattern)
        if (-not $match.Success) { continue }
        if ($match.Groups['seg'].Value -ne '0001') { continue }

        $symbols.Add([pscustomobject]@{
            Name    = $match.Groups['name'].Value
            Address = [System.Convert]::ToUInt64($match.Groups['addr'].Value, 16)
        })
    }

    return $symbols
}

function Test-MapAlignment {
    <#
      .SYNOPSIS
        Verdict over a parsed map: hot-function results, the aligned share, and
        whether the crisp half passed.
    #>
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Line
    )

    $symbols = @(Get-MapCodeSymbol -Line $Line)

    $hotResults = foreach ($hot in $script:HotFunction) {
        # StartsWith, not -like: see the note on $script:HotFunction.
        $found = @($symbols | Where-Object { $_.Name.StartsWith($hot.MangledPrefix, [System.StringComparison]::Ordinal) })
        if ($found.Count -eq 0) {
            [pscustomobject]@{ Label = $hot.Label; Found = $false; Modulo = $null; Ok = $false }
        }
        else {
            $modulo = [int]($found[0].Address % 64)
            [pscustomobject]@{ Label = $hot.Label; Found = $true; Modulo = $modulo; Ok = ($modulo -eq 0) }
        }
    }

    $hot64 = @($hotResults)
    $alignedCount = @($symbols | Where-Object { ($_.Address % 64) -eq 0 }).Count
    $share = if ($symbols.Count -gt 0) { $alignedCount / $symbols.Count } else { 0.0 }

    # An empty map is a failure, not a vacuous pass: it is what a renamed artifact, a
    # truncated file or a parser that stopped matching all look like.
    $ok = ($symbols.Count -gt 0) -and -not ($hot64 | Where-Object { -not $_.Ok })

    return [pscustomobject]@{
        SymbolCount  = $symbols.Count
        AlignedCount = $alignedCount
        Share        = $share
        HotFunction  = $hot64
        Ok           = [bool]$ok
    }
}

function Invoke-SelfTest {
    # A minimal map with the shape the real one has: a header, the section table, and
    # publics-by-value rows. Addresses are chosen so the expected verdict is obvious.
    function New-FixtureMap {
        param(
            [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$SymbolRow
        )

        $header = @(
            ' StratChessEvolved'
            ''
            ' Timestamp is 00000000 (Repro mode)'
            ''
            ' Preferred load address is 0000000140000000'
            ''
            ' Start         Length     Name                   Class'
            ' 0001:00000000 000c9cdfH .text                   CODE'
            ''
            '  Address         Publics by Value              Rva+Base               Lib:Object'
            ''
        )
        return @($header + $SymbolRow)
    }

    function New-SymbolRow {
        param(
            [Parameter(Mandatory)][string]$Name,
            [Parameter(Mandatory)][uint64]$Address
        )

        $offset = $Address - 0x140000000
        return (' 0001:{0:x8}       {1} {2:x16}     StratChessEvolved.exe.lto.AIPerplex.cpp.obj' -f $offset, $Name, $Address)
    }

    $pvs        = '?pvs@AIPerplex@@AEAAHAEAUThreadData@@HHHH_NAEAVTranspositionTable@@@Z'
    $quiescence = '?quiescence@AIPerplex@@AEAAHAEAUThreadData@@HHHHAEAVTranspositionTable@@@Z'
    $pvsFunclet = '?dtor$254@?0??pvs@AIPerplex@@AEAAHAEAUThreadData@@HHHH_NAEAVTranspositionTable@@@Z@4HA'

    $script:selfTestFailures = 0
    Write-Host '==> Self-test' -ForegroundColor Cyan

    function Assert-Case {
        param(
            [Parameter(Mandatory)][string]$Name,
            [Parameter(Mandatory)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Line,
            [Parameter(Mandatory)][bool]$ExpectOk,
            [int]$ExpectSymbolCount = -1
        )

        $verdict = Test-MapAlignment -Line $Line
        $problems = @()
        if ($verdict.Ok -ne $ExpectOk) {
            $problems += "expected Ok=$ExpectOk, got Ok=$($verdict.Ok)"
        }
        if ($ExpectSymbolCount -ge 0 -and $verdict.SymbolCount -ne $ExpectSymbolCount) {
            $problems += "expected $ExpectSymbolCount symbol(s), got $($verdict.SymbolCount)"
        }

        if ($problems.Count -eq 0) {
            Write-Host "  PASS  $Name" -ForegroundColor Green
        }
        else {
            Write-Host "  FAIL  $Name ($($problems -join '; '))" -ForegroundColor Red
            $script:selfTestFailures++
        }
    }

    Assert-Case -Name 'both hot functions on a 64-byte boundary passes' -ExpectOk $true -ExpectSymbolCount 2 -Line (
        New-FixtureMap -SymbolRow @(
            (New-SymbolRow -Name $pvs -Address 0x14000a700)
            (New-SymbolRow -Name $quiescence -Address 0x14000c200)
        ))

    # The situation the check exists for: the flag is gone and the hot functions land
    # wherever the linker put them. These are the offsets #578 measured without it.
    Assert-Case -Name 'FALSIFY: pvs at %64 = 16 fails' -ExpectOk $false -Line (
        New-FixtureMap -SymbolRow @(
            (New-SymbolRow -Name $pvs -Address 0x14000a710)
            (New-SymbolRow -Name $quiescence -Address 0x14000c200)
        ))

    Assert-Case -Name 'FALSIFY: quiescence at %64 = 48 fails' -ExpectOk $false -Line (
        New-FixtureMap -SymbolRow @(
            (New-SymbolRow -Name $pvs -Address 0x14000a700)
            (New-SymbolRow -Name $quiescence -Address 0x14000c230)
        ))

    Assert-Case -Name 'FALSIFY: a missing hot function fails, never passes vacuously' -ExpectOk $false -Line (
        New-FixtureMap -SymbolRow @((New-SymbolRow -Name $pvs -Address 0x14000a700)))

    # The substring trap: an exception funclet carries the hot function's mangled name
    # inside its own. Matching loosely would accept this map as containing an aligned
    # pvs when the real one is absent.
    Assert-Case -Name 'FALSIFY: a dtor funclet does not stand in for pvs' -ExpectOk $false -Line (
        New-FixtureMap -SymbolRow @(
            (New-SymbolRow -Name $pvsFunclet -Address 0x14000ae40)
            (New-SymbolRow -Name $quiescence -Address 0x14000c200)
        ))

    Assert-Case -Name 'FALSIFY: an empty map fails' -ExpectOk $false -ExpectSymbolCount 0 -Line (New-FixtureMap -SymbolRow @())

    Assert-Case -Name 'FALSIFY: a map with no publics section fails' -ExpectOk $false -ExpectSymbolCount 0 -Line @(
        ' StratChessEvolved'
        ' Timestamp is 00000000 (Repro mode)'
    )

    # Data symbols live in other segments and must not dilute the code share.
    $mixed = New-FixtureMap -SymbolRow @(
        (New-SymbolRow -Name $pvs -Address 0x14000a700)
        (New-SymbolRow -Name $quiescence -Address 0x14000c200)
        (New-SymbolRow -Name '?helper@@YAXXZ' -Address 0x14000c244)
        ' 0002:00000010       ?g_data@@3HA           0000000140100010     StratChessEvolved.exe.obj'
    )
    Assert-Case -Name 'symbols outside segment 0001 are not counted' -ExpectOk $true -ExpectSymbolCount 3 -Line $mixed

    $shareVerdict = Test-MapAlignment -Line $mixed
    if ([math]::Abs($shareVerdict.Share - (2.0 / 3.0)) -lt 0.0001) {
        Write-Host '  PASS  aligned share counts every code symbol, not just the hot ones' -ForegroundColor Green
    }
    else {
        Write-Host "  FAIL  aligned share: expected 0.667, got $($shareVerdict.Share)" -ForegroundColor Red
        $script:selfTestFailures++
    }

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

if (-not $MapPath) {
    # Through Get-BuildArtifact.ps1 so the preset and configuration defaults are the
    # ones every other consumer of the build tree uses. Dot-invoked, not via
    # `pwsh -File`: a process boundary would serialise the path it returns.
    $artifactScript = Join-Path $PSScriptRoot 'Get-BuildArtifact.ps1'
    $exePath = & $artifactScript -Target StratChessEvolved -Config $Config
    $MapPath = [System.IO.Path]::ChangeExtension([string]$exePath, '.map')
}

if (-not (Test-Path -LiteralPath $MapPath -PathType Leaf)) {
    Write-Host "FAIL: no linker map at $MapPath" -ForegroundColor Red
    Write-Host 'CMakeLists.txt emits it on every link of the engine target, so a missing map' -ForegroundColor Yellow
    Write-Host 'means the tree was never built with the current CMakeLists.txt. Rebuild with:' -ForegroundColor Yellow
    Write-Host '  .\build.ps1 main' -ForegroundColor Yellow
    exit 1
}

$verdict = Test-MapAlignment -Line @(Get-Content -LiteralPath $MapPath)

Write-Host "==> Hot-code alignment ($([System.IO.Path]::GetFileName($MapPath)))" -ForegroundColor Cyan

foreach ($hot in $verdict.HotFunction) {
    if (-not $hot.Found) {
        Write-Host "  FAIL  $($hot.Label) -- not in the map" -ForegroundColor Red
        continue
    }
    if ($hot.Ok) {
        Write-Host "  PASS  $($hot.Label) at %64 = 0" -ForegroundColor Green
    }
    else {
        Write-Host "  FAIL  $($hot.Label) at %64 = $($hot.Modulo)" -ForegroundColor Red
    }
}

# Invariant culture: this line is read side by side in local and CI logs, and a
# decimal comma on one of them makes two identical results look different.
$sharePercent = ($verdict.Share * 100).ToString('N1', [cultureinfo]::InvariantCulture) + '%'
Write-Host "  $($verdict.AlignedCount) of $($verdict.SymbolCount) code symbols >=64-byte aligned ($sharePercent)" -ForegroundColor DarkGray

if (-not $verdict.Ok) {
    Write-Host ''
    Write-Host 'Hot-code alignment is NOT in effect in the shipping image.' -ForegroundColor Red
    Write-Host 'Check that /clang:-falign-functions=64 is still on the clang-cl branch of' -ForegroundColor Yellow
    Write-Host 'strat_configure_target, and that the toolchain still honours `align 64` through' -ForegroundColor Yellow
    Write-Host 'link-time codegen. Both failures are silent; this check is the tripwire (#513).' -ForegroundColor Yellow
    exit 1
}

if ($verdict.Share -lt $script:ShareWarnBelow) {
    Write-Host ''
    Write-Host "WARN: the aligned share is $sharePercent, well under the ~92.7% a build with the" -ForegroundColor Yellow
    Write-Host 'flag measures. Both hot functions are aligned, so this is not a failure -- but a' -ForegroundColor Yellow
    Write-Host 'dropped flag with two lucky addresses looks exactly like this.' -ForegroundColor Yellow
}

Write-Host "PASS: hot-code alignment in effect ($sharePercent of code symbols aligned)." -ForegroundColor Green
exit 0
