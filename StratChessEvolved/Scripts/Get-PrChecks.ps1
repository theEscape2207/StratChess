<#
.SYNOPSIS
    One-line verdict on a pull request's checks, with the failure drill-down already done.

.DESCRIPTION
    Answering "is this PR green?" from `gh` alone takes several calls and does not
    survive being interrupted: `gh pr checks` exits 8 while anything is pending, and a
    job that gates on others is not listed at all until its dependencies finish. When
    something is red, finding out why means parsing a job id out of a URL, listing the
    job's steps, downloading the whole log and grepping it.

    This does all of that in one invocation and prints one of three shapes:

      green    PR #352: 3 commits, 10/10 green, 4m41s, MERGEABLE
      red      the failing check, the step that failed, its ##[error] lines, the URL
      running  progress, plus any step running far longer than its siblings

    Read-only by design. It never merges, re-runs or comments -- the point is to make
    the state legible, not to act on it.

    Exit codes are meant for scripting: 0 every check green, 1 at least one failed,
    2 still running (or no pull request found).

.PARAMETER Pr
    Pull request number. Defaults to the one for the current branch.

.PARAMETER Wait
    Poll until every check reaches a terminal state, then report once.

.PARAMETER PollSeconds
    Seconds between polls under -Wait.

.PARAMETER TimeoutMinutes
    Give up waiting after this long and report what is known, exiting 2.

.PARAMETER LogLines
    Trailing log lines to show for a failing job, after its ##[error] lines.

.PARAMETER StuckMinutes
    Flag a step that has been running at least this long. The default is well above
    any healthy step in this repository, where the slowest job finishes in ~4 minutes.

.PARAMETER SelfTest
    Run synthetic tests over the parsing and summarising logic and exit.

.HOW TO INVOKE
    pwsh -File StratChessEvolved/Scripts/Get-PrChecks.ps1
    pwsh -File StratChessEvolved/Scripts/Get-PrChecks.ps1 -Pr 352
    pwsh -File StratChessEvolved/Scripts/Get-PrChecks.ps1 -Wait
    pwsh -File StratChessEvolved/Scripts/Get-PrChecks.ps1 -SelfTest
#>

[CmdletBinding(DefaultParameterSetName = 'Run')]
param(
    [Parameter(ParameterSetName = 'Run')]
    [int]$Pr,

    [Parameter(ParameterSetName = 'Run')]
    [switch]$Wait,

    [Parameter(ParameterSetName = 'Run')]
    [ValidateRange(5, 300)]
    [int]$PollSeconds = 20,

    [Parameter(ParameterSetName = 'Run')]
    [ValidateRange(1, 360)]
    [int]$TimeoutMinutes = 30,

    [Parameter(ParameterSetName = 'Run')]
    [ValidateRange(1, 500)]
    [int]$LogLines = 30,

    [Parameter(ParameterSetName = 'Run')]
    [ValidateRange(1, 360)]
    [int]$StuckMinutes = 10,

    [Parameter(Mandatory, ParameterSetName = 'SelfTest')]
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# A skipped check is a pass here on purpose: build-and-test-result reports SKIPPED as
# success so Docs and Tooling pull requests are not blocked by jobs that correctly
# never ran. NEUTRAL is the same shape. Everything else that is not SUCCESS is a stop.
$script:PassingConclusion = @('SUCCESS', 'SKIPPED', 'NEUTRAL')

function Format-Duration {
    param([Parameter(Mandatory)][AllowNull()][Nullable[double]]$Seconds)

    if ($null -eq $Seconds -or $Seconds -lt 0) { return '--' }
    $t = [TimeSpan]::FromSeconds([Math]::Round($Seconds))
    # Floor, not [int]: PowerShell's int cast rounds to nearest, so 4m41s would
    # render as "5m41s" -- the minutes carried up while the seconds did not.
    if ($t.TotalHours -ge 1) { return ('{0}h{1:00}m' -f [Math]::Floor($t.TotalHours), $t.Minutes) }
    if ($t.TotalMinutes -ge 1) { return ('{0}m{1:00}s' -f [Math]::Floor($t.TotalMinutes), $t.Seconds) }
    return ('{0}s' -f $t.Seconds)
}

function Resolve-JobId {
    <#
      .SYNOPSIS
        The numeric job id in an Actions detailsUrl, or $null if it is not one.
    #>
    param([Parameter(Mandatory)][AllowEmptyString()][AllowNull()][string]$Url)

    if ([string]::IsNullOrWhiteSpace($Url)) { return $null }
    $m = [regex]::Match($Url, '/actions/runs/\d+/job/(\d+)')
    if (-not $m.Success) { return $null }
    return $m.Groups[1].Value
}

function Resolve-RunId {
    param([Parameter(Mandatory)][AllowEmptyString()][AllowNull()][string]$Url)

    if ([string]::IsNullOrWhiteSpace($Url)) { return $null }
    $m = [regex]::Match($Url, '/actions/runs/(\d+)')
    if (-not $m.Success) { return $null }
    return $m.Groups[1].Value
}

function ConvertTo-Utc {
    param([Parameter(Mandatory)][AllowNull()][AllowEmptyString()]$Value)

    if ([string]::IsNullOrWhiteSpace([string]$Value)) { return $null }
    return [datetime]::Parse(
        [string]$Value, [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::AdjustToUniversal -bor
        [Globalization.DateTimeStyles]::AssumeUniversal)
}

function Get-Prop {
    <#
      .SYNOPSIS
        A property's value, or $null when the object does not carry it. Needed because
        StrictMode makes a missing property an error, and a legacy StatusContext has a
        different field set from a CheckRun.
    #>
    param([Parameter(Mandatory)][AllowNull()]$Object, [Parameter(Mandatory)][string]$Name)

    if ($null -eq $Object) { return $null }
    if ($Object.PSObject.Properties.Name -notcontains $Name) { return $null }
    return $Object.$Name
}

function Get-CheckSummary {
    <#
      .SYNOPSIS
        Counts, verdict and elapsed wall-clock for one statusCheckRollup array.
      .DESCRIPTION
        Verdict is 'green' only when every check finished and none failed; 'failed'
        the moment one has failed, even with others still running -- a failure is
        already decisive and waiting for the rest wastes the reader's time; and
        'running' otherwise. `Now` is injected so the self-test is not clock-dependent.
    #>
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][AllowNull()][array]$Rollup,
        [Parameter(Mandatory)][datetime]$Now
    )

    $checks = @($Rollup | Where-Object { $null -ne $_ })
    $passed = 0; $failed = @(); $running = @(); $skipped = 0
    $earliest = $null; $latest = $null

    foreach ($c in $checks) {
        # A legacy StatusContext spells its fields differently from a CheckRun, and
        # carries no timestamps at all.
        $name = (Get-Prop $c 'name')       ?? (Get-Prop $c 'context') ?? '(unnamed)'
        $status = (Get-Prop $c 'status')     ?? 'COMPLETED'
        $concl = (Get-Prop $c 'conclusion') ?? (Get-Prop $c 'state')   ?? ''
        $url = (Get-Prop $c 'detailsUrl') ?? (Get-Prop $c 'targetUrl')

        $started = ConvertTo-Utc (Get-Prop $c 'startedAt')
        $completed = ConvertTo-Utc (Get-Prop $c 'completedAt')
        if ($started -and (-not $earliest -or $started -lt $earliest)) { $earliest = $started }
        if ($completed -and (-not $latest -or $completed -gt $latest)) { $latest = $completed }

        $entry = [pscustomobject]@{
            Name = $name; Status = $status; Conclusion = $concl.ToUpperInvariant()
            Url = $url; Started = $started; Completed = $completed
            Workflow = if ($c.PSObject.Properties.Name -contains 'workflowName') { $c.workflowName } else { $null }
        }

        if ($status.ToUpperInvariant() -ne 'COMPLETED') { $running += $entry; continue }
        if ($entry.Conclusion -in $script:PassingConclusion) {
            $passed++
            if ($entry.Conclusion -eq 'SKIPPED') { $skipped++ }
        }
        else { $failed += $entry }
    }

    # No checks at all is not a pass -- it usually means the run has not been created
    # yet, or a workflow failed to parse. Reporting green there would be the one
    # answer this script must never give wrongly.
    $verdict = if ($checks.Count -eq 0) { 'none' }
               elseif ($failed.Count -gt 0) { 'failed' }
               elseif ($running.Count -gt 0) { 'running' }
               else { 'green' }

    $endpoint = if ($running.Count -gt 0) { $Now } else { $latest }
    $elapsed = if ($earliest -and $endpoint) { ($endpoint - $earliest).TotalSeconds } else { $null }

    return [pscustomobject]@{
        Total = $checks.Count; Passed = $passed; Skipped = $skipped
        Failed = $failed; Running = $running; Verdict = $verdict; ElapsedSeconds = $elapsed
    }
}

# ----------------------------------------------------------------------------------
# Self-test: everything above is pure, so all of it is testable without a network call.
# ----------------------------------------------------------------------------------
function Invoke-SelfTest {
    $script:failures = 0
    $now = [datetime]::Parse('2026-08-19T10:15:00Z', [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::AdjustToUniversal -bor [Globalization.DateTimeStyles]::AssumeUniversal)

    function Assert-Equal {
        param([string]$Name, $Actual, $Expected)
        if ("$Actual" -eq "$Expected") { Write-Host "  PASS  $Name" -ForegroundColor Green }
        else {
            Write-Host "  FAIL  $Name (expected '$Expected', got '$Actual')" -ForegroundColor Red
            $script:failures++
        }
    }

    function New-Check {
        param($Name, $Status, $Conclusion, $Started, $Completed)
        [pscustomobject]@{
            __typename = 'CheckRun'; name = $Name; status = $Status; conclusion = $Conclusion
            startedAt = $Started; completedAt = $Completed; workflowName = 'Build and test'
            detailsUrl = 'https://github.com/o/r/actions/runs/1/job/2'
        }
    }

    Write-Host '==> Self-test' -ForegroundColor Cyan

    Assert-Equal 'duration seconds'  (Format-Duration 42)      '42s'
    Assert-Equal 'duration minutes'  (Format-Duration 281)     '4m41s'
    Assert-Equal 'duration pads'     (Format-Duration 302)     '5m02s'
    # 3599s is 59m59s: the minutes must not carry up into an hour that has not passed.
    Assert-Equal 'duration no carry' (Format-Duration 3599)    '59m59s'
    Assert-Equal 'duration hour edge' (Format-Duration 3660)   '1h01m'
    Assert-Equal 'duration hours'    (Format-Duration 21600)   '6h00m'
    Assert-Equal 'duration null'     (Format-Duration $null)   '--'

    Assert-Equal 'job id parsed' (Resolve-JobId 'https://github.com/o/r/actions/runs/32/job/96') '96'
    Assert-Equal 'run id parsed' (Resolve-RunId 'https://github.com/o/r/actions/runs/32/job/96') '32'
    Assert-Equal 'non-actions url' (Resolve-JobId 'https://example.com/build/7') ''
    Assert-Equal 'empty url'       (Resolve-JobId '') ''

    $green = @(
        (New-Check 'classify'   'COMPLETED' 'SUCCESS' '2026-08-19T10:08:57Z' '2026-08-19T10:09:07Z'),
        (New-Check 'build-linux' 'COMPLETED' 'SUCCESS' '2026-08-19T10:09:10Z' '2026-08-19T10:13:38Z')
    )
    $s = Get-CheckSummary -Rollup $green -Now $now
    Assert-Equal 'green verdict' $s.Verdict 'green'
    Assert-Equal 'green passed'  $s.Passed 2
    # Wall clock spans the earliest start to the latest finish, not the sum.
    Assert-Equal 'green elapsed' (Format-Duration $s.ElapsedSeconds) '4m41s'

    $skipped = @(
        (New-Check 'classify' 'COMPLETED' 'SUCCESS' '2026-08-19T10:08:57Z' '2026-08-19T10:09:07Z'),
        (New-Check 'build-linux' 'COMPLETED' 'SKIPPED' $null $null)
    )
    $s = Get-CheckSummary -Rollup $skipped -Now $now
    Assert-Equal 'skipped is green'   $s.Verdict 'green'
    Assert-Equal 'skipped counted'    $s.Skipped 1
    Assert-Equal 'skipped in passed'  $s.Passed 2

    $failed = @(
        (New-Check 'classify' 'COMPLETED' 'SUCCESS' '2026-08-19T10:08:57Z' '2026-08-19T10:09:07Z'),
        (New-Check 'build-linux' 'COMPLETED' 'FAILURE' '2026-08-19T10:09:10Z' '2026-08-19T10:11:00Z')
    )
    $s = Get-CheckSummary -Rollup $failed -Now $now
    Assert-Equal 'failure verdict' $s.Verdict 'failed'
    Assert-Equal 'failure named'   $s.Failed[0].Name 'build-linux'

    # The incident shape: cancelled must not be mistaken for a pass.
    $cancelled = @(New-Check 'tsan-linux' 'COMPLETED' 'CANCELLED' '2026-08-19T02:04:35Z' '2026-08-19T08:05:08Z')
    $s = Get-CheckSummary -Rollup $cancelled -Now $now
    Assert-Equal 'cancelled is a failure' $s.Verdict 'failed'
    Assert-Equal 'cancelled elapsed'      (Format-Duration $s.ElapsedSeconds) '6h00m'

    # A failure while others still run is already decisive.
    $mixed = @(
        (New-Check 'build-linux' 'COMPLETED' 'FAILURE' '2026-08-19T10:09:10Z' '2026-08-19T10:11:00Z'),
        (New-Check 'lint-linux' 'IN_PROGRESS' $null '2026-08-19T10:09:10Z' $null)
    )
    $s = Get-CheckSummary -Rollup $mixed -Now $now
    Assert-Equal 'failure beats running' $s.Verdict 'failed'

    $running = @(
        (New-Check 'classify' 'COMPLETED' 'SUCCESS' '2026-08-19T10:08:57Z' '2026-08-19T10:09:07Z'),
        (New-Check 'build-linux' 'IN_PROGRESS' $null '2026-08-19T10:09:10Z' $null)
    )
    $s = Get-CheckSummary -Rollup $running -Now $now
    Assert-Equal 'running verdict' $s.Verdict 'running'
    # Elapsed runs to Now while anything is still going, not to the last completion.
    Assert-Equal 'running elapsed' (Format-Duration $s.ElapsedSeconds) '6m03s'

    $s = Get-CheckSummary -Rollup @() -Now $now
    Assert-Equal 'no checks is never green' $s.Verdict 'none'
    Assert-Equal 'no checks total'          $s.Total 0

    if ($script:failures -gt 0) {
        Write-Host "$script:failures self-test case(s) FAILED." -ForegroundColor Red
        return $false
    }
    Write-Host 'Self-test PASSED.' -ForegroundColor Green
    return $true
}

if ($SelfTest) {
    if (Invoke-SelfTest) { exit 0 }
    exit 1
}

# ----------------------------------------------------------------------------------
# Live reporting
# ----------------------------------------------------------------------------------
function Invoke-Gh {
    <#
      .SYNOPSIS
        gh's stdout as one string. stderr is deliberately left on the console rather
        than merged in: gh writes warnings there, and folding them into the result
        would corrupt the JSON the callers parse.
    #>
    param([Parameter(Mandatory)][string[]]$Argument, [switch]$AllowFailure)

    $out = & gh @Argument
    if ($LASTEXITCODE -ne 0 -and -not $AllowFailure) {
        throw "gh $($Argument -join ' ') exited $LASTEXITCODE"
    }
    return ($out | Out-String)
}

function Get-PrSnapshot {
    param([int]$Number)

    $fields = 'number,title,url,commits,mergeable,mergeStateStatus,statusCheckRollup,isDraft'
    $ghArgs = @('pr', 'view')
    if ($Number -gt 0) { $ghArgs += "$Number" }
    $ghArgs += @('--json', $fields)
    return (Invoke-Gh -Argument $ghArgs) | ConvertFrom-Json
}

function Show-FailingCheck {
    <#
      .SYNOPSIS
        The failing step and its errors, so the reader does not have to go and look.
    #>
    param(
        [Parameter(Mandatory)]$Check,
        [Parameter(Mandatory)][string]$Repo,
        [Parameter(Mandatory)][int]$Tail
    )

    $duration = if ($Check.Started -and $Check.Completed) {
        Format-Duration ($Check.Completed - $Check.Started).TotalSeconds
    } else { '--' }

    Write-Host ("  {0,-9} {1}  ({2})" -f $Check.Conclusion, $Check.Name, $duration) -ForegroundColor Red
    if ($Check.Url) { Write-Host "            $($Check.Url)" -ForegroundColor DarkGray }

    $jobId = Resolve-JobId $Check.Url
    if (-not $jobId) {
        Write-Host '            (not an Actions job -- nothing further to fetch)' -ForegroundColor DarkGray
        return
    }

    # Which step failed. A cancelled job usually has no failed step at all, only one
    # left in_progress -- name that instead, since it is where the time went.
    try {
        $job = (Invoke-Gh -Argument @('api', "repos/$Repo/actions/jobs/$jobId")) | ConvertFrom-Json
        $bad = @($job.steps | Where-Object { $_.conclusion -in @('failure', 'cancelled', 'timed_out') }) |
            Select-Object -First 1
        if (-not $bad) {
            $bad = @($job.steps | Where-Object { $_.status -ne 'completed' }) | Select-Object -First 1
        }
        if ($bad) {
            $stepSecs = $null
            $bs = ConvertTo-Utc $bad.started_at
            $bc = ConvertTo-Utc $bad.completed_at
            if ($bs -and $bc) { $stepSecs = ($bc - $bs).TotalSeconds }
            Write-Host ("            step {0} '{1}' -> {2} after {3}" -f `
                    $bad.number, $bad.name, ($bad.conclusion ?? $bad.status), (Format-Duration $stepSecs)) `
                -ForegroundColor Yellow
        }
    }
    catch {
        Write-Host "            (could not read job steps: $($_.Exception.Message))" -ForegroundColor DarkGray
    }

    # The log blob only exists once the job is finished -- which it is, to be here.
    try {
        $log = Invoke-Gh -Argument @('api', "repos/$Repo/actions/jobs/$jobId/logs") -AllowFailure
        $lines = $log -split "`r?`n"
        $errors = @($lines | Where-Object { $_ -match '##\[error\]' } | Select-Object -First 15)
        if ($errors.Count -gt 0) {
            Write-Host '            --- errors ---' -ForegroundColor DarkGray
            foreach ($e in $errors) {
                Write-Host ('            ' + ($e -replace '^\S+\s+', '')) -ForegroundColor Red
            }
        }
        else {
            Write-Host "            --- last $Tail lines ---" -ForegroundColor DarkGray
            foreach ($l in @($lines | Where-Object { $_.Trim() } | Select-Object -Last $Tail)) {
                Write-Host ('            ' + ($l -replace '^\S+\s+', '')) -ForegroundColor DarkGray
            }
        }
    }
    catch {
        Write-Host "            (could not read job log: $($_.Exception.Message))" -ForegroundColor DarkGray
    }
}

function Show-RunningCheck {
    param(
        [Parameter(Mandatory)]$Check,
        [Parameter(Mandatory)][string]$Repo,
        [Parameter(Mandatory)][int]$StuckAfterMinutes,
        [Parameter(Mandatory)][datetime]$Now
    )

    $elapsed = if ($Check.Started) { ($Now - $Check.Started).TotalSeconds } else { $null }
    $jobId = Resolve-JobId $Check.Url
    $stepNote = ''
    $stuck = $false

    if ($jobId) {
        try {
            $job = (Invoke-Gh -Argument @('api', "repos/$Repo/actions/jobs/$jobId") -AllowFailure) | ConvertFrom-Json
            $step = @($job.steps | Where-Object { $_.status -eq 'in_progress' }) | Select-Object -First 1
            if ($step) {
                $ss = ConvertTo-Utc $step.started_at
                $secs = if ($ss) { ($Now - $ss).TotalSeconds } else { $null }
                $stepNote = " -- step $($step.number) '$($step.name)' running $(Format-Duration $secs)"
                if ($secs -and $secs -ge ($StuckAfterMinutes * 60)) { $stuck = $true }
            }
        }
        catch { }
    }

    $label = if ($stuck) { 'STUCK?' } else { 'run' }
    $color = if ($stuck) { 'Yellow' } else { 'DarkGray' }
    Write-Host ("  {0,-9} {1} ({2}){3}" -f $label, $Check.Name, (Format-Duration $elapsed), $stepNote) -ForegroundColor $color
}

$repo = ((Invoke-Gh -Argument @('repo', 'view', '--json', 'nameWithOwner')) | ConvertFrom-Json).nameWithOwner

try { $pull = Get-PrSnapshot -Number $Pr }
catch {
    Write-Host "No pull request found for this branch. $($_.Exception.Message)" -ForegroundColor Yellow
    exit 2
}

$deadline = (Get-Date).ToUniversalTime().AddMinutes($TimeoutMinutes)
$timedOut = $false

while ($true) {
    $now = (Get-Date).ToUniversalTime()
    $summary = Get-CheckSummary -Rollup @($pull.statusCheckRollup) -Now $now
    # 'none' keeps waiting too: straight after a push the run does not exist yet, and
    # reporting "no checks" a second after pushing would be noise, not information.
    if (-not $Wait -or $summary.Verdict -notin @('running', 'none')) { break }
    if ($now -ge $deadline) { $timedOut = $true; break }

    Write-Host ("... {0}/{1} complete, {2} elapsed" -f `
        ($summary.Passed + $summary.Failed.Count), $summary.Total,
        (Format-Duration $summary.ElapsedSeconds)) -ForegroundColor DarkGray
    Start-Sleep -Seconds $PollSeconds
    $pull = Get-PrSnapshot -Number $Pr
}

$commitCount = @($pull.commits).Count
$done = $summary.Passed + $summary.Failed.Count
$skipNote = if ($summary.Skipped -gt 0) { " ($($summary.Skipped) skipped)" } else { '' }
$draftNote = if ($pull.isDraft) { ', DRAFT' } else { '' }

switch ($summary.Verdict) {
    'green' {
        Write-Host ("PR #{0}: {1} commit{2}, {3}/{4} green{5}, {6}, {7}{8}" -f `
                $pull.number, $commitCount, ($commitCount -eq 1 ? '' : 's'),
                $summary.Passed, $summary.Total, $skipNote,
            (Format-Duration $summary.ElapsedSeconds), $pull.mergeStateStatus, $draftNote) -ForegroundColor Green
        exit 0
    }
    'failed' {
        Write-Host ("PR #{0}: {1} commit{2}, {3}/{4} green, {5} failed, {6}, {7}{8}" -f `
                $pull.number, $commitCount, ($commitCount -eq 1 ? '' : 's'),
                $summary.Passed, $summary.Total, $summary.Failed.Count,
            (Format-Duration $summary.ElapsedSeconds), $pull.mergeStateStatus, $draftNote) -ForegroundColor Red
        foreach ($f in $summary.Failed) { Show-FailingCheck -Check $f -Repo $repo -Tail $LogLines }
        if ($summary.Running.Count -gt 0) {
            Write-Host "  ($($summary.Running.Count) check(s) still running)" -ForegroundColor DarkGray
        }
        exit 1
    }
    'none' {
        Write-Host ("PR #{0}: {1} commit{2}, no checks reported{3}, {4}" -f `
                $pull.number, $commitCount, ($commitCount -eq 1 ? '' : 's'),
            ($timedOut ? " after $TimeoutMinutes min" : ' yet'), $pull.mergeStateStatus) -ForegroundColor Yellow
        Write-Host '  Nothing has reported. Check that a workflow matched this event and parsed.' -ForegroundColor DarkGray
        exit 2
    }
    default {
        Write-Host ("PR #{0}: {1} commit{2}, {3}/{4} complete, {5} elapsed{6}" -f `
                $pull.number, $commitCount, ($commitCount -eq 1 ? '' : 's'),
                $done, $summary.Total, (Format-Duration $summary.ElapsedSeconds),
            ($timedOut ? " -- gave up waiting after $TimeoutMinutes min" : '')) -ForegroundColor Yellow
        $nowUtc = (Get-Date).ToUniversalTime()
        foreach ($r in $summary.Running) {
            Show-RunningCheck -Check $r -Repo $repo -StuckAfterMinutes $StuckMinutes -Now $nowUtc
        }
        exit 2
    }
}
