<#
.SYNOPSIS
    Create a deterministic compilation database with shipping Engine commands.

.DESCRIPTION
    CMake emits one compile command per target/source pair. Engine sources are built
    once for StratChessEvolved and again for StratChessTests, whose command defines
    STRAT_ENABLE_TEST_ACCESS. clang-tidy accepts only one command per source
    reliably, so this script canonicalizes source paths and selects the shipping
    StratChessEvolved command for duplicates. Ambiguous or missing shipping
    candidates fail closed.

.PARAMETER InputPath
    Path to CMake's compile_commands.json.

.PARAMETER OutputDirectory
    Separate directory in which to write the normalized compile_commands.json.

.PARAMETER SelfTest
    Run synthetic normalization tests and exit.

.HOW TO INVOKE
    pwsh -File StratChessEvolved/Scripts/New-TidyCompileDatabase.ps1 `
        -InputPath build/windows-clang-cl/compile_commands.json `
        -OutputDirectory build/windows-clang-cl/tidy-gate
    pwsh -File StratChessEvolved/Scripts/New-TidyCompileDatabase.ps1 -SelfTest
#>

[CmdletBinding(DefaultParameterSetName = 'Run')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Run')]
    [string]$InputPath,

    [Parameter(Mandatory, ParameterSetName = 'Run')]
    [string]$OutputDirectory,

    [Parameter(Mandatory, ParameterSetName = 'SelfTest')]
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Convert-TidyCompileDatabase {
    param(
        [Parameter(Mandatory)][string]$SourcePath,
        [Parameter(Mandatory)][string]$DestinationDirectory
    )

    $inputFullPath = [IO.Path]::GetFullPath($SourcePath)
    if (-not (Test-Path -LiteralPath $inputFullPath -PathType Leaf)) {
        throw "Compilation database does not exist: $inputFullPath"
    }

    $destinationFullPath = [IO.Path]::GetFullPath($DestinationDirectory)
    $outputPath = Join-Path $destinationFullPath 'compile_commands.json'
    if ([string]::Equals($inputFullPath, $outputPath, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The normalized compilation database must be written to a separate directory.'
    }

    $json = Get-Content -Raw -LiteralPath $inputFullPath
    try {
        $trimmed = $json.TrimStart()
        if (-not $trimmed.StartsWith('[')) { throw 'top-level value is not an array' }
        $entries = @(ConvertFrom-Json -InputObject $json -Depth 100 -ErrorAction Stop)
    } catch {
        throw "Compilation database is not valid JSON: $($_.Exception.Message)"
    }

    $groups = [System.Collections.Generic.Dictionary[string, System.Collections.Generic.List[object]]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $entries) {
        $directoryProperty = $entry.PSObject.Properties['directory']
        $fileProperty = $entry.PSObject.Properties['file']
        if (-not $directoryProperty -or -not $fileProperty -or
            [string]::IsNullOrWhiteSpace([string]$directoryProperty.Value) -or
            [string]::IsNullOrWhiteSpace([string]$fileProperty.Value)) {
            throw "Compilation database entry is missing a non-empty 'directory' or 'file' field."
        }

        $directory = [IO.Path]::GetFullPath([string]$directoryProperty.Value)
        $file = [string]$fileProperty.Value
        $canonicalSource = if ([IO.Path]::IsPathRooted($file)) {
            [IO.Path]::GetFullPath($file)
        } else {
            [IO.Path]::GetFullPath((Join-Path $directory $file))
        }

        if (-not $groups.ContainsKey($canonicalSource)) {
            $groups[$canonicalSource] = [System.Collections.Generic.List[object]]::new()
        }
        $groups[$canonicalSource].Add($entry)
    }

    $selected = [System.Collections.Generic.List[object]]::new()
    foreach ($canonicalSource in @($groups.Keys | Sort-Object)) {
        $candidates = @($groups[$canonicalSource])
        if ($candidates.Count -eq 1) {
            $selected.Add($candidates[0])
            continue
        }

        $shipping = @($candidates | Where-Object {
            $parts = [System.Collections.Generic.List[string]]::new()
            foreach ($propertyName in @('command', 'output')) {
                $property = $_.PSObject.Properties[$propertyName]
                if ($property) { $parts.Add([string]$property.Value) }
            }
            $argumentsProperty = $_.PSObject.Properties['arguments']
            if ($argumentsProperty) {
                foreach ($argument in @($argumentsProperty.Value)) { $parts.Add([string]$argument) }
            }
            $commandText = $parts -join ' '
            $commandText -match 'CMakeFiles[\\/]StratChessEvolved\.dir(?:[\\/]|\s|$)' -and
                $commandText -notmatch '(?:^|[\s"''])(?:-D)?STRAT_ENABLE_TEST_ACCESS(?:[=\s"'']|$)'
        })

        if ($shipping.Count -ne 1) {
            throw ("Source '{0}' has {1} compile commands and {2} shipping candidates; expected exactly one shipping command." -f
                $canonicalSource, $candidates.Count, $shipping.Count)
        }
        $selected.Add($shipping[0])
    }

    New-Item -ItemType Directory -Path $destinationFullPath -Force | Out-Null
    ConvertTo-Json -InputObject @($selected) -Depth 100 |
        Set-Content -LiteralPath $outputPath -Encoding utf8NoBOM

    return [pscustomobject]@{
        InputEntries      = $entries.Count
        UniqueSources     = $groups.Count
        SelectedCommands  = $selected.Count
        DuplicateEntries  = $entries.Count - $groups.Count
        OutputPath        = $outputPath
    }
}

function Invoke-SelfTest {
    $root = Join-Path ([System.IO.Path]::GetTempPath()) ("strat-tidy-db-{0}" -f [guid]::NewGuid())
    New-Item -ItemType Directory -Path $root | Out-Null
    $script:SelfTestFailures = 0

    function Assert-Equal {
        param([string]$Name, $Expected, $Actual)
        if ($Expected -eq $Actual) {
            Write-Host "  PASS  $Name" -ForegroundColor Green
        } else {
            Write-Host "  FAIL  $Name (expected '$Expected', got '$Actual')" -ForegroundColor Red
            $script:SelfTestFailures++
        }
    }

    function Assert-Throws {
        param([string]$Name, [scriptblock]$Action, [string]$Pattern)
        try {
            & $Action
            Write-Host "  FAIL  $Name (did not throw)" -ForegroundColor Red
            $script:SelfTestFailures++
        } catch {
            if ($_.Exception.Message -like "*$Pattern*") {
                Write-Host "  PASS  $Name" -ForegroundColor Green
            } else {
                Write-Host "  FAIL  $Name (unexpected error: $($_.Exception.Message))" -ForegroundColor Red
                $script:SelfTestFailures++
            }
        }
    }

    function Write-Database {
        param([string]$Name, [object[]]$Entries)
        $dir = Join-Path $root $Name
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
        $path = Join-Path $dir 'compile_commands.json'
        ConvertTo-Json -InputObject @($Entries) -Depth 20 | Set-Content -LiteralPath $path -Encoding utf8NoBOM
        return $path
    }

    try {
        $build = Join-Path $root 'success/build'
        $entries = @(
            [pscustomobject]@{
                directory = (Join-Path $build 'shipping')
                file      = '../../src/StratEngine/Eval.cpp'
                command   = 'clang++ -DSHIPPING -o CMakeFiles/StratChessEvolved.dir/StratEngine/Eval.cpp.o Eval.cpp'
                output    = 'CMakeFiles/StratChessEvolved.dir/StratEngine/Eval.cpp.o'
            },
            [pscustomobject]@{
                directory = (Join-Path $build 'tests/subdir')
                file      = '../../../src/StratEngine/./Eval.cpp'
                arguments = @('clang++', '-DSTRAT_ENABLE_TEST_ACCESS', '-o', 'CMakeFiles/StratChessTests.dir/StratEngine/Eval.cpp.o')
                output    = 'CMakeFiles/StratChessTests.dir/StratEngine/Eval.cpp.o'
            },
            [pscustomobject]@{
                directory = $build
                file      = '../src/StratChessEvolved/main.cpp'
                arguments = @('clang++', '-c', 'main.cpp')
                output    = 'CMakeFiles/StratChessEvolved.dir/StratChessEvolved/main.cpp.o'
            }
        )
        $successInputPath = Write-Database -Name 'success' -Entries $entries
        $outputDir = Join-Path $root 'success-normalized'
        $stats = Convert-TidyCompileDatabase -SourcePath $successInputPath -DestinationDirectory $outputDir
        $normalized = @(Get-Content -Raw -LiteralPath (Join-Path $outputDir 'compile_commands.json') | ConvertFrom-Json)

        Assert-Equal 'reports every input entry' 3 $stats.InputEntries
        Assert-Equal 'reports canonical unique sources' 2 $stats.UniqueSources
        Assert-Equal 'reports selected commands' 2 $stats.SelectedCommands
        Assert-Equal 'reports duplicate entries' 1 $stats.DuplicateEntries
        Assert-Equal 'writes one command per source' 2 $normalized.Count
        $engineEntry = @($normalized | Where-Object { [IO.Path]::GetFileName($_.file) -eq 'Eval.cpp' })[0]
        Assert-Equal 'selects the shipping engine command' 'CMakeFiles/StratChessEvolved.dir/StratEngine/Eval.cpp.o' $engineEntry.output
        Assert-Equal 'preserves selected object fields' 'clang++ -DSHIPPING -o CMakeFiles/StratChessEvolved.dir/StratEngine/Eval.cpp.o Eval.cpp' $engineEntry.command
        Assert-Equal 'sorts output by canonical source' 'main.cpp,Eval.cpp' (($normalized | ForEach-Object { [IO.Path]::GetFileName($_.file) }) -join ',')

        $ambiguous = @(
            [pscustomobject]@{ directory = $build; file = '../src/StratEngine/Board.cpp'; command = 'clang++ -o CMakeFiles/StratChessEvolved.dir/a.o' },
            [pscustomobject]@{ directory = $build; file = '../src/StratEngine/./Board.cpp'; command = 'clang++ -o CMakeFiles/StratChessEvolved.dir/b.o' }
        )
        $ambiguousPath = Write-Database -Name 'ambiguous' -Entries $ambiguous
        Assert-Throws 'ambiguous shipping candidates fail closed' {
            Convert-TidyCompileDatabase -SourcePath $ambiguousPath -DestinationDirectory (Join-Path $root 'ambiguous-out')
        } 'expected exactly one shipping command'

        $testOnly = @(
            [pscustomobject]@{ directory = $build; file = '../src/StratEngine/Game.cpp'; command = 'clang++ -DSTRAT_ENABLE_TEST_ACCESS -o CMakeFiles/StratChessTests.dir/a.o' },
            [pscustomobject]@{ directory = $build; file = '../src/StratEngine/./Game.cpp'; command = 'clang++ -o CMakeFiles/StratChessTests.dir/b.o' }
        )
        $testOnlyPath = Write-Database -Name 'test-only' -Entries $testOnly
        Assert-Throws 'missing shipping candidate fails closed' {
            Convert-TidyCompileDatabase -SourcePath $testOnlyPath -DestinationDirectory (Join-Path $root 'test-only-out')
        } 'expected exactly one shipping command'

        $malformed = Join-Path $root 'malformed.json'
        Set-Content -LiteralPath $malformed -Value '{not json' -Encoding utf8NoBOM
        Assert-Throws 'malformed JSON fails closed' {
            Convert-TidyCompileDatabase -SourcePath $malformed -DestinationDirectory (Join-Path $root 'malformed-out')
        } 'not valid JSON'

        Assert-Throws 'missing input fails closed' {
            Convert-TidyCompileDatabase -SourcePath (Join-Path $root 'missing.json') -DestinationDirectory (Join-Path $root 'missing-out')
        } 'does not exist'

        Assert-Throws 'input is never overwritten' {
            Convert-TidyCompileDatabase -SourcePath $successInputPath -DestinationDirectory (Split-Path $successInputPath -Parent)
        } 'separate directory'
    } finally {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    }

    Write-Host ''
    if ($script:SelfTestFailures -gt 0) {
        Write-Host "$script:SelfTestFailures self-test case(s) FAILED." -ForegroundColor Red
        return $false
    }
    Write-Host 'All compilation-database self-tests passed.' -ForegroundColor Green
    return $true
}

if ($SelfTest) {
    if (Invoke-SelfTest) { exit 0 }
    exit 1
}

try {
    $result = Convert-TidyCompileDatabase -SourcePath $InputPath -DestinationDirectory $OutputDirectory
    Write-Host ("Normalized compile database: input={0}, unique={1}, selected={2}, duplicates={3}" -f
        $result.InputEntries, $result.UniqueSources, $result.SelectedCommands, $result.DuplicateEntries)
    Write-Host "Wrote $($result.OutputPath)"
} catch {
    Write-Error $_.Exception.Message
    exit 1
}
