<#
.SYNOPSIS
    Check that an engine exe runs standalone: it imports only OS-shipped DLLs, answers UCI
    from an otherwise empty directory, and leaves nothing behind there.

.DESCRIPTION
    Three checks, one PASS/FAIL line each:

      1. Imports. `llvm-objdump -p` on the exe, located through vswhere under
         VC\Tools\Llvm\x64\bin (the ARM64 copy does not run on an x64 host). Every
         `DLL Name` must match $AllowedImports. An unlisted import fails, so a VC++
         Redistributable DLL (MSVCP*, VCRUNTIME*) fails too. Zero parsed imports also fails: it means the parse broke.
      2. Empty-directory UCI smoke. The exe is copied alone into a fresh temporary
         directory and started there with no arguments -- UCI mode, as a GUI launches it.
         UciDriver.ps1 drives uci, isready, position startpos, go depth -Depth, quit and
         requires exit 0. With -ExpectedId, the `id name` line must equal it exactly.
      3. Filesystem. After exit the directory holds only the exe. That shows the engine
         wrote no output file. It is not a filesystem trace: it does not prove the
         absence of every read or transient write.

    The allowlist is what the hybrid-CRT Release build imports: KERNEL32.dll, one OS API set,
    and the api-ms-win-crt-* UCRT API sets. The C++ runtime and vcruntime are linked
    statically, so any MSVCP* or VCRUNTIME* import fails. Add an entry only with its reason.

.PARAMETER Exe
    The engine to check. Required unless -SelfTest.

.PARAMETER ExpectedId
    The exact text after `id name `, e.g. 'StratChess 0.8.0'. Omit to skip that comparison.

.PARAMETER Depth
    Search depth for the smoke. Default 8.

.PARAMETER SelfTest
    Run the cases against fixture objdump output and FakeUciEngine.ps1, then exit.

.OUTPUTS
    Exit code 0 when every check passes, 1 otherwise.

.HOW TO INVOKE
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\Test-StandaloneExe.ps1 -Exe C:\...\StratChessEvolved.exe
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\Test-StandaloneExe.ps1 -SelfTest
#>
[CmdletBinding()]
param(
    [string]$Exe,
    [string]$ExpectedId,
    [ValidateRange(1, 64)][int]$Depth = 8,
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'UciDriver.ps1')

# Wildcard patterns, case-insensitive: the PE import table's casing is the linker's choice.
$AllowedImports = @(
    # The Win32 base API. Ships with every Windows; nothing links without it.
    'KERNEL32.dll'
    # WaitOnAddress / WakeByAddressAll, for the static STL's std::atomic::wait. A Windows API
    # set resolved by the OS loader since Windows 8, not a redistributable.
    'api-ms-win-core-synch-l1-2-0.dll'
    # The Universal CRT's API sets. Part of Windows since Windows 10, not a redistributable.
    'api-ms-win-crt-*.dll'
)

function Get-DllImport {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$ObjdumpText)

    return @($ObjdumpText -split "`r?`n" |
        Where-Object { $_ -match '^\s*DLL Name:\s*(\S+)\s*$' } |
        ForEach-Object { $Matches[1] })
}

function Test-ImportAllowlist {
    <# Returns the failure reason, or '' when every import is allowed. #>
    param([Parameter(Mandatory)][AllowEmptyString()][string]$ObjdumpText)

    $imports = @(Get-DllImport -ObjdumpText $ObjdumpText)
    if ($imports.Count -eq 0) { return 'no DLL imports parsed; the objdump output is not what this check reads' }
    $unlisted = @($imports | Where-Object { $dll = $_; -not ($AllowedImports | Where-Object { $dll -like $_ }) })
    if ($unlisted.Count -gt 0) { return "unlisted import(s): $($unlisted -join ', ')" }
    return ''
}

function Get-LlvmObjdump {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw "vswhere.exe not found at $vswhere. Is Visual Studio installed?" }
    $vsRoot = (& $vswhere -latest -property installationPath) | Select-Object -First 1
    if (-not $vsRoot) { throw 'No Visual Studio installation found via vswhere.' }
    $tool = Join-Path $vsRoot 'VC\Tools\Llvm\x64\bin\llvm-objdump.exe'
    if (-not (Test-Path -LiteralPath $tool)) { throw "llvm-objdump not found at $tool. Is the VS Clang component installed?" }
    return $tool
}

function Invoke-StandaloneSmoke {
    <#
        Copies $ExePath alone into a fresh directory, drives one search there with no
        launch arguments, and returns Smoke and Files failure reasons ('' when passed).
    #>
    param(
        [Parameter(Mandatory)][string]$ExePath,
        [AllowEmptyString()][string]$Id = '',
        [Parameter(Mandatory)][int]$SearchDepth,
        [int]$TimeoutMs = 120000
    )

    $dir = Join-Path ([System.IO.Path]::GetTempPath()) "standalone-$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Path $dir | Out-Null
    try {
        $leaf = Split-Path -Leaf $ExePath
        $copy = Join-Path $dir $leaf
        Copy-Item -LiteralPath $ExePath -Destination $copy

        $smoke = ''
        $smokeDetail = ''
        try {
            $transcript = Invoke-UciFixedDepthSearch -ExePath $copy -WorkDir $dir -Position 'startpos' `
                -SearchDepth $SearchDepth -Threads 1 -Description "standalone $leaf" `
                -TimeoutMs $TimeoutMs -Arguments ''
            $lines = @($transcript -split "`r?`n")
            $missing = @('uciok', 'readyok' | Where-Object { $lines -notcontains $_ })
            if ($missing.Count -gt 0) {
                $smoke = "no $($missing -join ', ') line"
            } elseif ($Id) {
                $idLines = @($lines | Where-Object { $_ -like 'id name *' })
                if ($idLines.Count -ne 1 -or $idLines[0] -cne "id name $Id") {
                    $smoke = "expected 'id name $Id', got '$($idLines -join ' | ')'"
                }
            }
        } catch {
            # The driver's message carries the engine's stderr and transcript after its first line.
            $smoke = ($_.Exception.Message -split "`r?`n")[0]
            $smokeDetail = $_.Exception.Message
        }

        # Relative names, so an 8.3 short TEMP path (CI's RUNNER~1) cannot mismatch the long form
        # Get-ChildItem reports.
        $stray = @(Get-ChildItem -LiteralPath $dir -Recurse -Force -Name | Where-Object { $_ -ne $leaf })
        $files = if ($stray.Count -gt 0) { "left behind: $($stray -join ', ')" } else { '' }

        return [pscustomobject]@{ Smoke = $smoke; SmokeDetail = $smokeDetail; Files = $files }
    } finally {
        Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Write-Check {
    param([string]$Name, [AllowEmptyString()][string]$Failure)
    if ($Failure) {
        Write-Host "FAIL: $Name -- $Failure" -ForegroundColor Red
    } else {
        Write-Host "PASS: $Name" -ForegroundColor Green
    }
}

# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------
if ($SelfTest) {
    $failed = 0

    $header = "StratChessEvolved.exe:`tfile format coff-x86-64`n`nThe Import Tables:`n"
    $importCases = @(
        @{ Name = 'KERNEL32 alone passes';             Dlls = @('KERNEL32.dll');                        Expect = '' }
        @{ Name = 'casing does not matter';            Dlls = @('kernel32.dll');                        Expect = '' }
        @{ Name = 'the synch API set passes';          Dlls = @('KERNEL32.dll', 'API-MS-WIN-CORE-SYNCH-L1-2-0.dll'); Expect = '' }
        @{ Name = 'FALSIFY: C++ runtime DLL fails';     Dlls = @('KERNEL32.dll', 'MSVCP140.dll');        Expect = 'MSVCP140\.dll' }
        @{ Name = 'FALSIFY: VCRUNTIME fails';           Dlls = @('VCRUNTIME140.dll', 'KERNEL32.dll');    Expect = 'VCRUNTIME140\.dll' }
        @{ Name = 'UCRT API sets pass';                Dlls = @('KERNEL32.dll', 'api-ms-win-crt-heap-l1-1-0.dll', 'API-MS-WIN-CRT-STDIO-L1-1-0.dll'); Expect = '' }
        @{ Name = 'FALSIFY: an unlisted API set fails'; Dlls = @('KERNEL32.dll', 'api-ms-win-core-file-l1-1-0.dll'); Expect = 'api-ms-win-core-file' }
        @{ Name = 'FALSIFY: nothing parsed fails';      Dlls = @();                                      Expect = 'no DLL imports parsed' }
    )
    foreach ($case in $importCases) {
        $text = $header + (($case.Dlls | ForEach-Object { "  lookup 00000000 time 00000000 fwd 00000000 name 00000000 addr 00000000`r`n`r`n    DLL Name: $_`r`n    vma:  Hint/Ord Name`r`n" }) -join '')
        $got = Test-ImportAllowlist -ObjdumpText $text
        $ok = if ($case.Expect) { $got -match $case.Expect } else { $got -eq '' }
        if (-not $ok) { $failed++ }
        Write-Check "imports: $($case.Name)" $(if ($ok) { '' } else { "got '$got'" })
    }

    # The smoke runs FakeUciEngine.ps1 behind a generated shim, since FakeUciEngine.cmd
    # finds the engine beside itself and the smoke copies the exe alone. A shim may first
    # write a stray file, which the filesystem check must catch.
    $fake = Join-Path $PSScriptRoot 'FakeUciEngine.ps1'
    $shimDir = Join-Path ([System.IO.Path]::GetTempPath()) "standalone-shims-$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Path $shimDir | Out-Null
    $smokeCases = @(
        @{ Name = 'well-behaved engine passes';       Mode = 'ok';           Id = 'FakeUciEngine'; Stray = $false; Smoke = '';                 Files = '' }
        @{ Name = 'id is not checked when omitted';   Mode = 'ok';           Id = '';              Stray = $false; Smoke = '';                 Files = '' }
        @{ Name = 'FALSIFY: wrong id fails';          Mode = 'ok';           Id = 'StratChess 0.8.0'; Stray = $false; Smoke = "expected 'id name StratChess 0\.8\.0'"; Files = '' }
        @{ Name = 'FALSIFY: nonzero exit fails';      Mode = 'nonzero-exit'; Id = '';              Stray = $false; Smoke = 'exited with code 3'; Files = '' }
        @{ Name = 'FALSIFY: a written file fails';    Mode = 'ok';           Id = '';              Stray = $true;  Smoke = '';                 Files = 'left behind: stray\.log' }
    )
    try {
        $index = 0
        foreach ($case in $smokeCases) {
            $index++
            $shim = Join-Path $shimDir "engine$index.cmd"
            $strayLine = if ($case.Stray) { "echo x> stray.log`r`n" } else { '' }
            Set-Content -LiteralPath $shim -Encoding ascii -Value (
                "@echo off`r`n$strayLine" +
                "pwsh -NoProfile -ExecutionPolicy Bypass -File `"$fake`"`r`n" +
                "exit /b %ERRORLEVEL%")

            $env:STRAT_FAKE_UCI_MODE = $case.Mode
            $env:STRAT_FAKE_UCI_LIFETIME_MS = 20000
            try {
                $r = Invoke-StandaloneSmoke -ExePath $shim -Id $case.Id -SearchDepth 2 -TimeoutMs 15000
            } finally {
                $env:STRAT_FAKE_UCI_MODE = $null
                $env:STRAT_FAKE_UCI_LIFETIME_MS = $null
            }
            $smokeOk = if ($case.Smoke) { $r.Smoke -match $case.Smoke } else { $r.Smoke -eq '' }
            $filesOk = if ($case.Files) { $r.Files -match $case.Files } else { $r.Files -eq '' }
            $ok = $smokeOk -and $filesOk
            if (-not $ok) { $failed++ }
            Write-Check "smoke: $($case.Name)" $(if ($ok) { '' } else { "smoke='$($r.Smoke)' files='$($r.Files)'" })
        }
    } finally {
        Remove-Item -LiteralPath $shimDir -Recurse -Force -ErrorAction SilentlyContinue
    }

    Write-Host ''
    if ($failed -gt 0) {
        Write-Host "$failed self-test case(s) FAILED." -ForegroundColor Red
        exit 1
    }
    Write-Host 'All self-test cases passed.' -ForegroundColor Green
    exit 0
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
if (-not $Exe) {
    Write-Host 'Test-StandaloneExe.ps1: -Exe is required (or -SelfTest).' -ForegroundColor Yellow
    exit 1
}
if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    Write-Host "FAIL: exe not found: $Exe" -ForegroundColor Red
    exit 1
}
$exePath = (Resolve-Path -LiteralPath $Exe).Path

$objdump = Get-LlvmObjdump
$objdumpText = (& $objdump -p $exePath) -join "`n"
if ($LASTEXITCODE -ne 0) { throw "llvm-objdump -p failed with exit code $LASTEXITCODE on $exePath" }
$importFailure = Test-ImportAllowlist -ObjdumpText $objdumpText
Write-Check "imports ($((Get-DllImport -ObjdumpText $objdumpText) -join ', '))" $importFailure

$result = Invoke-StandaloneSmoke -ExePath $exePath -Id $ExpectedId -SearchDepth $Depth
Write-Check "empty-directory UCI smoke (depth $Depth$(if ($ExpectedId) { ", id '$ExpectedId'" }))" $result.Smoke
if ($result.SmokeDetail) { Write-Host $result.SmokeDetail }
Write-Check 'no file left beside the exe' $result.Files

if ($importFailure -or $result.Smoke -or $result.Files) { exit 1 }
exit 0
