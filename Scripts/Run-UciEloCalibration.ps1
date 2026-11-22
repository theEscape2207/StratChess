<#
.SYNOPSIS
    UCI_Elo calibration gauntlet: StratChess at UCI_LimitStrength against Stockfish 19 at
    its own UCI_LimitStrength, one fastchess match per (UciElo, SfElo) pair.

.DESCRIPTION
    Plays each -UciElo[i] against -SfElo[i] in order as its own attempt, with Run-EloMatch's
    adjudication (draw 40/8/10, resign 4/800), colour-swapped pairs and the large book
    (StratChessSupport\EngineTesting\openings-large.pgn, sequential from the one-based
    -OpeningStart). Both engines: Threads=1, Hash=64, UCI_LimitStrength=true, UCI_Elo=<n>;
    StratChess at its default MultiPV. Stockfish is StratChessSupport\EngineTesting\stockfish.exe
    (sf_19, see stockfish-PROVENANCE.txt). Writes no Measurements/local.md row: a limited engine
    against Stockfish is not a per-change result; Measurements/uci-elo-calibration.md is the ledger.

    One directory per attempt, under the MAIN checkout's logs\elo\calibration\ so it outlives
    the worktree:
        <stamp>-s<stage>-elo<UciElo>-sf<SfElo>[-smoke]\  config.json, match.log, games.pgn
    On a clean finish ONLY (fastchess exit 0, every game present, no time loss, illegal move,
    disconnect or stall) the driver writes run.json there:
        complete, smoke, stage, uciElo, sfElo, games, openingStart, openingEnd, tc, concurrency,
        machine, candidate {commit, dirty, engineName, exe, sha256}, stockfish {exe, sha256},
        book {path, sha256}, options {StratChess, Stockfish}, startedAt, finishedAt,
        wallSeconds, pvWarnings, bestmovePvMismatch.
    A failed attempt gets no run.json and is never read by fit_uci_elo_calibration.py: discard it
    and rerun the pair once; a second failure stops the calibration.

    A level L is reached with UciElo = ceil(inverse(L)) through -Table, so it plays inside the
    depth band 1 + int(L). -Table must be the candidate's StrengthLimit.cpp table; its default is
    the shipped one, under which levels 0, 2, 4, 7, 10, 14, 19 are 907, 1103, 1315, 1535, 1767, 2126,
    2355. The 2026-11-21 gauntlet ran under -Table 1320:0,2600:19 (1320, 1455, 1590, 1792, 1994,
    2264, 2600).

.HOW TO INVOKE (from bash, cmd, or PowerShell)
    Build the shipping exe first (.\build.ps1 main); this script does not build. Then:
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\Run-UciEloCalibration.ps1 -Smoke
    pwsh ... Run-UciEloCalibration.ps1 -Stage 1 -UciElo 907,1103,1315,1535,1767,2126,2355 `
        -SfElo 1320,1320,1320,1535,1767,2126,2355 -Games 60 -OpeningStart 1
    pwsh ... Run-UciEloCalibration.ps1 -Stage 2 -UciElo <same> -SfElo <stage-1 report> -Games 240 -OpeningStart 31
    Then: python Scripts\fit_uci_elo_calibration.py <attempt dirs> [--table <same>] [--stage1-report | --smoke-check]

.NOTES
    Must be invoked with -File, not dot-sourced.
    Concurrency 8 is a benchmark condition: run.json records it with the machine.
    Do not rebuild the candidate between stages: the fit refuses attempts whose candidate SHA-256
    differs, and a rebuild changes the embedded commit hash.
#>

[CmdletBinding()]
param(
    # 1 (at each level's own label) or 2 (at the stage-1 estimate); a -Smoke run may leave it at 0.
    [ValidateSet(0, 1, 2)]
    [int]$Stage = 0,
    # StratChess UCI_Elo per attempt. Comma-separated also as one string: pwsh -File passes
    # "1320,1455" as a single argument, which an [int[]] parameter refuses.
    [string[]]$UciElo = @(),
    # Stockfish UCI_Elo per attempt, paired by position with -UciElo; 1320..3190.
    [string[]]$SfElo = @(),
    # Games per attempt, even (colour-swapped pairs).
    [int]$Games = 0,
    # One-based first opening of the sequential book slice; every attempt plays the same slice.
    [int]$OpeningStart = 1,
    [string]$Tc = '60+0.6',
    [int]$Concurrency = 8,
    # 20 games at level 0 vs SF 1320 unless -UciElo/-SfElo say otherwise; run.json says smoke.
    [switch]$Smoke,
    # Default: this worktree's build\windows-clang-cl\StratChessEvolved.exe, freshness-checked.
    [string]$CandidateExe = '',
    # elo:level anchors the candidate's UCI_Elo runs through (StrengthLimit.cpp's kAnchors); labels
    # each attempt's level and prices it. Default: the shipped table.
    [string]$Table = '907:0,1103:2.00390625,1315:4.0078125,1535:7.00625,1767:10.0046875,2127:14.0125,2355:19',
    # Runs the ceil(inverse) arithmetic and attempt-dir cases and exits. Pure.
    [switch]$SelfTest
)

Set-StrictMode -Version Latest

function ConvertTo-Anchors {
    <# 'elo:level,...' -> anchors strictly increasing in both columns, as StrengthLimit.cpp requires. #>
    param([Parameter(Mandatory)][string]$Text)
    $parsed = @($Text -split ',' | ForEach-Object {
            $eloText, $levelText = $_ -split ':'
            [pscustomobject]@{
                Elo   = [double]::Parse($eloText, [cultureinfo]::InvariantCulture)
                Level = [double]::Parse($levelText, [cultureinfo]::InvariantCulture)
            }
        })
    for ($i = 1; $i -lt $parsed.Count; $i++) {
        if ($parsed[$i].Elo -le $parsed[$i - 1].Elo -or $parsed[$i].Level -le $parsed[$i - 1].Level) {
            throw "-Table must increase strictly in both columns: $Text"
        }
    }
    return $parsed
}

function Get-UciEloForLevel {
    <# ceil(inverse(L)): the smallest UCI_Elo whose level is at least L. [int] would round
       half-to-even; the epsilon keeps an exact anchor from ceiling one above itself. #>
    param([Parameter(Mandatory)][double]$Level, [Parameter(Mandatory)][object[]]$Anchors)
    for ($i = 1; $i -lt $Anchors.Count; $i++) {
        $lo = $Anchors[$i - 1]
        $hi = $Anchors[$i]
        if ($Level -le $hi.Level) {
            $from = [math]::Max($Level, $lo.Level)
            return [int][math]::Ceiling($lo.Elo + ($hi.Elo - $lo.Elo) * ($from - $lo.Level) / ($hi.Level - $lo.Level) - 1e-9)
        }
    }
    throw "level $Level is above the table"
}

function Get-LevelForElo {
    <# StrengthLimit.cpp's level_for_elo over the given anchors. #>
    param([Parameter(Mandatory)][int]$Elo, [Parameter(Mandatory)][object[]]$Anchors)
    if ($Elo -le $Anchors[0].Elo) { return $Anchors[0].Level }
    for ($i = 1; $i -lt $Anchors.Count; $i++) {
        $lo = $Anchors[$i - 1]
        $hi = $Anchors[$i]
        if ($Elo -le $hi.Elo) { return $lo.Level + ($hi.Level - $lo.Level) * ($Elo - $lo.Elo) / ($hi.Elo - $lo.Elo) }
    }
    return $Anchors[-1].Level
}

function Get-AttemptDirName {
    param(
        [Parameter(Mandatory)][string]$Stamp,
        [Parameter(Mandatory)][int]$StageNumber,
        [Parameter(Mandatory)][int]$Candidate,
        [Parameter(Mandatory)][int]$Opponent,
        [switch]$IsSmoke
    )
    $suffix = if ($IsSmoke) { '-smoke' } else { '' }
    return "$Stamp-s$StageNumber-elo$Candidate-sf$Opponent$suffix"
}

function ConvertTo-EloList {
    <# Splits each item on commas, so "1320,1455" (pwsh -File) and 1320, 1455 (in-process) agree. #>
    param([Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Item)
    return @($Item | ForEach-Object { $_ -split ',' } | Where-Object { $_.Trim() -ne '' } | ForEach-Object { [int]::Parse($_.Trim(), [cultureinfo]::InvariantCulture) })
}

function Get-GameSecondsForecast {
    <# Game-slot seconds a game: ~90 at levels 0-4, 115 at 7-10, 155 at 14-19 (the 2026-11-21 forecast). #>
    param([Parameter(Mandatory)][double]$Level)
    if ($Level -lt 5) { return 90 }
    if ($Level -lt 12) { return 115 }
    return 155
}

if ($SelfTest) {
    $failures = 0
    $old = @(ConvertTo-Anchors -Text '1320:0,2600:19')
    $shipped = @(ConvertTo-Anchors -Text $Table)
    $sampled = @(0, 2, 4, 7, 10, 14, 19)
    $cases = @(
        @{ Name = '1320:0,2600:19 levels -> 1320..2600'; Got = { ($sampled | ForEach-Object { Get-UciEloForLevel -Level $_ -Anchors $old }) -join '/' }; Expect = '1320/1455/1590/1792/1994/2264/2600' }
        @{ Name = 'falsification: [int] of the inverse at level 4 is NOT the answer'; Got = { [int](1320 + 4 * 1280 / 19.0) -ne 1590 }; Expect = $true }
        @{ Name = '1455 plays level 2.0039'; Got = { [math]::Round((Get-LevelForElo -Elo 1455 -Anchors $old), 4) }; Expect = 2.0039 }
        @{ Name = '2264 plays level 14.0125, depth band 15'; Got = { 1 + [math]::Truncate((Get-LevelForElo -Elo 2264 -Anchors $old)) }; Expect = 15 }
        @{ Name = 'default table levels -> 907..2355'; Got = { ($sampled | ForEach-Object { Get-UciEloForLevel -Level $_ -Anchors $shipped }) -join '/' }; Expect = '907/1103/1315/1535/1767/2126/2355' }
        @{ Name = 'every sampled level stays in its band, both tables'; Got = { @(foreach ($t in @(, $old) + @(, $shipped)) { $sampled | Where-Object { [math]::Truncate((Get-LevelForElo -Elo (Get-UciEloForLevel -Level $_ -Anchors $t) -Anchors $t)) -ne $_ } }).Count }; Expect = 0 }
        @{ Name = 'a decreasing table is refused'; Got = { try { ConvertTo-Anchors -Text '1500:0,1400:19' | Out-Null; 'accepted' } catch { 'refused' } }; Expect = 'refused' }
        @{ Name = 'attempt dir';       Got = { Get-AttemptDirName -Stamp '20261121-130000' -StageNumber 1 -Candidate 1455 -Opponent 1455 }; Expect = '20261121-130000-s1-elo1455-sf1455' }
        @{ Name = 'smoke attempt dir'; Got = { Get-AttemptDirName -Stamp '20261121-130000' -StageNumber 0 -Candidate 1320 -Opponent 1320 -IsSmoke }; Expect = '20261121-130000-s0-elo1320-sf1320-smoke' }
        @{ Name = 'one -File string splits'; Got = { (ConvertTo-EloList -Item @('1320,1455,2600')) -join '/' }; Expect = '1320/1455/2600' }
        @{ Name = 'in-process array passes through'; Got = { (ConvertTo-EloList -Item @('1320', '1455')) -join '/' }; Expect = '1320/1455' }
        @{ Name = 'forecast by level'; Got = { '{0}/{1}/{2}' -f (Get-GameSecondsForecast -Level 4.0078), (Get-GameSecondsForecast -Level 10.0047), (Get-GameSecondsForecast -Level 14.0125) }; Expect = '90/115/155' }
    )
    foreach ($c in $cases) {
        $got = & $c.Got
        if ($got -eq $c.Expect) {
            Write-Host "  ok    $($c.Name)" -ForegroundColor DarkGray
        } else {
            Write-Host "  FAIL  $($c.Name): expected '$($c.Expect)', got '$got'" -ForegroundColor Red
            $failures++
        }
    }
    if ($failures) {
        Write-Host "FAIL: $failures self-test case(s) failed." -ForegroundColor Red
        exit 1
    }
    Write-Host 'PASS: Run-UciEloCalibration self-test.' -ForegroundColor Green
    exit 0
}

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'QuietWindow.ps1')

# --- Arguments ---------------------------------------------------------------------------------
$UciElo = @(ConvertTo-EloList -Item $UciElo)
$SfElo = @(ConvertTo-EloList -Item $SfElo)
if ($Smoke) {
    if ($UciElo.Count -eq 0) { $UciElo = @(1320) }
    if ($SfElo.Count -eq 0) { $SfElo = @(1320) }
    if ($Games -eq 0) { $Games = 20 }
} elseif ($Stage -eq 0) {
    Write-Host '-Stage 1 or 2 is required outside -Smoke.' -ForegroundColor Red
    exit 1
}
if ($UciElo.Count -eq 0 -or $UciElo.Count -ne $SfElo.Count) {
    Write-Host "-UciElo and -SfElo must be non-empty and the same length ($($UciElo.Count) vs $($SfElo.Count))." -ForegroundColor Red
    exit 1
}
if ($Games -le 0 -or $Games % 2 -ne 0) {
    Write-Host "-Games must be a positive even number (colour-swapped pairs); got $Games." -ForegroundColor Red
    exit 1
}
foreach ($opp in $SfElo) {
    if ($opp -lt 1320 -or $opp -gt 3190) {
        Write-Host "Stockfish UCI_Elo $opp is outside its 1320..3190 range." -ForegroundColor Red
        exit 1
    }
}
$pairCount = $Games / 2
$openingEnd = $OpeningStart + $pairCount - 1

# --- Paths ---------------------------------------------------------------------------------------
$RepoRoot = Split-Path $PSScriptRoot -Parent
$mainRoot = (git -C $RepoRoot rev-parse --path-format=absolute --git-common-dir) -replace '[\\/]\.git[\\/]?$', ''
$EngineTesting = Join-Path (Split-Path $mainRoot -Parent) 'StratChessSupport\EngineTesting'
$fastchess = Join-Path $EngineTesting 'fastchess.exe'
$stockfish = Join-Path $EngineTesting 'stockfish.exe'
$bookPath = Join-Path $EngineTesting 'openings-large.pgn'
$calibrationRoot = Join-Path $mainRoot 'logs\elo\calibration'

if ($CandidateExe -eq '') {
    $CandidateExe = & (Join-Path $PSScriptRoot 'Get-BuildArtifact.ps1')
    if (-not $CandidateExe) { Write-Host 'No fresh candidate build; run .\build.ps1 main.' -ForegroundColor Red; exit 1 }
}
foreach ($needed in @($fastchess, $stockfish, $bookPath, $CandidateExe)) {
    if (-not (Test-Path $needed)) { Write-Host "MISSING: $needed" -ForegroundColor Red; exit 1 }
}
$CandidateExe = (Resolve-Path $CandidateExe).Path

$openingCount = @(Select-String -Path $bookPath -Pattern '^\[Event').Count
if ($OpeningStart -lt 1 -or $openingEnd -gt $openingCount) {
    Write-Host "Openings $OpeningStart-$openingEnd fall outside the book's 1-$openingCount." -ForegroundColor Red
    exit 1
}

$commit = (git -C $RepoRoot rev-parse HEAD).Trim()
$isDirty = [bool](git -C $RepoRoot status --porcelain --untracked-files=no)
$candidateHash = (Get-FileHash $CandidateExe -Algorithm SHA256).Hash
$stockfishHash = (Get-FileHash $stockfish -Algorithm SHA256).Hash
$bookHash = (Get-FileHash $bookPath -Algorithm SHA256).Hash

# --- Plan ----------------------------------------------------------------------------------------
$tableAnchors = @(ConvertTo-Anchors -Text $Table)
$phases = for ($i = 0; $i -lt $UciElo.Count; $i++) {
    $lvl = Get-LevelForElo -Elo $UciElo[$i] -Anchors $tableAnchors
    @{
        Name    = "elo $($UciElo[$i]) (level $($lvl.ToString('0.####', [cultureinfo]::InvariantCulture))) vs SF $($SfElo[$i]), $Games games"
        Quiet   = $true
        Seconds = $Games * (Get-GameSecondsForecast -Level $lvl) / $Concurrency + 30
        Rough   = $true
    }
}
$stageLabel = if ($Smoke) { 'smoke' } else { "stage $Stage" }
Write-Host "==> UCI_Elo calibration, $stageLabel | openings $OpeningStart-$openingEnd | tc=$Tc | concurrency=$Concurrency" -ForegroundColor Cyan
Write-Host "    candidate $CandidateExe ($($commit.Substring(0, 8))$(if ($isDirty) { '+dirty' }), sha256 $($candidateHash.Substring(0, 12)))"
Write-Host "    attempts under $calibrationRoot"
Write-PhasePlan -Phase @($phases)

$pvWarnRe = 'Warning; (Illegal PV move|PV continues after (checkmate|stalemate|threefold repetition|fifty-move rule)|Incomplete mating PV|Mating PV does not end|Too long mating PV|Bestmove does not match beginning of last PV|Sign mismatch in mate scores)'
$fatalRe = 'Illegal move .* played by|Move does not match uci move format|disconnects|connection stalls|Engine .* stalls|loses on time|stalled / disconnected|is not responsive|No output from|respond to uci'

$failed = @()
$finished = @()
for ($i = 0; $i -lt $UciElo.Count; $i++) {
    $candElo = $UciElo[$i]
    $oppElo = $SfElo[$i]
    $phase = @($phases)[$i]
    Write-PhaseBanner -Name $phase.Name -Quiet $phase.Quiet -Seconds $phase.Seconds -Rough $phase.Rough

    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $attemptDir = Join-Path $calibrationRoot (Get-AttemptDirName -Stamp $stamp -StageNumber $Stage -Candidate $candElo -Opponent $oppElo -IsSmoke:$Smoke)
    $wdStrat = Join-Path $attemptDir 'wd-strat'
    $wdSf = Join-Path $attemptDir 'wd-sf'
    New-Item -ItemType Directory -Force $wdStrat, $wdSf | Out-Null
    $pgnOut = Join-Path $attemptDir 'games.pgn'
    $matchLog = Join-Path $attemptDir 'match.log'

    $stratOptions = [ordered]@{ Threads = 1; Hash = 64; UCI_LimitStrength = 'true'; UCI_Elo = $candElo }
    $sfOptions = [ordered]@{ Threads = 1; Hash = 64; UCI_LimitStrength = 'true'; UCI_Elo = $oppElo }
    $stratArgs = @("cmd=$CandidateExe", 'name=StratChess', "dir=$wdStrat", 'args=uci') + @($stratOptions.Keys | ForEach-Object { "option.$_=$($stratOptions[$_])" })
    $sfArgs = @("cmd=$stockfish", "name=SF-$oppElo", "dir=$wdSf") + @($sfOptions.Keys | ForEach-Object { "option.$_=$($sfOptions[$_])" })
    $fcArgs = @(
        '-engine') + $stratArgs + @('-engine') + $sfArgs + @(
        '-each', "tc=$Tc",
        '-rounds', $pairCount, '-repeat', '-concurrency', $Concurrency, '-recover',
        '-openings', "file=$bookPath", 'format=pgn', 'order=sequential', "start=$OpeningStart",
        '-draw', 'movenumber=40', 'movecount=8', 'score=10',
        '-resign', 'movecount=4', 'score=800',
        '-pgnout', "file=$pgnOut", 'notation=san')

    $startedAt = Get-Date
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    Push-Location $attemptDir
    try {
        & $fastchess @fcArgs 2>&1 | Tee-Object -FilePath $matchLog | Out-Null
        $fcExit = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    $clock.Stop()

    $logLines = @(Get-Content $matchLog)
    $disasters = @($logLines | Select-String -Pattern $fatalRe)
    $pvWarnings = @($logLines | Select-String -Pattern $pvWarnRe)
    $mismatch = @($logLines | Select-String -SimpleMatch 'Bestmove does not match beginning of last PV')
    $played = if (Test-Path $pgnOut) { @(Select-String -Path $pgnOut -Pattern '^\[Event').Count } else { 0 }
    $scoreLine = $logLines | Select-String -Pattern 'Games: \d+' | Select-Object -Last 1

    $problems = @()
    if ($fcExit -ne 0) { $problems += "fastchess exited $fcExit" }
    if ($disasters.Count) { $problems += "$($disasters.Count) harness/engine failures: $($disasters[0].Line.Trim())" }
    if ($played -ne $Games) { $problems += "games.pgn holds $played of $Games games" }

    if ($problems.Count) {
        Write-Host "FAILED attempt $attemptDir (discard, rerun once):" -ForegroundColor Red
        $problems | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
        $failed += $attemptDir
        continue
    }

    $engineName = ''
    $nameLine = Select-String -Path $pgnOut -Pattern '^\[Engine(White|Black)Name "(StratChess[^"]*)"\]' | Select-Object -First 1
    if ($nameLine) { $engineName = $nameLine.Matches[0].Groups[2].Value }
    $run = [ordered]@{
        complete           = $true
        smoke              = [bool]$Smoke
        stage              = $Stage
        uciElo             = $candElo
        sfElo              = $oppElo
        games              = $Games
        openingStart       = $OpeningStart
        openingEnd         = $openingEnd
        tc                 = $Tc
        concurrency        = $Concurrency
        machine            = $env:COMPUTERNAME
        candidate          = [ordered]@{ commit = $commit; dirty = $isDirty; engineName = $engineName; exe = $CandidateExe; sha256 = $candidateHash }
        stockfish          = [ordered]@{ exe = $stockfish; sha256 = $stockfishHash }
        book               = [ordered]@{ path = $bookPath; sha256 = $bookHash }
        options            = [ordered]@{ StratChess = $stratOptions; Stockfish = $sfOptions }
        startedAt          = $startedAt.ToString('o', [cultureinfo]::InvariantCulture)
        finishedAt         = (Get-Date).ToString('o', [cultureinfo]::InvariantCulture)
        wallSeconds        = [math]::Round($clock.Elapsed.TotalSeconds, 1)
        pvWarnings         = $pvWarnings.Count
        bestmovePvMismatch = $mismatch.Count
    }
    $run | ConvertTo-Json -Depth 5 | Set-Content -Path (Join-Path $attemptDir 'run.json') -Encoding utf8NoBOM
    $finished += $attemptDir
    $minutes = ($clock.Elapsed.TotalMinutes).ToString('0.0', [cultureinfo]::InvariantCulture)
    Write-Host "done: $attemptDir ($minutes min) $(if ($scoreLine) { $scoreLine.Line.Trim() })" -ForegroundColor Green
}

Write-PhaseBanner -Name 'calibration attempts finished' -Quiet $false -Seconds 0
Write-Host ''
Write-Host 'Complete attempts:'
$finished | ForEach-Object { Write-Host "  $_" }
if ($failed.Count) {
    Write-Host 'Failed attempts (no run.json; discard and rerun each once):' -ForegroundColor Red
    $failed | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}
exit 0
