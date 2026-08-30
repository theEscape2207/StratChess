<#
.SYNOPSIS
    Fail when a GitHub Actions job carries no `timeout-minutes`.

.DESCRIPTION
    A job without `timeout-minutes` inherits GitHub's 6-hour default. That ceiling is
    never reached by work -- the whole PR matrix finishes in minutes -- so in practice
    it is only ever reached by a hang, and a hung job burns six hours of runner time
    while the required check sits pending. Every job must therefore state a bound.

    Jobs that call a reusable workflow (`uses:` at job level) are exempt, because
    GitHub does not accept `timeout-minutes` on them.

    Parsing is indentation-based rather than a YAML library, so the check has no
    dependency to install in the job that runs it. That is sound because YAML requires
    a block scalar's body to be indented deeper than the key introducing it: in a
    workflow every `run: |` sits at indent 8 or more, so no line of script can be
    mistaken for a job name at indent 2. Anything this script cannot parse with
    confidence -- a missing `jobs:` key, a file with no jobs -- is an error, not a pass.

.PARAMETER WorkflowDirectory
    Directory of workflow files to check. Defaults to the repository's
    .github/workflows, resolved from this script's own location.

.PARAMETER SelfTest
    Run synthetic parser tests and exit. Verifies that the detector actually detects,
    which a green run over already-compliant files does not.

.HOW TO INVOKE
    pwsh -File Scripts/Test-WorkflowTimeouts.ps1
    pwsh -File Scripts/Test-WorkflowTimeouts.ps1 -SelfTest
#>

[CmdletBinding(DefaultParameterSetName = 'Run')]
param(
    [Parameter(ParameterSetName = 'Run')]
    [string]$WorkflowDirectory,

    [Parameter(Mandatory, ParameterSetName = 'SelfTest')]
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# A job header: exactly two spaces, a name, a colon, nothing else.
$script:JobPattern = '^  ([A-Za-z0-9_-]+):\s*$'
# Job-level keys sit at four spaces. A step-level timeout is indented eight and
# must NOT satisfy the job -- it bounds one step, not the job around it.
$script:TimeoutPattern = '^    timeout-minutes:\s*\d+\s*$'
$script:ReusablePattern = '^    uses:\s*\S'
$script:TopLevelPattern = '^[A-Za-z0-9_-]+:'

function Get-JobWithoutTimeout {
    <#
      .SYNOPSIS
        Names of jobs in one workflow that state no timeout. Throws if the file
        cannot be parsed with confidence.
    #>
    param(
        # AllowEmptyString because a workflow's blank separator lines arrive as
        # empty elements, which the binder rejects by default.
        [Parameter(Mandatory)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Line,
        [Parameter(Mandatory)][string]$Label
    )

    $jobsIndex = -1
    for ($i = 0; $i -lt $Line.Count; $i++) {
        if ($Line[$i] -eq 'jobs:') {
            if ($jobsIndex -ge 0) { throw "${Label}: more than one top-level 'jobs:' key." }
            $jobsIndex = $i
        }
    }
    if ($jobsIndex -lt 0) { throw "${Label}: no top-level 'jobs:' key." }

    $offenders = [System.Collections.Generic.List[string]]::new()
    $current = $null
    $hasTimeout = $false
    $isReusable = $false
    $jobCount = 0

    function Complete-Job {
        if ($null -ne $current -and -not $hasTimeout -and -not $isReusable) {
            $offenders.Add($current)
        }
    }

    for ($i = $jobsIndex + 1; $i -lt $Line.Count; $i++) {
        $text = $Line[$i]

        # A second top-level key would end the jobs mapping. None exists today;
        # stopping here keeps that from silently becoming a blind spot.
        if ($text -match $script:TopLevelPattern) { break }

        $match = [regex]::Match($text, $script:JobPattern)
        if ($match.Success) {
            Complete-Job
            $current = $match.Groups[1].Value
            $hasTimeout = $false
            $isReusable = $false
            $jobCount++
            continue
        }

        if ($null -eq $current) { continue }
        if ($text -match $script:TimeoutPattern) { $hasTimeout = $true }
        if ($text -match $script:ReusablePattern) { $isReusable = $true }
    }
    Complete-Job

    if ($jobCount -eq 0) { throw "${Label}: 'jobs:' key contains no jobs." }

    return $offenders
}

function Invoke-SelfTest {
    function Assert-Case {
        param(
            [Parameter(Mandatory)][string]$Name,
            [Parameter(Mandatory)][string]$Content,
            [string[]]$ExpectedOffender,
            [switch]$ExpectThrow
        )

        $lines = $Content -split "`r?`n"
        try {
            $actual = @(Get-JobWithoutTimeout -Line $lines -Label 'self-test')
        }
        catch {
            if ($ExpectThrow) {
                Write-Host "  PASS  $Name (rejected: $($_.Exception.Message))" -ForegroundColor Green
            }
            else {
                Write-Host "  FAIL  $Name (unexpected error: $($_.Exception.Message))" -ForegroundColor Red
                $script:selfTestFailures++
            }
            return
        }

        if ($ExpectThrow) {
            Write-Host "  FAIL  $Name (expected an error, got [$($actual -join ', ')])" -ForegroundColor Red
            $script:selfTestFailures++
            return
        }

        $expected = @($ExpectedOffender)
        if (($actual -join '|') -eq ($expected -join '|')) {
            Write-Host "  PASS  $Name" -ForegroundColor Green
        }
        else {
            Write-Host "  FAIL  $Name (expected [$($expected -join ', ')], got [$($actual -join ', ')])" -ForegroundColor Red
            $script:selfTestFailures++
        }
    }

    $script:selfTestFailures = 0
    Write-Host "==> Self-test" -ForegroundColor Cyan

    Assert-Case -Name 'bounded job passes' -Content @'
jobs:
  build:
    runs-on: ubuntu-24.04
    timeout-minutes: 20
    steps:
      - run: echo hi
'@ -ExpectedOffender @()

    Assert-Case -Name 'unbounded job is caught' -Content @'
jobs:
  build:
    runs-on: ubuntu-24.04
    steps:
      - run: echo hi
'@ -ExpectedOffender @('build')

    Assert-Case -Name 'only the unbounded job of two is caught' -Content @'
jobs:
  first:
    runs-on: ubuntu-24.04
    timeout-minutes: 5
    steps:
      - run: echo hi
  second:
    runs-on: ubuntu-24.04
    steps:
      - run: echo hi
'@ -ExpectedOffender @('second')

    Assert-Case -Name 'step-level timeout does not bound the job' -Content @'
jobs:
  build:
    runs-on: ubuntu-24.04
    steps:
      - name: slow thing
        timeout-minutes: 10
        run: echo hi
'@ -ExpectedOffender @('build')

    Assert-Case -Name 'reusable-workflow job is exempt' -Content @'
jobs:
  call:
    uses: ./.github/workflows/other.yml
'@ -ExpectedOffender @()

    # The hazard the indent-2 anchor exists to avoid: script text inside a block
    # scalar must never be read as a job name.
    Assert-Case -Name 'script text is not mistaken for a job' -Content @'
jobs:
  build:
    runs-on: ubuntu-24.04
    timeout-minutes: 20
    steps:
      - run: |
          cat <<'EOF'
          not-a-job:
            still-not-a-job:
          EOF
'@ -ExpectedOffender @()

    # Blank separator lines arrive as empty array elements, which the binder
    # rejects unless the parameter allows them.
    Assert-Case -Name 'blank lines between jobs are tolerated' -Content @'
jobs:

  first:
    runs-on: ubuntu-24.04
    timeout-minutes: 5

    steps:
      - run: echo hi

  second:
    runs-on: ubuntu-24.04

    steps:
      - run: echo hi
'@ -ExpectedOffender @('second')

    Assert-Case -Name 'keys above jobs: are ignored' -Content @'
on:
  pull_request:
    branches: [main]
concurrency:
  group: x
jobs:
  build:
    runs-on: ubuntu-24.04
    timeout-minutes: 20
    steps:
      - run: echo hi
'@ -ExpectedOffender @()

    Assert-Case -Name 'missing jobs: key is an error' -Content @'
on:
  pull_request:
    branches: [main]
'@ -ExpectThrow

    Assert-Case -Name 'empty jobs: mapping is an error' -Content @'
jobs:
'@ -ExpectThrow

    $failures = $script:selfTestFailures
    if ($failures -gt 0) {
        Write-Host "$failures self-test case(s) FAILED." -ForegroundColor Red
        return $false
    }
    Write-Host "Self-test PASSED." -ForegroundColor Green
    return $true
}

if ($SelfTest) {
    if (Invoke-SelfTest) { exit 0 }
    exit 1
}

if (-not $WorkflowDirectory) {
    # Scripts/ -> repository root. Resolving from the script's own
    # location keeps this correct in every worktree.
    $repoRoot = Split-Path $PSScriptRoot -Parent
    $WorkflowDirectory = Join-Path $repoRoot '.github/workflows'
}

if (-not (Test-Path -LiteralPath $WorkflowDirectory -PathType Container)) {
    Write-Host "FAIL: no workflow directory at $WorkflowDirectory" -ForegroundColor Red
    exit 1
}

$files = @(Get-ChildItem -LiteralPath $WorkflowDirectory -Filter '*.yml' -File | Sort-Object Name)
if ($files.Count -eq 0) {
    Write-Host "FAIL: no workflow files in $WorkflowDirectory" -ForegroundColor Red
    exit 1
}

Write-Host "==> Job timeouts ($($files.Count) workflow file(s))" -ForegroundColor Cyan

$violations = 0
foreach ($file in $files) {
    $lines = Get-Content -LiteralPath $file.FullName
    try {
        $offenders = @(Get-JobWithoutTimeout -Line $lines -Label $file.Name)
    }
    catch {
        Write-Host "  FAIL  $($file.Name): $($_.Exception.Message)" -ForegroundColor Red
        $violations++
        continue
    }

    if ($offenders.Count -eq 0) {
        Write-Host "  PASS  $($file.Name)" -ForegroundColor Green
        continue
    }

    foreach ($job in $offenders) {
        Write-Host "  FAIL  $($file.Name): job '$job' has no timeout-minutes" -ForegroundColor Red
        $violations++
    }
}

if ($violations -gt 0) {
    Write-Host ""
    Write-Host "$violations job(s) without a timeout. Add 'timeout-minutes:' beside 'runs-on:'." -ForegroundColor Red
    Write-Host "A job without one inherits GitHub's 6-hour default, which only a hang ever reaches." -ForegroundColor Yellow
    exit 1
}

Write-Host "PASS: every job states a timeout." -ForegroundColor Green
exit 0
