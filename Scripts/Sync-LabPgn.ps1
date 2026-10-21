<#
.SYNOPSIS
    Lists the successful strength-lab runs not yet copied to StrengthLabPgn, and copies them.

.DESCRIPTION
    GitHub deletes a run's shard artifacts 90 days after the run, and the PGNs inside are the raw
    corpus every game analysis starts from. The rule is to copy EVERY successful strength.yml
    run: a ledger row is written days after the run, so "copy what has a row" races the expiry.
    Failed and cancelled runs are not copied.

    With no flags it only lists, oldest first, with the days each run has left. Exit 0 when
    nothing is missing, 1 when something is.

    With -Download it fetches each missing run into
    <StrengthLabPgn>\<run date>_strength-<run id>\, the layout `gh run download` writes and the
    analysis scripts read, then appends the run's row to the folder's README.md, sorted by
    folder. A run is downloaded to a `.partial` folder and renamed only once every shard is
    present, so an interrupted download is retried rather than mistaken for a copy.

    A successful run can carry no shard artifacts (a probe, or one already expired). Those are
    named separately and are never "missing": there is nothing left to copy.

    StrengthLabPgn is StratChessSupport\StrengthLabPgn beside the main checkout, not in the repo.

.PARAMETER Download
    Fetch the missing runs and add their README rows.

.PARAMETER Repo
    The GitHub repository whose strength.yml runs are listed.

.PARAMETER PgnRoot
    Override the StrengthLabPgn folder.

.PARAMETER RunLimit
    How many of the newest successful runs to consider. The window is 90 days of ~1 run a day, so
    the default is generous.

.PARAMETER SelfTest
    Run synthetic tests over the pure parts and exit.

.HOW TO INVOKE
    pwsh -File Scripts/Sync-LabPgn.ps1
    pwsh -File Scripts/Sync-LabPgn.ps1 -Download
    pwsh -File Scripts/Sync-LabPgn.ps1 -SelfTest
#>

[CmdletBinding(DefaultParameterSetName = 'Run')]
param(
    [Parameter(ParameterSetName = 'Run')]
    [switch]$Download,

    [Parameter(ParameterSetName = 'Run')]
    [string]$Repo = 'theEscape2207/StratChess',

    [Parameter(ParameterSetName = 'Run')]
    [string]$PgnRoot = '',

    [Parameter(ParameterSetName = 'Run')]
    [ValidateRange(1, 1000)]
    [int]$RunLimit = 500,

    [Parameter(Mandatory, ParameterSetName = 'SelfTest')]
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# GitHub's artifact retention for strength.yml (`retention-days`), and the maximum for a public repo.
$script:RetentionDays = 90
$script:FolderPattern = '^(\d{4}-\d{2}-\d{2})_strength-(\d+)$'
$script:RowPattern = '^\| `(\d{4}-\d{2}-\d{2}_strength-\d+)/` \|'

function ConvertTo-Utc {
    param([Parameter(Mandatory)][string]$Value)

    return [datetime]::Parse(
        $Value, [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::AdjustToUniversal -bor
        [Globalization.DateTimeStyles]::AssumeUniversal)
}

function Get-RunFolderName {
    param([Parameter(Mandatory)][datetime]$Created, [Parameter(Mandatory)][string]$RunId)

    return '{0}_strength-{1}' -f $Created.ToString('yyyy-MM-dd', [Globalization.CultureInfo]::InvariantCulture), $RunId
}

function Get-DaysLeft {
    <#
      .SYNOPSIS
        Whole days until the run's artifacts expire (negative once they have).
    #>
    param([Parameter(Mandatory)][datetime]$Created, [Parameter(Mandatory)][datetime]$Now)

    return [int][math]::Floor(($Created.AddDays($script:RetentionDays) - $Now).TotalDays)
}

function Get-LocalRunId {
    <#
      .SYNOPSIS
        Run ids of the finished copies among a folder's child names; `.partial` leftovers do not count.
    #>
    param([Parameter(Mandatory)][AllowEmptyCollection()][string[]]$FolderName)

    $ids = foreach ($n in $FolderName) {
        $m = [regex]::Match($n, $script:FolderPattern)
        if ($m.Success) { $m.Groups[2].Value }
    }
    return @($ids)
}

function Get-MissingRun {
    <#
      .SYNOPSIS
        The runs whose id is not among the local ones, oldest first.
    #>
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Run,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$LocalId
    )

    return @($Run | Where-Object { $_.Id -notin $LocalId } | Sort-Object { $_.Created })
}

function Get-ShardCount {
    <#
      .SYNOPSIS
        Live shard artifacts in a run's artifact-list JSON, by name. Expired ones cannot be downloaded.
    #>
    param([Parameter(Mandatory)][string]$ArtifactsJson)

    $list = ($ArtifactsJson | ConvertFrom-Json).artifacts
    return @($list | Where-Object { $_.name -match '^strength-\d+-shard-\d+$' -and -not $_.expired }).Count
}

function Get-PgnGameCount {
    <#
      .SYNOPSIS
        Games in a PGN: one `[Result ` tag each.
    #>
    param([Parameter(Mandatory)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Line)

    return @($Line | Where-Object { $_.StartsWith('[Result ') }).Count
}

function Get-ReadmeRow {
    param(
        [Parameter(Mandatory)][string]$Folder,
        [Parameter(Mandatory)][string]$Sha,
        [Parameter(Mandatory)][int]$Shards,
        [Parameter(Mandatory)][int]$Games
    )

    $inv = [Globalization.CultureInfo]::InvariantCulture
    return '| `{0}/` | `{1}` | {2} | {3} | |' -f $Folder, $Sha.Substring(0, [math]::Min(9, $Sha.Length)),
        $Shards.ToString($inv), $Games.ToString('N0', $inv)
}

function Merge-ReadmeRow {
    <#
      .SYNOPSIS
        README text with the new rows added to the runs table, kept sorted by folder.
      .DESCRIPTION
        Keeps the file's own line endings and trailing newline. A folder already in the table is not
        added twice; a README with no table to extend is an error, not a silent no-op.
    #>
    param(
        [Parameter(Mandatory)][string]$Text,
        [Parameter(Mandatory)][string[]]$NewRow
    )

    $eol = if ($Text.Contains("`r`n")) { "`r`n" } else { "`n" }
    $lines = [System.Collections.Generic.List[string]]::new([string[]]($Text -split "`r?`n"))
    $first = -1; $last = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match $script:RowPattern) {
            if ($first -lt 0) { $first = $i }
            $last = $i
        }
    }
    if ($first -lt 0) { throw 'README has no runs table to extend.' }
    if (@($lines[$first..$last] | Where-Object { $_ -notmatch $script:RowPattern }).Count -gt 0) {
        throw 'README runs table is not contiguous.'
    }

    $byFolder = @{}
    foreach ($row in @($lines[$first..$last]) + $NewRow) {
        $key = [regex]::Match($row, $script:RowPattern).Groups[1].Value
        if ($key -and -not $byFolder.ContainsKey($key)) { $byFolder[$key] = $row }
    }
    $keys = [string[]]$byFolder.Keys
    [Array]::Sort($keys, [StringComparer]::Ordinal)
    $sorted = @($keys | ForEach-Object { $byFolder[$_] })

    $lines.RemoveRange($first, $last - $first + 1)
    $lines.InsertRange($first, [string[]]$sorted)
    return $lines -join $eol
}

# ----------------------------------------------------------------------------------
# Self-test: the pure parts above, no network and no real folder.
# ----------------------------------------------------------------------------------
function Invoke-SelfTest {
    $script:failures = 0

    function Assert-Equal {
        param([string]$Name, $Actual, $Expected)
        if ("$Actual" -ceq "$Expected") { Write-Host "  PASS  $Name" -ForegroundColor Green }
        else {
            Write-Host "  FAIL  $Name (expected '$Expected', got '$Actual')" -ForegroundColor Red
            $script:failures++
        }
    }

    Write-Host '==> Self-test' -ForegroundColor Cyan

    # Expiry: the issue's own pair -- a run on 2026-08-05 goes on 2026-11-03.
    $created = ConvertTo-Utc '2026-08-05T13:43:28Z'
    Assert-Equal 'folder name uses the UTC run date' (Get-RunFolderName $created '31011587737') '2026-08-05_strength-31011587737'
    Assert-Equal 'days left, same day as expiry' (Get-DaysLeft $created (ConvertTo-Utc '2026-11-03T00:00:00Z')) 0
    Assert-Equal 'days left, 12 days out' (Get-DaysLeft $created (ConvertTo-Utc '2026-10-22T10:00:00Z')) 12
    # Floor, not round: 12.1 days left is 12, never 13.
    Assert-Equal 'days left floors' (Get-DaysLeft $created (ConvertTo-Utc '2026-10-22T13:00:00Z')) 12
    Assert-Equal 'days left negative once expired' (Get-DaysLeft $created (ConvertTo-Utc '2026-11-05T00:00:00Z')) (-2)
    # A run late in the UTC day keeps its UTC date, whatever the machine's zone.
    Assert-Equal 'folder date is UTC' (Get-RunFolderName (ConvertTo-Utc '2026-10-19T23:59:00Z') '1') '2026-10-19_strength-1'

    $names = @('2026-10-19_strength-39208302248', '2026-10-20_strength-5.partial', 'notes', '2026-10-21_strength-6')
    Assert-Equal 'local ids skip .partial and strays' ((Get-LocalRunId $names) -join ',') '39208302248,6'
    Assert-Equal 'no folders, no ids' (@(Get-LocalRunId @())).Count 0

    $runs = @(
        [pscustomobject]@{ Id = '3'; Created = (ConvertTo-Utc '2026-10-03T00:00:00Z') }
        [pscustomobject]@{ Id = '1'; Created = (ConvertTo-Utc '2026-10-01T00:00:00Z') }
        [pscustomobject]@{ Id = '2'; Created = (ConvertTo-Utc '2026-10-02T00:00:00Z') }
    )
    Assert-Equal 'missing is oldest first' ((Get-MissingRun $runs @('2')).Id -join ',') '1,3'
    Assert-Equal 'nothing missing' (@(Get-MissingRun $runs @('1', '2', '3'))).Count 0
    # A run that never copied is the falsification: it must be reported, not assumed present.
    Assert-Equal 'a new run is missing' ((Get-MissingRun $runs @('1', '2')).Id -join ',') '3'

    $artifacts = '{"artifacts":[{"name":"strength-9-shard-0","expired":false},{"name":"strength-9-shard-1","expired":false},' +
    '{"name":"strength-9-shard-2","expired":true},{"name":"strength-9-comparison","expired":false},' +
    '{"name":"strength-9-toolkit","expired":false}]}'
    Assert-Equal 'shards: live only, shards only' (Get-ShardCount $artifacts) 2
    Assert-Equal 'shards: none' (Get-ShardCount '{"artifacts":[{"name":"strength-9","expired":false}]}') 0
    Assert-Equal 'shards: empty list' (Get-ShardCount '{"artifacts":[]}') 0

    $pgn = @('[Event "x"]', '[Result "1-0"]', '1. e4 {+0.2/12 0.5s}', '[Result "1/2-1/2"]', '[ResultNote "no"]', '')
    Assert-Equal 'games counted by Result tag' (Get-PgnGameCount $pgn) 2
    Assert-Equal 'empty pgn' (Get-PgnGameCount @()) 0

    Assert-Equal 'row: sha cut to 9, games grouped' `
    (Get-ReadmeRow '2026-10-20_strength-1' '95300b4ec222f59540e1a95aad2ccf68fd7070d3' 18 19980) `
        '| `2026-10-20_strength-1/` | `95300b4ec` | 18 | 19,980 | |'
    Assert-Equal 'row: short sha kept' (Get-ReadmeRow 'a_strength-1' 'abc' 2 5) '| `a_strength-1/` | `abc` | 2 | 5 | |'

    $readme = "# T`r`n`r`n| Folder | Commit |`r`n|---|---|`r`n" +
    "| ``2026-10-01_strength-1/`` | ``aaa`` | 18 | 1 | note |`r`n" +
    "| ``2026-10-03_strength-3/`` | ``ccc`` | 18 | 3 | |`r`n`r`nTail.`r`n"
    $newRow = '| `2026-10-02_strength-2/` | `bbb` | 18 | 2 | |'
    $merged = Merge-ReadmeRow $readme @($newRow)
    $order = [regex]::Matches($merged, 'strength-(\d)/') | ForEach-Object { $_.Groups[1].Value }
    Assert-Equal 'merge sorts a middle row in' ($order -join ',') '1,2,3'
    Assert-Equal 'merge keeps CRLF and the tail' ($merged -replace "`r`n", '|') `
        ('# T||| Folder | Commit |' + '|' + '|---|---|' + '|' + '| `2026-10-01_strength-1/` | `aaa` | 18 | 1 | note |' + '|' +
        $newRow + '|' + '| `2026-10-03_strength-3/` | `ccc` | 18 | 3 | |' + '||Tail.|')
    Assert-Equal 'merge is idempotent' (Merge-ReadmeRow $merged @($newRow)) $merged
    Assert-Equal 'merge keeps an existing row, not the duplicate' `
    ((Merge-ReadmeRow $readme @('| `2026-10-01_strength-1/` | `zzz` | 1 | 1 | |')) -match 'aaa') $true
    $lf = Merge-ReadmeRow ($readme -replace "`r`n", "`n") @($newRow)
    Assert-Equal 'merge keeps LF' $lf.Contains("`r") $false

    $threw = $false
    try { $null = Merge-ReadmeRow "# no table`r`n" @($newRow) } catch { $threw = $true }
    Assert-Equal 'no table is an error' $threw $true

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
# Live
# ----------------------------------------------------------------------------------
function Invoke-Gh {
    <#
      .SYNOPSIS
        gh's stdout as one string; stderr stays on the console so it cannot corrupt parsed output.
    #>
    param([Parameter(Mandatory)][string[]]$Argument)

    $out = & gh @Argument
    if ($LASTEXITCODE -ne 0) { throw "gh $($Argument -join ' ') exited $LASTEXITCODE" }
    return ($out | Out-String)
}

function Get-SuccessfulRun {
    # --jq keeps createdAt a raw UTC string: ConvertFrom-Json would hand back a local-zone DateTime,
    # and the folder date is the UTC one.
    $tsv = Invoke-Gh -Argument @('run', 'list', '--repo', $Repo, '--workflow', 'strength.yml', '--status', 'success',
        '--limit', "$RunLimit", '--json', 'databaseId,createdAt,headSha',
        '--jq', '.[] | [.databaseId,.createdAt,.headSha] | @tsv')
    $runs = foreach ($line in @($tsv -split "`r?`n" | Where-Object { $_.Trim() })) {
        $f = $line -split "`t"
        [pscustomobject]@{ Id = $f[0]; Created = (ConvertTo-Utc $f[1]); Sha = $f[2] }
    }
    return @($runs)
}

function Save-Run {
    <#
      .SYNOPSIS
        Downloads one run's shards and returns its README row. Throws, leaving no finished folder,
        if any shard is missing.
    #>
    param([Parameter(Mandatory)]$Run, [Parameter(Mandatory)][int]$ExpectedShards, [Parameter(Mandatory)][string]$Root)

    $folder = Get-RunFolderName $Run.Created $Run.Id
    $dest = Join-Path $Root $folder
    $partial = "$dest.partial"
    if (Test-Path $partial) { Remove-Item $partial -Recurse -Force }

    & gh run download $Run.Id --repo $Repo -p "strength-$($Run.Id)-shard-*" -D $partial 2>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "gh run download $($Run.Id) exited $LASTEXITCODE" }

    $row = Get-FolderRow -Path $partial -Folder $folder -Sha $Run.Sha -ExpectedShards $ExpectedShards
    Move-Item $partial $dest
    return $row
}

function Get-FolderRow {
    <#
      .SYNOPSIS
        The README row for a run folder, counted from the shards on disk. Throws on a missing shard
        or match.pgn.
    #>
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Folder,
        [Parameter(Mandatory)][string]$Sha,
        [int]$ExpectedShards = 0
    )

    $shardDirs = @(Get-ChildItem $Path -Directory)
    if ($shardDirs.Count -eq 0 -or ($ExpectedShards -gt 0 -and $shardDirs.Count -ne $ExpectedShards)) {
        throw "$Folder`: $($shardDirs.Count) shard folders, expected $ExpectedShards (in $Path)"
    }

    $games = 0
    foreach ($dir in $shardDirs) {
        $pgn = Join-Path $dir.FullName 'match.pgn'
        if (-not (Test-Path $pgn)) { throw "$Folder`: $($dir.Name) has no match.pgn (in $Path)" }
        $games += Get-PgnGameCount ([System.IO.File]::ReadLines($pgn))
    }
    return Get-ReadmeRow $Folder $Sha $shardDirs.Count $games
}

function Add-ReadmeRow {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Row)

    $text = [System.IO.File]::ReadAllText($Path)
    [System.IO.File]::WriteAllText($Path, (Merge-ReadmeRow $text @($Row)), [System.Text.UTF8Encoding]::new($false))
}

if ($PgnRoot -eq '') {
    # --git-common-dir is the main repository's .git however deeply this worktree is nested.
    $repoRoot = Split-Path $PSScriptRoot -Parent
    $mainRoot = (& git -C $repoRoot rev-parse --path-format=absolute --git-common-dir) -replace '[\\/]\.git[\\/]?$', ''
    $PgnRoot = Join-Path (Split-Path $mainRoot -Parent) 'StratChessSupport\StrengthLabPgn'
}
if (-not (Test-Path $PgnRoot)) { throw "StrengthLabPgn folder not found: $PgnRoot" }
$readmePath = Join-Path $PgnRoot 'README.md'
if (-not (Test-Path $readmePath)) { throw "README.md not found in $PgnRoot" }

$localNames = @(Get-ChildItem $PgnRoot -Directory | ForEach-Object { $_.Name })
$localIds = @(Get-LocalRunId $localNames)
$runList = @(Get-SuccessfulRun)
$missing = @(Get-MissingRun $runList $localIds)
$utcNow = (Get-Date).ToUniversalTime()

# A finished folder with no table row is a copy nobody can find by commit.
$readmeText = [System.IO.File]::ReadAllText($readmePath)
$unrowed = @($localNames | Where-Object { $_ -match $script:FolderPattern -and -not $readmeText.Contains("``$_/``") })
foreach ($name in $unrowed) {
    Write-Host "WARN  $name has no README row$($Download ? '' : ' (-Download adds it)')" -ForegroundColor Yellow
}

$copyable = @()
$unavailable = @()
foreach ($run in $missing) {
    $shards = Get-ShardCount (Invoke-Gh -Argument @('api', "repos/$Repo/actions/runs/$($run.Id)/artifacts?per_page=100"))
    $left = Get-DaysLeft $run.Created $utcNow
    $entry = [pscustomobject]@{ Run = $run; Shards = $shards; DaysLeft = $left }
    if ($shards -gt 0) { $copyable += $entry } else { $unavailable += $entry }
}

Write-Host ("{0} successful runs, {1} local, {2} missing" -f $runList.Count, $localIds.Count, $copyable.Count)
foreach ($e in $copyable) {
    Write-Host ("  {0}  {1}  {2}  {3,2} shards  {4,3} days left" -f
        $e.Run.Id, (Get-RunFolderName $e.Run.Created $e.Run.Id).Substring(0, 10), $e.Run.Sha.Substring(0, 9), $e.Shards, $e.DaysLeft) `
        -ForegroundColor ($e.DaysLeft -le 14 ? 'Yellow' : 'White')
}
if ($unavailable.Count -gt 0) {
    Write-Host ("{0} successful run(s) have no live shard artifacts (nothing to copy): {1}" -f
        $unavailable.Count, (($unavailable | ForEach-Object { $_.Run.Id }) -join ' ')) -ForegroundColor DarkGray
}

if (-not $Download) {
    if ($copyable.Count + $unrowed.Count -gt 0) { Write-Host 'Run with -Download to fix that.' -ForegroundColor DarkGray }
    exit ($copyable.Count + $unrowed.Count -gt 0 ? 1 : 0)
}

# Rows for finished folders that lack one: a copy made by hand, or a run interrupted between
# the rename and the row. The commit comes from the run list; a run past the list is skipped.
foreach ($name in $unrowed) {
    $runId = [regex]::Match($name, $script:FolderPattern).Groups[2].Value
    $known = @($runList | Where-Object { $_.Id -eq $runId })
    if ($known.Count -eq 0) {
        Write-Host "SKIP  $name is not among the last $RunLimit successful runs; add its row by hand" -ForegroundColor Yellow
        continue
    }
    $row = Get-FolderRow -Path (Join-Path $PgnRoot $name) -Folder $name -Sha $known[0].Sha
    Add-ReadmeRow -Path $readmePath -Row $row
    Write-Host "    $row"
}

$copied = 0
foreach ($e in $copyable) {
    Write-Host "==> $($e.Run.Id) ($($e.Shards) shards)" -ForegroundColor Cyan
    $row = Save-Run -Run $e.Run -ExpectedShards $e.Shards -Root $PgnRoot
    Add-ReadmeRow -Path $readmePath -Row $row
    Write-Host "    $row"
    $copied++
}
Write-Host "Copied $copied run(s)." -ForegroundColor Green
exit 0
