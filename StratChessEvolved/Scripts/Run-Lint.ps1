<#
.SYNOPSIS
    Run clang-format and clang-tidy over the files a change touches.

.DESCRIPTION
    The shared local/CI lint entry point. clang-format and the fast clang-tidy Gate
    profile block pull requests. Analyzer-heavy Deep checks block Nightly instead.
    clang-tidy uses a normalized database that selects shipping Engine commands and
    a bounded worker pool, then prints captured results in deterministic source order.

    Neither tool is on PATH on a normal Windows box. They ship inside Visual Studio
    next to clang-cl, so this script resolves them the same way build.ps1 resolves
    the compiler: through vswhere, never a hard-coded VS path.

.WHEN TO USE
    Before pushing anything that touches C++ sources, and whenever CI's lint job
    fails and you want the same answer locally. Validate-PrePR.ps1 calls format and
    the Gate profile automatically.

.HOW TO INVOKE
    pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-Lint.ps1
    ... -Check Format            # formatting only, what CI blocks on
    ... -Check Tidy -Profile Gate # required static analysis
    ... -Check Tidy -Profile Deep # analyzer-heavy Nightly profile
    ... -All                     # whole tree instead of the changed files
    ... -Jobs 1                  # override Gate=4 / Deep=2 worker defaults
    ... -Fix                     # rewrite files in place to satisfy clang-format

.PARAMETER Check
    Format, Tidy, BlameIgnore, or Both (default, meaning all three).

    BlameIgnore is pure git and needs no toolchain: it fails when a commit on this
    branch touches -ReformatThreshold or more sources without appearing in
    .git-blame-ignore-revs. Every clang-format configuration change rewrites most
    of the tree, and forgetting to record that buries the real history of every
    file it touched.

.PARAMETER ReformatThreshold
    Source-file count at which a commit is expected in .git-blame-ignore-revs.
    Default 20.

.PARAMETER AllowUnlistedReformat
    Acknowledge a large commit that genuinely changes code, so BlameIgnore does
    not expect it to be recorded.

.PARAMETER All
    Lint every non-archived source instead of only what changed against -BaseRef.

.PARAMETER BaseRef
    Ref to diff against for the changed-file list. Default 'origin/main'.

.PARAMETER Fix
    Apply clang-format in place. Never applies clang-tidy fixes -- those change
    semantics often enough that they want reviewing individually.

.PARAMETER ClangFormat / ClangTidy
    Explicit paths, overriding discovery. Use when testing a specific toolchain.

.PARAMETER Profile
    Gate (default) or Deep. Gate uses directory config discovery so the test-tree
    override applies. Deep uses the repository's explicit .clang-tidy-deep config
    and excludes test translation units.

.PARAMETER Jobs
    Positive clang-tidy worker count. Defaults to 4 for Gate and 2 for Deep.

.PARAMETER BuildDirectory
    Directory containing CMake's compile_commands.json. Defaults to the selected
    local Windows preset; CI passes this explicitly.

.PARAMETER SelfTest
    Run the bounded clang-tidy worker-pool tests and exit.

.NOTES
    Must be invoked with -File, not dot-sourced. $PSScriptRoot is $null under dot-source.
#>
[CmdletBinding()]
param(
    [ValidateSet('Format', 'Tidy', 'BlameIgnore', 'Both')]
    [string]$Check = 'Both',

    [switch]$All,
    [string]$BaseRef = 'origin/main',
    [switch]$Fix,

    # A commit touching at least this many sources is treated as a reformat.
    [int]$ReformatThreshold = 20,

    # Acknowledge a large commit that genuinely changes code, so it is not
    # expected in .git-blame-ignore-revs.
    [switch]$AllowUnlistedReformat,

    [string]$ClangFormat,
    [string]$ClangTidy,

    [ValidateSet('Gate', 'Deep')]
    [string]$Profile = 'Gate',

    [ValidateRange(1, 256)]
    [int]$Jobs,

    [string]$BuildDirectory,

    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',

    [ValidateSet('clang-cl', 'msvc')]
    [string]$Compiler = 'clang-cl',

    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# The LLVM major the CI lint job pins. Local output must match CI's or the gate
# becomes "clean on my machine": clang-format's output can change between majors,
# and clang-tidy's check inventory certainly does.
$PinnedMajor = 22

$GameDir  = Split-Path $PSScriptRoot -Parent
$RepoRoot = Split-Path $GameDir -Parent

# ---------------------------------------------------------------------------
# Tool resolution: explicit parameter, then PATH, then Visual Studio.
# ---------------------------------------------------------------------------
function Resolve-LlvmTool {
    param(
        [Parameter(Mandatory)][string]$Name,     # 'clang-format' / 'clang-tidy'
        [string]$Explicit
    )

    if ($Explicit) {
        if (-not (Test-Path $Explicit)) { throw "$Name not found at the path given: $Explicit" }
        return $Explicit
    }

    $onPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    # Visual Studio ships LLVM alongside clang-cl. Locate the installation the same
    # way build.ps1 does rather than assuming an edition or a year.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vsRoot = & $vswhere -latest -prerelease -products * -property installationPath 2>$null | Select-Object -First 1
        if ($vsRoot) {
            $candidate = Join-Path $vsRoot "VC\Tools\Llvm\x64\bin\$Name.exe"
            if (Test-Path $candidate) { return $candidate }
        }
    }

    throw @"
$Name could not be located. Looked in:
  1. the -$($Name -replace '-','') parameter (not supplied)
  2. PATH
  3. <VS install>\VC\Tools\Llvm\x64\bin\$Name.exe, via vswhere

Install the 'C++ Clang tools for Windows' component in the Visual Studio Installer,
or pass an explicit path.
"@
}

function Get-LlvmMajorFromVersionText {
    param([Parameter(Mandatory)][string]$VersionText)
    if ($VersionText -match '(?im)\bversion\s+(\d+)\.') { return [int]$Matches[1] }
    return 0
}

function Get-ToolMajor {
    param([Parameter(Mandatory)][string]$Exe)
    $versionText = @(& $Exe --version 2>&1) -join "`n"
    return Get-LlvmMajorFromVersionText -VersionText $versionText
}

function Assert-PinnedMajor {
    param([string]$Exe, [string]$Name)
    $major = Get-ToolMajor -Exe $Exe
    $label = "  {0,-13} {1} (LLVM {2})" -f "${Name}:", $Exe, $major
    if ($major -eq $PinnedMajor) {
        Write-Host $label -ForegroundColor DarkGray
    } else {
        Write-Host $label -ForegroundColor Yellow
        Write-Warning "$Name is LLVM $major but CI pins LLVM $PinnedMajor. Results may not match CI."
    }
}

# ---------------------------------------------------------------------------
# File selection
# ---------------------------------------------------------------------------
function Test-RequiresWholeTreeLint {
    param([Parameter(Mandatory)][string[]]$ChangedFiles)
    foreach ($file in $ChangedFiles) {
        $path = $file.Replace('\', '/')
        if ($path -eq '.clang-format' -or $path -eq '.clang-tidy' -or
            $path -eq '.clang-tidy-deep' -or $path -like '*/.clang-tidy' -or
            $path -like '*/Scripts/Run-Lint.ps1' -or
            $path -like '*/Scripts/New-TidyCompileDatabase.ps1') {
            return $true
        }
    }
    return $false
}

function Get-TargetFiles {
    $all = @(git -C $RepoRoot ls-files '*.cpp' '*.h' 2>$null |
             Where-Object { $_ -and $_ -notmatch '(^|/)Archived/' })

    if ($All) { return $all }

    $committed = @(git -C $RepoRoot diff --name-only "$BaseRef...HEAD" 2>$null | Where-Object { $_ })
    if ($LASTEXITCODE -ne 0) {
        # Fail closed. Linting nothing must never look like linting cleanly -- that is
        # the same green-because-it-checked-nothing failure the CI job guards against.
        throw "Cannot diff against '$BaseRef'. Fetch it, or pass -All to lint the whole tree."
    }

    # Uncommitted work counts: this script is reached precisely when someone has not
    # committed yet. Mirrors Get-ChangeTier.ps1's porcelain handling, renames included.
    $working = @(git -C $RepoRoot status --porcelain 2>$null | Where-Object { $_ } | ForEach-Object {
        $p = $_.Substring(3).Trim().Trim('"')
        if ($p -match '\s->\s') { $p = ($p -split '\s->\s')[-1].Trim().Trim('"') }
        $p
    })

    $changed = @($committed + $working | Where-Object { $_ } | Select-Object -Unique)
    # Changes to the lint machinery affect every TU and must validate themselves.
    if (Test-RequiresWholeTreeLint -ChangedFiles $changed) { return $all }

    # git ls-files omits a developer's new untracked source. Include existing
    # working-tree sources so local validation cannot report green without them.
    $workingSources = @($working | Where-Object {
        $_ -match '\.(cpp|h)$' -and $_ -notmatch '(^|/)Archived/' -and
            (Test-Path -LiteralPath (Join-Path $RepoRoot $_) -PathType Leaf)
    })
    $candidates = @($all + $workingSources | Select-Object -Unique)
    return @($candidates | Where-Object { $changed -contains $_ })
}

# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------
function Invoke-FormatCheck {
    param([string[]]$Files, [string]$Exe)

    if ($Fix) {
        Write-Host "`n==> clang-format (applying to $($Files.Count) file(s))" -ForegroundColor Cyan

        # Iterate to a fixpoint. clang-format is NOT idempotent in one pass: reflowing
        # a long comment can emit continuation lines that the next pass then corrects
        # ('//text' gaining its space). A single pass therefore leaves files that still
        # fail --dry-run -Werror, i.e. still fail CI. Observed on Eval.cpp during the
        # initial reformat, which converged on the second pass.
        $maxPasses = 5
        for ($pass = 1; $pass -le $maxPasses; $pass++) {
            $dirty = @($Files | Where-Object {
                & $Exe --style=file --dry-run -Werror (Join-Path $RepoRoot $_) 2>&1 | Out-Null
                $LASTEXITCODE -ne 0
            })
            if ($dirty.Count -eq 0) {
                Write-Host "Formatting applied; converged after $($pass - 1) pass(es)." -ForegroundColor Green
                return $true
            }
            Write-Host "  pass ${pass}: reformatting $($dirty.Count) file(s)" -ForegroundColor DarkGray
            foreach ($f in $dirty) { & $Exe -i --style=file (Join-Path $RepoRoot $f) }
        }

        Write-Host "FAIL: clang-format did not reach a fixpoint in $maxPasses passes." -ForegroundColor Red
        Write-Host 'That is a clang-format bug or a pathological construct, not your change.' -ForegroundColor Red
        return $false
    }

    Write-Host "`n==> clang-format (checking $($Files.Count) file(s))" -ForegroundColor Cyan
    $bad = @()
    foreach ($f in $Files) {
        & $Exe --style=file --dry-run -Werror (Join-Path $RepoRoot $f) 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { $bad += $f }
    }

    if ($bad.Count -gt 0) {
        Write-Host "FAIL: $($bad.Count) file(s) are not formatted:" -ForegroundColor Red
        $bad | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
        Write-Host "`nFix them with: pwsh -File StratChessEvolved\Scripts\Run-Lint.ps1 -Check Format -Fix" -ForegroundColor Yellow
        return $false
    }
    Write-Host 'PASS: all checked files are correctly formatted.' -ForegroundColor Green
    return $true
}

function Get-DefaultTidyJobs {
    param([Parameter(Mandatory)][ValidateSet('Gate', 'Deep')][string]$ProfileName)
    if ($ProfileName -eq 'Gate') { return 4 }
    return 2
}

function Select-TidySources {
    param(
        [Parameter(Mandatory)][string]$RepoDirectory,
        [Parameter(Mandatory)][string[]]$Files,
        [Parameter(Mandatory)][object[]]$CompilationEntries,
        [Parameter(Mandatory)][ValidateSet('Gate', 'Deep')][string]$ProfileName
    )

    $databaseSources = [System.Collections.Generic.Dictionary[string, string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $CompilationEntries) {
        $directoryProperty = $entry.PSObject.Properties['directory']
        $fileProperty = $entry.PSObject.Properties['file']
        if (-not $directoryProperty -or -not $fileProperty) {
            throw "Normalized compilation database entry is missing 'directory' or 'file'."
        }
        $directory = [IO.Path]::GetFullPath([string]$directoryProperty.Value)
        $file = [string]$fileProperty.Value
        $source = if ([IO.Path]::IsPathRooted($file)) {
            [IO.Path]::GetFullPath($file)
        } else {
            [IO.Path]::GetFullPath((Join-Path $directory $file))
        }
        if ($databaseSources.ContainsKey($source)) {
            throw "Normalized compilation database still contains duplicate source '$source'."
        }
        $databaseSources[$source] = $source
    }

    $requested = @($Files | Where-Object { $_ -like '*.cpp' } | ForEach-Object {
        [pscustomobject]@{
            Relative = $_.Replace('\', '/')
            Absolute = [IO.Path]::GetFullPath((Join-Path $RepoDirectory $_))
        }
    })
    if ($ProfileName -eq 'Deep') {
        $requested = @($requested | Where-Object { $_.Relative -notmatch '(^|/)StratChessTests/' })
    }
    if ($requested.Count -eq 0) { return @() }

    $missing = @($requested | Where-Object { -not $databaseSources.ContainsKey($_.Absolute) })
    if ($missing.Count -gt 0) {
        throw "Requested translation unit(s) missing from normalized compilation database: $($missing.Relative -join ', ')"
    }
    return @($requested.Absolute | Sort-Object -Unique)
}

function Test-TidyWorkerResults {
    param(
        [Parameter(Mandatory)][string[]]$ExpectedSources,
        [Parameter(Mandatory)][object[]]$Results
    )

    $expected = @($ExpectedSources | Sort-Object -Unique)
    $actual = @($Results | ForEach-Object { $_.Source } | Sort-Object)
    $complete = $expected.Count -eq $actual.Count -and
        (($expected -join "`n") -eq ($actual -join "`n"))

    $findingsByCheck = @{}
    foreach ($result in $Results) {
        $diagnostics = "{0}`n{1}" -f $result.StdOut, $result.StdErr
        foreach ($match in [regex]::Matches(
            $diagnostics,
            '(?m):\d+:\d+:\s+(?:warning|error):.*\[(?<check>[A-Za-z0-9_.-]+)(?:,[^\]]+)?\]\s*$')) {
            $check = $match.Groups['check'].Value
            if (-not $findingsByCheck.ContainsKey($check)) { $findingsByCheck[$check] = 0 }
            $findingsByCheck[$check]++
        }
    }

    $findings = @($findingsByCheck.Keys | Sort-Object | ForEach-Object {
        [pscustomobject]@{ Check = $_; Count = $findingsByCheck[$_] }
    })
    $workersClean = @($Results | Where-Object {
        $_.ExitCode -ne 0 -or -not [string]::IsNullOrWhiteSpace([string]$_.WorkerError)
    }).Count -eq 0

    return [pscustomobject]@{
        Succeeded = $complete -and $workersClean -and $findings.Count -eq 0
        Complete  = $complete
        Findings  = $findings
    }
}

function Invoke-TidyWorkers {
    param(
        [Parameter(Mandatory)][string]$Exe,
        [string[]]$ArgumentPrefix = @(),
        [Parameter(Mandatory)][string]$DatabaseDirectory,
        [Parameter(Mandatory)][string[]]$Sources,
        [string]$ConfigFile,
        [Parameter(Mandatory)][ValidateRange(1, 256)][int]$WorkerCount
    )

    $orderedSources = @($Sources | Sort-Object -Unique)
    $work = @(for ($index = 0; $index -lt $orderedSources.Count; $index++) {
        [pscustomobject]@{ Index = $index; Source = $orderedSources[$index] }
    })

    $results = @($work | ForEach-Object -Parallel {
        $item = $_
        $prefixArguments = $using:ArgumentPrefix
        $database = $using:DatabaseDirectory
        $config = $using:ConfigFile
        $executable = $using:Exe
        $stopwatch = [Diagnostics.Stopwatch]::StartNew()

        try {
            $startInfo = [Diagnostics.ProcessStartInfo]::new()
            $startInfo.FileName = $executable
            $startInfo.UseShellExecute = $false
            $startInfo.CreateNoWindow = $true
            $startInfo.RedirectStandardOutput = $true
            $startInfo.RedirectStandardError = $true
            foreach ($argument in @($prefixArguments)) { $startInfo.ArgumentList.Add($argument) }
            $startInfo.ArgumentList.Add('-p')
            $startInfo.ArgumentList.Add($database)
            $startInfo.ArgumentList.Add('--quiet')
            if (-not [string]::IsNullOrWhiteSpace($config)) {
                $startInfo.ArgumentList.Add("--config-file=$config")
            }
            $startInfo.ArgumentList.Add($item.Source)

            $process = [Diagnostics.Process]::new()
            $process.StartInfo = $startInfo
            if (-not $process.Start()) { throw 'Process.Start returned false.' }
            $stdoutTask = $process.StandardOutput.ReadToEndAsync()
            $stderrTask = $process.StandardError.ReadToEndAsync()
            $process.WaitForExit()
            $stdout = $stdoutTask.GetAwaiter().GetResult()
            $stderr = $stderrTask.GetAwaiter().GetResult()
            $exitCode = $process.ExitCode
            $process.Dispose()

            [pscustomobject]@{
                Index       = $item.Index
                Source      = $item.Source
                ExitCode    = $exitCode
                StdOut      = $stdout
                StdErr      = $stderr
                WorkerError = ''
                ElapsedMs   = $stopwatch.ElapsedMilliseconds
            }
        } catch {
            [pscustomobject]@{
                Index       = $item.Index
                Source      = $item.Source
                ExitCode    = -1
                StdOut      = ''
                StdErr      = ''
                WorkerError = $_.Exception.Message
                ElapsedMs   = $stopwatch.ElapsedMilliseconds
            }
        }
    } -ThrottleLimit $WorkerCount)

    $results = @($results | Sort-Object Source)
    $assessment = Test-TidyWorkerResults -ExpectedSources $orderedSources -Results $results
    return [pscustomobject]@{
        Succeeded = $assessment.Succeeded
        Complete  = $assessment.Complete
        Findings  = $assessment.Findings
        Results   = $results
    }
}

function Invoke-TidyCheck {
    param(
        [string[]]$Files,
        [string]$Exe,
        [string]$TidyProfile,
        [int]$WorkerCount,
        [string]$RequestedWorkers
    )

    $buildDir = $BuildDirectory
    if ([string]::IsNullOrWhiteSpace($buildDir)) {
        $preset = "windows-$Compiler"
        if ($Config -eq 'Debug') { $preset += '-debug' }
        $buildDir = Join-Path $RepoRoot "build\$preset"
    }
    $buildDir = [IO.Path]::GetFullPath($buildDir)
    $inputDatabase = Join-Path $buildDir 'compile_commands.json'
    if (-not (Test-Path -LiteralPath $inputDatabase -PathType Leaf)) {
        throw "No compile_commands.json found in '$buildDir'. Configure the build first or pass -BuildDirectory."
    }

    $normalizedDirectory = Join-Path $buildDir ("tidy-{0}" -f $TidyProfile.ToLowerInvariant())
    $normalizer = Join-Path $PSScriptRoot 'New-TidyCompileDatabase.ps1'
    if (-not (Test-Path -LiteralPath $normalizer -PathType Leaf)) {
        throw "Compilation-database normalizer not found: $normalizer"
    }
    $pwsh = (Get-Process -Id $PID).Path
    $normalizerOutput = @(& $pwsh -NoProfile -File $normalizer `
        -InputPath $inputDatabase -OutputDirectory $normalizedDirectory 2>&1)
    $normalizerOutput | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw 'Compilation-database normalization failed.' }

    $normalizedDatabase = Join-Path $normalizedDirectory 'compile_commands.json'
    try {
        $databaseJson = Get-Content -Raw -LiteralPath $normalizedDatabase
        $databaseEntries = @(ConvertFrom-Json -InputObject $databaseJson -Depth 100 -ErrorAction Stop)
    } catch {
        throw "Normalized compilation database is missing or malformed: $($_.Exception.Message)"
    }

    $sources = @(Select-TidySources -RepoDirectory $RepoRoot -Files $Files `
        -CompilationEntries $databaseEntries -ProfileName $TidyProfile)
    if ($sources.Count -eq 0) {
        if ($All) { throw 'Whole-tree clang-tidy selected zero translation units.' }
        Write-Host "`n==> clang-tidy: no .cpp files in the $TidyProfile scope." -ForegroundColor DarkGray
        return $true
    }

    $effectiveWorkers = [Math]::Min($WorkerCount, $sources.Count)
    $configFile = $null
    $configLabel = "$(Join-Path $RepoRoot '.clang-tidy') (directory discovery)"
    if ($TidyProfile -eq 'Deep') {
        $configFile = Join-Path $RepoRoot '.clang-tidy-deep'
        if (-not (Test-Path -LiteralPath $configFile -PathType Leaf)) {
            throw "Deep clang-tidy config not found: $configFile"
        }
        $configLabel = $configFile
    }

    Write-Host "`n==> clang-tidy $TidyProfile (blocking)" -ForegroundColor Cyan
    Write-Host "  Config:              $configLabel" -ForegroundColor DarkGray
    Write-Host "  Normalized commands: $($databaseEntries.Count)" -ForegroundColor DarkGray
    Write-Host "  Selected sources:    $($sources.Count)" -ForegroundColor DarkGray
    Write-Host "  Requested workers:   $RequestedWorkers" -ForegroundColor DarkGray
    Write-Host "  Effective workers:   $effectiveWorkers" -ForegroundColor DarkGray

    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    $run = Invoke-TidyWorkers -Exe $Exe -DatabaseDirectory $normalizedDirectory `
        -Sources $sources -ConfigFile $configFile -WorkerCount $effectiveWorkers
    $stopwatch.Stop()

    foreach ($result in $run.Results) {
        if ($result.ExitCode -eq 0 -and
            [string]::IsNullOrWhiteSpace($result.StdOut) -and
            [string]::IsNullOrWhiteSpace($result.StdErr) -and
            [string]::IsNullOrWhiteSpace($result.WorkerError)) {
            continue
        }
        $relativeSource = [IO.Path]::GetRelativePath($RepoRoot, $result.Source).Replace('\', '/')
        Write-Host "`n--- $relativeSource" -ForegroundColor DarkGray
        if (-not [string]::IsNullOrWhiteSpace($result.StdOut)) {
            Write-Host $result.StdOut.TrimEnd()
        }
        if (-not [string]::IsNullOrWhiteSpace($result.StdErr)) {
            Write-Host $result.StdErr.TrimEnd()
        }
        if (-not [string]::IsNullOrWhiteSpace($result.WorkerError)) {
            Write-Host "worker error: $($result.WorkerError)" -ForegroundColor Red
        }
        if ($result.ExitCode -ne 0) {
            Write-Host "exit code: $($result.ExitCode)" -ForegroundColor Red
        }
    }

    Write-Host "`n  Completed invocations: $($run.Results.Count)/$($sources.Count)" -ForegroundColor DarkGray
    Write-Host ("  Elapsed:               {0:N1}s" -f $stopwatch.Elapsed.TotalSeconds) -ForegroundColor DarkGray
    if ($run.Findings.Count -gt 0) {
        Write-Host '  Findings by check:' -ForegroundColor Red
        foreach ($finding in $run.Findings) {
            Write-Host ("    {0,-48} {1}" -f $finding.Check, $finding.Count) -ForegroundColor Red
        }
    } else {
        Write-Host '  Findings by check:     none' -ForegroundColor DarkGray
    }

    if (-not $run.Succeeded) {
        Write-Host "FAIL: clang-tidy $TidyProfile did not complete cleanly." -ForegroundColor Red
        return $false
    }
    Write-Host "PASS: clang-tidy $TidyProfile completed cleanly." -ForegroundColor Green
    return $true
}

function Invoke-BlameIgnoreCheck {
    <#
        Every clang-format configuration change re-runs the formatter across the
        tree, producing a commit that rewrites most files without altering a line
        of code. Such a commit must be listed in .git-blame-ignore-revs or it
        buries the real history of every file it touches -- and it is easy to
        forget, because nothing else in the pipeline notices.

        The rule is deliberately a COUNT, not an attempt to prove a commit is
        formatting-only. Proving that needs the .clang-format as it stood at that
        commit, which stops being available the moment the config changes again --
        exactly the case this exists to catch. A count cannot be fooled that way,
        and in this repository a commit touching 20+ sources is a reformat or a
        mass rename; both want acknowledging. -AllowUnlistedReformat is the escape
        hatch for the rare genuine one.
    #>
    Write-Host "`n==> .git-blame-ignore-revs coverage" -ForegroundColor Cyan

    $ignoreFile = Join-Path $RepoRoot '.git-blame-ignore-revs'
    $listed = @()
    if (Test-Path $ignoreFile) {
        $listed = @(Get-Content $ignoreFile |
            ForEach-Object { $_.Trim() } |
            Where-Object { $_ -and -not $_.StartsWith('#') })
    }

    $commits = @(git -C $RepoRoot rev-list --no-merges "$BaseRef..HEAD" 2>$null | Where-Object { $_ })
    if ($LASTEXITCODE -ne 0) {
        # Fail closed, exactly as the file selection does: an unresolvable base
        # must not read as "nothing to check".
        throw "Cannot list commits against '$BaseRef'. Fetch it, or pass -BaseRef."
    }
    if ($commits.Count -eq 0) {
        Write-Host 'No commits on this branch; nothing to check.' -ForegroundColor Green
        return $true
    }

    $unlisted = @()
    foreach ($c in $commits) {
        $touched = @(git -C $RepoRoot show --name-only --format= $c 2>$null |
            Where-Object { $_ -match '\.(cpp|h)$' -and $_ -notmatch '(^|/)Archived/' })
        if ($touched.Count -lt $ReformatThreshold) { continue }
        if ($listed -contains $c) {
            Write-Host ("  listed    {0}  {1} source(s)" -f $c.Substring(0, 9), $touched.Count) -ForegroundColor DarkGray
            continue
        }
        $unlisted += [PSCustomObject]@{
            Sha     = $c
            Count   = $touched.Count
            Subject = (git -C $RepoRoot log -1 --format=%s $c)
        }
    }

    if ($unlisted.Count -eq 0) {
        Write-Host 'PASS: every large-footprint commit is accounted for.' -ForegroundColor Green
        return $true
    }

    if ($AllowUnlistedReformat) {
        foreach ($u in $unlisted) {
            Write-Host ("  ALLOWED   {0}  {1} source(s)  {2}" -f $u.Sha.Substring(0, 9), $u.Count, $u.Subject) -ForegroundColor Yellow
        }
        Write-Host 'PASS: unlisted commits acknowledged via -AllowUnlistedReformat.' -ForegroundColor Yellow
        return $true
    }

    Write-Host "FAIL: $($unlisted.Count) commit(s) touch $ReformatThreshold+ sources and are not in .git-blame-ignore-revs:" -ForegroundColor Red
    foreach ($u in $unlisted) {
        Write-Host ("  {0}  {1,3} source(s)  {2}" -f $u.Sha.Substring(0, 9), $u.Count, $u.Subject) -ForegroundColor Red
    }
    Write-Host ''
    Write-Host 'If these are reformats, append to .git-blame-ignore-revs:' -ForegroundColor Yellow
    foreach ($u in $unlisted) {
        Write-Host ''
        Write-Host ("    # {0}" -f $u.Subject) -ForegroundColor Yellow
        Write-Host ("    {0}" -f $u.Sha) -ForegroundColor Yellow
    }
    Write-Host ''
    Write-Host 'If they genuinely change code, re-run with -AllowUnlistedReformat.' -ForegroundColor Yellow
    return $false
}

function Invoke-TidyWorkerSelfTest {
    $root = Join-Path ([IO.Path]::GetTempPath()) ("strat-tidy-workers-{0}" -f [guid]::NewGuid())
    New-Item -ItemType Directory -Path $root | Out-Null
    $script:WorkerSelfTestFailures = 0

    function Assert-Equal {
        param([string]$Name, $Expected, $Actual)
        if ($Expected -eq $Actual) {
            Write-Host "  PASS  $Name" -ForegroundColor Green
        } else {
            Write-Host "  FAIL  $Name (expected '$Expected', got '$Actual')" -ForegroundColor Red
            $script:WorkerSelfTestFailures++
        }
    }

    function Assert-True {
        param([string]$Name, [bool]$Value)
        Assert-Equal -Name $Name -Expected $true -Actual $Value
    }

    function Assert-Throws {
        param([string]$Name, [scriptblock]$Action)
        try {
            & $Action
            Write-Host "  FAIL  $Name (did not throw)" -ForegroundColor Red
            $script:WorkerSelfTestFailures++
        } catch {
            Write-Host "  PASS  $Name" -ForegroundColor Green
        }
    }

    $fakeTidy = Join-Path $root 'fake-tidy.ps1'
    @'
param(
    [Alias('p')][string]$DatabaseDirectory,
    [switch]$quiet,
    [Parameter(Position = 0)][string]$source
)
$name = [IO.Path]::GetFileNameWithoutExtension($source)
$markerRoot = $env:STRAT_TIDY_TEST_MARKERS
$active = Join-Path $markerRoot ("{0}-{1}.active" -f $name, $PID)
Set-Content -LiteralPath $active -Value $name
$activeCount = @(Get-ChildItem -LiteralPath $markerRoot -Filter '*.active').Count
Set-Content -LiteralPath (Join-Path $markerRoot "$name.count") -Value $activeCount
if ($name -like '*slow*') { Start-Sleep -Milliseconds 800 }
elseif ($name -like '*mid*') { Start-Sleep -Milliseconds 650 }
else { Start-Sleep -Milliseconds 500 }
Write-Output "stdout:$name"
[Console]::Error.WriteLine("stderr:$name")
if ($name -like '*finding*') {
    Write-Output "${source}:1:1: warning: synthetic finding [bugprone-synthetic]"
}
Remove-Item -LiteralPath $active -Force
New-Item -ItemType File -Path (Join-Path $markerRoot ("{0}-{1}.invocation" -f $name, [guid]::NewGuid())) | Out-Null
if ($name -like '*fail*') { exit 3 }
exit 0
'@ | Set-Content -LiteralPath $fakeTidy -Encoding utf8NoBOM

    $markerRoot = Join-Path $root 'markers'
    New-Item -ItemType Directory -Path $markerRoot | Out-Null
    $previousMarkerRoot = $env:STRAT_TIDY_TEST_MARKERS
    $env:STRAT_TIDY_TEST_MARKERS = $markerRoot
    $pwsh = (Get-Process -Id $PID).Path
    $prefix = @('-NoProfile', '-File', $fakeTidy)

    try {
        Assert-True 'root tidy config expands lint to whole tree' `
            (Test-RequiresWholeTreeLint -ChangedFiles @('.clang-tidy'))
        Assert-True 'test tidy config expands lint to whole tree' `
            (Test-RequiresWholeTreeLint -ChangedFiles @('StratChessTests/.clang-tidy'))
        Assert-True 'lint runner expands lint to whole tree' `
            (Test-RequiresWholeTreeLint -ChangedFiles @('StratChessEvolved/Scripts/Run-Lint.ps1'))
        Assert-Equal 'ordinary source keeps changed-file scope' $false `
            (Test-RequiresWholeTreeLint -ChangedFiles @('StratEngine/Eval.cpp'))

        Assert-Equal 'parses Visual Studio multiline LLVM version' 22 `
            (Get-LlvmMajorFromVersionText -VersionText "LLVM (http://llvm.org/):`n  LLVM version 22.1.3")
        Assert-Equal 'parses single-line clang version' 22 `
            (Get-LlvmMajorFromVersionText -VersionText 'clang-format version 22.1.3')

        $repoFixture = Join-Path $root 'repo'
        $engineSource = Join-Path $repoFixture 'StratEngine/A.cpp'
        $testSource = Join-Path $repoFixture 'StratChessTests/T.cpp'
        $databaseEntries = @(
            [pscustomobject]@{ directory = $root; file = $engineSource; command = 'clang++ A.cpp' },
            [pscustomobject]@{ directory = $root; file = $testSource; command = 'clang++ T.cpp' }
        )
        $gateSources = Select-TidySources -RepoDirectory $repoFixture `
            -Files @('StratChessTests/T.cpp', 'StratEngine/A.h', 'StratEngine/A.cpp') `
            -CompilationEntries $databaseEntries -ProfileName Gate
        Assert-Equal 'Gate selects Engine and test translation units' 'T.cpp,A.cpp' `
            (($gateSources | ForEach-Object { [IO.Path]::GetFileName($_) }) -join ',')
        $deepSources = Select-TidySources -RepoDirectory $repoFixture `
            -Files @('StratChessTests/T.cpp', 'StratEngine/A.cpp') `
            -CompilationEntries $databaseEntries -ProfileName Deep
        Assert-Equal 'Deep excludes test translation units' 'A.cpp' `
            (($deepSources | ForEach-Object { [IO.Path]::GetFileName($_) }) -join ',')
        Assert-Equal 'header-only changed scope is a valid zero-TU selection' 0 `
            @(Select-TidySources -RepoDirectory $repoFixture -Files @('StratEngine/A.h') `
                -CompilationEntries $databaseEntries -ProfileName Gate).Count
        Assert-Throws 'requested translation unit missing from database fails closed' {
            Select-TidySources -RepoDirectory $repoFixture -Files @('StratEngine/Missing.cpp') `
                -CompilationEntries $databaseEntries -ProfileName Gate
        }

        Assert-Equal 'Gate default workers' 4 (Get-DefaultTidyJobs -ProfileName Gate)
        Assert-Equal 'Deep default workers' 2 (Get-DefaultTidyJobs -ProfileName Deep)
        Assert-Throws 'zero workers rejected' {
            Invoke-TidyWorkers -Exe $pwsh -ArgumentPrefix $prefix -DatabaseDirectory $root `
                -Sources @('zero.cpp') -WorkerCount 0
        }

        $sources = @(
            (Join-Path $root 'c-slow.cpp'),
            (Join-Path $root 'a-fast.cpp'),
            (Join-Path $root 'b-mid.cpp')
        )
        $parallel = Invoke-TidyWorkers -Exe $pwsh -ArgumentPrefix $prefix `
            -DatabaseDirectory $root -Sources $sources -WorkerCount 2
        Assert-Equal 'parallel run returns every result' 3 $parallel.Results.Count
        Assert-Equal 'results are deterministic by source' 'a-fast.cpp,b-mid.cpp,c-slow.cpp' `
            (($parallel.Results | ForEach-Object { [IO.Path]::GetFileName($_.Source) }) -join ',')
        Assert-Equal 'every source invoked exactly once' 3 @(Get-ChildItem -LiteralPath $markerRoot -Filter '*.invocation').Count
        Assert-True 'stdout remains source-specific' (@($parallel.Results | Where-Object {
            $_.StdOut -notmatch ("stdout:{0}" -f [IO.Path]::GetFileNameWithoutExtension($_.Source))
        }).Count -eq 0)
        Assert-True 'stderr remains source-specific' (@($parallel.Results | Where-Object {
            $_.StdErr -notmatch ("stderr:{0}" -f [IO.Path]::GetFileNameWithoutExtension($_.Source))
        }).Count -eq 0)
        $maxParallel = (Get-ChildItem -LiteralPath $markerRoot -Filter '*.count' |
            Get-Content | Measure-Object -Maximum).Maximum
        Assert-True 'parallelism is bounded by worker count' ($maxParallel -le 2)
        Assert-True 'parallel override actually overlaps work' ($maxParallel -ge 2)
        Assert-True 'clean worker aggregate passes' $parallel.Succeeded

        Get-ChildItem -LiteralPath $markerRoot | Remove-Item -Force
        $serial = Invoke-TidyWorkers -Exe $pwsh -ArgumentPrefix $prefix `
            -DatabaseDirectory $root -Sources $sources -WorkerCount 1
        $maxSerial = (Get-ChildItem -LiteralPath $markerRoot -Filter '*.count' |
            Get-Content | Measure-Object -Maximum).Maximum
        Assert-Equal 'Jobs 1 stays serial' 1 $maxSerial
        Assert-True 'serial aggregate passes' $serial.Succeeded

        Get-ChildItem -LiteralPath $markerRoot | Remove-Item -Force
        $failed = Invoke-TidyWorkers -Exe $pwsh -ArgumentPrefix $prefix `
            -DatabaseDirectory $root -Sources @((Join-Path $root 'worker-fail.cpp')) -WorkerCount 1
        Assert-Equal 'worker exit code is retained' 3 $failed.Results[0].ExitCode
        Assert-Equal 'worker failure fails aggregate' $false $failed.Succeeded

        Get-ChildItem -LiteralPath $markerRoot | Remove-Item -Force
        $finding = Invoke-TidyWorkers -Exe $pwsh -ArgumentPrefix $prefix `
            -DatabaseDirectory $root -Sources @((Join-Path $root 'worker-finding.cpp')) -WorkerCount 1
        Assert-Equal 'finding check is grouped' 'bugprone-synthetic' $finding.Findings[0].Check
        Assert-Equal 'finding fails aggregate even with exit zero' $false $finding.Succeeded

        $missing = Test-TidyWorkerResults -ExpectedSources @('a.cpp', 'b.cpp') -Results @(
            [pscustomobject]@{ Source = 'a.cpp'; ExitCode = 0; StdOut = ''; StdErr = ''; WorkerError = '' }
        )
        Assert-Equal 'missing worker result fails aggregate' $false $missing.Succeeded
    } finally {
        if ($null -eq $previousMarkerRoot) {
            Remove-Item Env:STRAT_TIDY_TEST_MARKERS -ErrorAction SilentlyContinue
        } else {
            $env:STRAT_TIDY_TEST_MARKERS = $previousMarkerRoot
        }
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    }

    Write-Host ''
    if ($script:WorkerSelfTestFailures -gt 0) {
        Write-Host "$script:WorkerSelfTestFailures self-test case(s) FAILED." -ForegroundColor Red
        return $false
    }
    Write-Host 'All clang-tidy worker self-tests passed.' -ForegroundColor Green
    return $true
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
if ($SelfTest) {
    if (Invoke-TidyWorkerSelfTest) { exit 0 }
    exit 1
}

$blameOk = $true
if ($Check -in @('BlameIgnore', 'Both')) { $blameOk = Invoke-BlameIgnoreCheck }

# Pure git; needs no toolchain and no file list, so it can stand alone.
if ($Check -eq 'BlameIgnore') {
    Write-Host ''
    if (-not $blameOk) { Write-Host 'Lint FAILED (blame-ignore coverage).' -ForegroundColor Red; exit 1 }
    Write-Host 'Lint PASSED.' -ForegroundColor Green
    exit 0
}

Write-Host "`n==> Toolchain" -ForegroundColor Cyan
$fmtExe = $null; $tidyExe = $null
if ($Check -in @('Format', 'Both')) {
    $fmtExe = Resolve-LlvmTool -Name 'clang-format' -Explicit $ClangFormat
    Assert-PinnedMajor -Exe $fmtExe -Name 'clang-format'
}
if ($Check -in @('Tidy', 'Both')) {
    $tidyExe = Resolve-LlvmTool -Name 'clang-tidy' -Explicit $ClangTidy
    Assert-PinnedMajor -Exe $tidyExe -Name 'clang-tidy'
}

$files = Get-TargetFiles
if ($files.Count -eq 0) {
    Write-Host "`nNo C++ sources in scope; nothing to lint." -ForegroundColor Green
    if (-not $blameOk) { Write-Host 'Lint FAILED (blame-ignore coverage).' -ForegroundColor Red; exit 1 }
    exit 0
}

$formatOk = $true
$tidyOk = $true
if ($Check -in @('Format', 'Both')) { $formatOk = Invoke-FormatCheck -Files $files -Exe $fmtExe }
if ($Check -in @('Tidy', 'Both')) {
    $defaultWorkers = Get-DefaultTidyJobs -ProfileName $Profile
    $workerCount = if ($PSBoundParameters.ContainsKey('Jobs')) { $Jobs } else { $defaultWorkers }
    $requestedWorkers = if ($PSBoundParameters.ContainsKey('Jobs')) { [string]$Jobs } else { "default ($defaultWorkers)" }
    $tidyOk = Invoke-TidyCheck -Files $files -Exe $tidyExe -TidyProfile $Profile `
        -WorkerCount $workerCount -RequestedWorkers $requestedWorkers
}

Write-Host ''
if (-not $formatOk) { Write-Host 'Lint FAILED (formatting).' -ForegroundColor Red; exit 1 }
if (-not $tidyOk)   { Write-Host "Lint FAILED (clang-tidy $Profile)." -ForegroundColor Red; exit 1 }
if (-not $blameOk)  { Write-Host 'Lint FAILED (blame-ignore coverage).' -ForegroundColor Red; exit 1 }
Write-Host 'Lint PASSED.' -ForegroundColor Green
exit 0
