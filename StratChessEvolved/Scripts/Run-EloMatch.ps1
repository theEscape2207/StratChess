<#
.SYNOPSIS
    ELO match: measure a candidate build against the pinned reference build.

.DESCRIPTION
    Runs a fastchess match (color-swapped opening pairs, draw/resign adjudication)
    between a candidate StratChessEvolved.exe and the pinned reference build, then
    reports the ELO difference with its error bound and appends a record line to
    Docs/EloLog.md.

    The reference exe is cached in <DepsRoot>EngineTesting\ and rebuilt on demand
    from its git tag via a temporary worktree, so the procedure survives a wiped
    deps folder. The candidate is NOT built by this script — build it first
    (.\build.ps1 main).

.WHEN TO USE
    After any change that can affect playing strength (search, evaluation, move
    ordering, time management) — tactical suites verify correctness only.
    See Docs/EloLog.md for setup, interpretation guide, and measurement history.

.HOW TO INVOKE (from bash, cmd, or PowerShell)
    cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-EloMatch.ps1"
    Smoke run (pipeline check, 20 games):
    cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-EloMatch.ps1 -Smoke"
    SPRT (stops as soon as the result is decisive; -Games becomes an upper bound):
    cmd.exe /c "pwsh ... Run-EloMatch.ps1 -Sprt NonRegression"   # did this hurt?
    cmd.exe /c "pwsh ... Run-EloMatch.ps1 -Sprt Gain"            # is it worth >= ~10 Elo?

.NOTES
    Must be invoked with -File, not dot-sourced. $PSScriptRoot is $null under dot-source.
    Losses on illegal moves / disconnects / stalls are harness or engine BUGS, not
    strength data — the script exits 1 when fastchess reports any.
    A fixed 500-game batch resolves only ±25 Elo on this setup, so anything expected
    to be worth less than that needs -Sprt to be decidable at all. See the
    "Choosing SPRT vs a fixed batch" section in Docs/EloLog.md.
#>

param(
    # Candidate exe. Default: this repo's Release build (must already exist).
    [string]$CandidateExe = '',
    # Git tag of the pinned reference build.
    [string]$ReferenceTag = 'elo-reference-v1',
    # Explicit path to a reference exe. When set, skips the tag-based cache/rebuild
    # lookup entirely and uses this exe directly as the reference side — ReferenceTag
    # then becomes purely a display-name label (fastchess engine name + EloLog.md row).
    # Use to compare two configurations of the SAME binary (e.g. threads=4 vs threads=1).
    [string]$ReferenceExe = '',
    # Total games (2 games per opening pair). Default ≈ ±15 ELO at 95%.
    [int]$Games = 500,
    # fastchess time control: seconds+increment.
    [string]$Tc = '10+0.1',
    # Default tuned for the current dev machine (12 physical cores / 24 logical
    # threads, AMD Ryzen AI 9 HX 370): each concurrent game runs two
    # single-threaded engine processes, so 6 concurrent games uses all 12
    # physical cores without oversubscribing them. Sized off physical cores,
    # not logical/SMT thread count -- single-threaded search doesn't benefit
    # from SMT sharing, and oversubscribing can introduce timing noise (or
    # genuine time losses) into the measurement on a fixed real-time control.
    # Re-tune if this ever runs on different hardware.
    [int]$Concurrency = 6,
    # Extra fastchess -engine option tokens for the candidate, space-separated
    # (e.g. 'option.Threads=4 option.Hash=64'). Appended verbatim after args=uci.
    [string]$CandidateOptions = '',
    # Extra fastchess -engine option tokens for the reference build, same format.
    [string]$ReferenceOptions = '',
    # 20-game pipeline check instead of a full measurement.
    [switch]$Smoke,
    # Resume an interrupted match instead of starting a new one. Pass the path
    # to the existing logs\elo\<stamp> directory from the run that got killed
    # (it must still contain fastchess's autosaved config.json). All other
    # match-setup parameters (-CandidateExe, -ReferenceTag/-ReferenceExe,
    # -Games, -Tc, -Concurrency, -CandidateOptions, -ReferenceOptions) are
    # ignored when resuming -- fastchess's -config restores the original
    # engine/tournament setup from that directory's saved state.
    [string]$ResumeDir = '',
    # Games between fastchess's own autosave of tournament state (config.json).
    # Lower = less replayed work if a match is ever interrupted, at the cost of
    # more frequent disk writes. Default matches fastchess's own default (20).
    [int]$AutosaveInterval = 20,
    # Sequential Probability Ratio Test: stop as soon as the result is decisive
    # instead of always playing -Games games. Most eval terms in epic #110 are
    # worth 5-20 Elo, i.e. INSIDE the +/-25 Elo noise floor of a 500-game batch --
    # a fixed batch simply cannot resolve them (see Docs/EloLog.md).
    #   NonRegression : elo0=-5 elo1=0  -- "prove it did not make things worse"
    #                   (refactors, restructures, anything expected neutral)
    #   Gain          : elo0=0  elo1=10 -- "prove it is worth >= ~10 Elo"
    #                   (small new eval terms)
    #   Custom        : use -Elo0/-Elo1 explicitly (both required)
    # When set, -Games becomes an upper BOUND rather than a target.
    [ValidateSet('', 'NonRegression', 'Gain', 'Custom')]
    [string]$Sprt = '',
    # SPRT bounds in Elo. Only read when -Sprt is 'Custom'; the presets above
    # set them otherwise. H0 is "the true difference is elo0", H1 is "elo1".
    [int]$Elo0 = 0,
    [int]$Elo1 = 0,
    # SPRT error rates: alpha = P(accept H1 | H0 true), beta = P(accept H0 | H1 true).
    # 0.05 each is the conventional choice and matches fastchess's own examples.
    [double]$Alpha = 0.05,
    [double]$Beta = 0.05,
    # fastchess SPRT model. Pinned to 'logistic' so -Elo0/-Elo1 mean literal Elo,
    # the same scale Docs/EloLog.md reports everywhere. fastchess's own default is
    # 'normalized' (nElo) -- a DIFFERENT scale, on which "elo1=10" would silently
    # mean something else entirely. Override only if you know which scale you want.
    [ValidateSet('logistic', 'normalized', 'bayesian')]
    [string]$SprtModel = 'logistic'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# --- Resolve + validate SPRT settings ----------------------------------------
# Presets exist so callers do not pick alpha by hand. A non-regression test and a
# gain test are different questions with different bounds; conflating them is how
# a neutral change gets recorded as a win.
if ($Sprt -ne '') {
    if ($Smoke) {
        Write-Host '-Sprt cannot be combined with -Smoke: a 20-game run can never reach an SPRT decision,' -ForegroundColor Red
        Write-Host 'so the result would always be "inconclusive" -- which reads like a measurement but is not one.' -ForegroundColor Red
        exit 1
    }
    switch ($Sprt) {
        'NonRegression' { $Elo0 = -5; $Elo1 = 0 }
        'Gain'          { $Elo0 = 0;  $Elo1 = 10 }
        'Custom'        {
            if (-not $PSBoundParameters.ContainsKey('Elo0') -or -not $PSBoundParameters.ContainsKey('Elo1')) {
                Write-Host '-Sprt Custom requires BOTH -Elo0 and -Elo1 to be passed explicitly.' -ForegroundColor Red
                exit 1
            }
        }
    }
    if ($Elo0 -ge $Elo1) {
        Write-Host "SPRT bounds are inverted: elo0=$Elo0 must be strictly less than elo1=$Elo1." -ForegroundColor Red
        exit 1
    }
    if ($Alpha -le 0 -or $Alpha -ge 1 -or $Beta -le 0 -or $Beta -ge 1) {
        Write-Host "SPRT error rates must be in (0,1): alpha=$Alpha beta=$Beta." -ForegroundColor Red
        exit 1
    }
}

$GameDir  = Split-Path $PSScriptRoot -Parent
$RepoRoot = Split-Path $GameDir -Parent

# --- Resolve DepsRoot the same way Directory.Build.props does ---------------
# Worktree layout: <main-repo>\.claude\worktrees\<name>\ -> DepsRoot is 4 up.
# Main repo root: DepsRoot is the parent directory.
# (Directory.Build.user.props overrides are not parsed here; if you use one,
#  pass explicit paths or keep EngineTesting\ in the default location.)
$mainRepoSln = Join-Path (Split-Path (Split-Path (Split-Path $RepoRoot -Parent) -Parent) -Parent) 'StratChessEvolved.sln'
$DepsRoot = if (Test-Path $mainRepoSln) {
    Split-Path (Split-Path (Split-Path (Split-Path $RepoRoot -Parent) -Parent) -Parent) -Parent
} else {
    Split-Path $RepoRoot -Parent
}
$EngineTesting = Join-Path $DepsRoot 'EngineTesting'
$fastchess     = Join-Path $EngineTesting 'fastchess.exe'
$book          = Join-Path $RepoRoot 'Tests\openings\openings-250.pgn'
$refExe        = Join-Path $EngineTesting "StratChess-$ReferenceTag.exe"
if ($ReferenceExe -ne '') { $refExe = $ReferenceExe }

if ($CandidateExe -eq '') { $CandidateExe = Join-Path $RepoRoot 'x64\Release\StratChessEvolved.exe' }

# --- Preflight ---------------------------------------------------------------
if (-not (Test-Path $fastchess)) {
    Write-Host "MISSING: $fastchess" -ForegroundColor Red
    Write-Host 'One-time setup: download the fastchess Windows x64 release from'
    Write-Host '  https://github.com/Disservin/fastchess/releases'
    Write-Host "and place fastchess.exe in $EngineTesting (see Docs/EloLog.md for the pinned version)."
    exit 1
}

if ($ResumeDir -ne '') {
    # --- Resume mode: skip fresh match setup entirely; fastchess's -config
    # restores the original engine/tournament configuration from the saved
    # state (autosaved every -autosaveinterval games during the run that was
    # interrupted). -CandidateExe/-ReferenceTag/-ReferenceExe/-Games/-Tc/
    # -Concurrency/-CandidateOptions/-ReferenceOptions are all ignored here.
    $ResumeDir = (Resolve-Path $ResumeDir).Path
    $resumeConfig = Join-Path $ResumeDir 'config.json'
    if (-not (Test-Path $resumeConfig)) {
        Write-Host "MISSING: $resumeConfig -- $ResumeDir doesn't look like an interrupted match directory (no autosaved config.json)." -ForegroundColor Red
        exit 1
    }

    # config.json is a single flat file shared by every invocation (fastchess
    # always writes it to its cwd, which this script always Push-Location's
    # into $pgnDir -- there is no per-match subdirectory). The original run's
    # timestamp stamp is recovered from the pgn output path it recorded, not
    # from $ResumeDir's own name.
    $resumeState = Get-Content $resumeConfig -Raw | ConvertFrom-Json
    $stamp    = [System.IO.Path]::GetFileNameWithoutExtension($resumeState.pgn.file)
    $pgnDir   = $ResumeDir
    $pgnOut   = Join-Path $pgnDir "$stamp.pgn"
    $matchLog = Join-Path $pgnDir "$stamp.log"

    $candidateSha = (git -C $RepoRoot rev-parse --short HEAD).Trim()
    $dirty = (git -C $RepoRoot status --porcelain) ? '+dirty' : ''
    $candidateName = "candidate-$candidateSha$dirty"

    Write-Host "==> Resuming match in $ResumeDir" -ForegroundColor Cyan
    Write-Host "    PGN: $pgnOut"

    Push-Location $pgnDir
    & $fastchess `
        -config file=config.json `
        -recover `
        -autosaveinterval $AutosaveInterval `
        2>&1 | Tee-Object -FilePath $matchLog -Append

    $fcExit = $LASTEXITCODE
    Pop-Location
} else {
    if (-not (Test-Path $book)) {
        Write-Host "MISSING: $book (committed opening book — repo checkout incomplete?)" -ForegroundColor Red
        exit 1
    }
    if (-not (Test-Path $CandidateExe)) {
        Write-Host "MISSING candidate exe: $CandidateExe" -ForegroundColor Red
        Write-Host 'Build it first: .\build.ps1 main'
        exit 1
    }
    if ($ReferenceExe -ne '' -and -not (Test-Path $refExe)) {
        Write-Host "MISSING reference exe: $refExe" -ForegroundColor Red
        exit 1
    }

    # --- Ensure reference exe (rebuild from tag on cache miss; skipped entirely when -ReferenceExe is set) ---
    if ($ReferenceExe -eq '' -and -not (Test-Path $refExe)) {
        Write-Host "==> Reference exe not cached; rebuilding from tag '$ReferenceTag'" -ForegroundColor Cyan
        git -C $RepoRoot rev-parse --verify --quiet "refs/tags/$ReferenceTag" | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "Tag '$ReferenceTag' not found. git fetch origin --tags and retry." -ForegroundColor Red
            exit 1
        }
        # The temp worktree MUST live under <main-repo>\.claude\worktrees\ -- that is
        # the layout Directory.Build.props detects to resolve DepsRoot for worktree
        # builds; anywhere else and the reference build cannot find spdlog/json.
        $mainRoot = (git -C $RepoRoot rev-parse --path-format=absolute --git-common-dir) -replace '[\\/]\.git$', ''
        $tmpWt = Join-Path $mainRoot ".claude\worktrees\elo-ref-build-$PID"
        git -C $RepoRoot worktree add --detach $tmpWt "refs/tags/$ReferenceTag"
        if ($LASTEXITCODE -ne 0) { Write-Host 'worktree add failed' -ForegroundColor Red; exit 1 }
        try {
            Push-Location $tmpWt
            & (Join-Path $tmpWt 'build.ps1') main
            if ($LASTEXITCODE -ne 0) { throw "reference build failed" }
            Pop-Location
            New-Item -ItemType Directory -Force $EngineTesting | Out-Null
            Copy-Item (Join-Path $tmpWt 'x64\Release\StratChessEvolved.exe') $refExe
        } finally {
            if ((Get-Location).Path -eq $tmpWt) { Pop-Location }
            git -C $RepoRoot worktree remove --force $tmpWt
        }
        Write-Host "Reference cached: $refExe"
    }

    # --- Match -------------------------------------------------------------------
    if ($Smoke) { $Games = 20 }
    $rounds = [math]::Max(1, [int]($Games / 2))

    $candidateSha = (git -C $RepoRoot rev-parse --short HEAD).Trim()
    $dirty = (git -C $RepoRoot status --porcelain) ? '+dirty' : ''
    $candidateName = "candidate-$candidateSha$dirty"

    $stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
    $pgnDir  = Join-Path $GameDir 'logs\elo'
    New-Item -ItemType Directory -Force $pgnDir | Out-Null
    $pgnOut  = Join-Path $pgnDir "$stamp.pgn"
    $matchLog = Join-Path $pgnDir "$stamp.log"

    # Separate working dirs so per-engine incidental output (SimplePerfStats.txt)
    # never collides across concurrent games.
    $dirA = Join-Path $pgnDir "$stamp-cand"
    $dirB = Join-Path $pgnDir "$stamp-ref"
    New-Item -ItemType Directory -Force $dirA, $dirB | Out-Null

    # Report the RESOLVED bounds, not the preset name -- the log should record what
    # was actually tested, not which shorthand was typed.
    $gamesLabel = ($Sprt -ne '') ? "up to $Games games (SPRT cap)" : "$Games games"
    Write-Host "==> $candidateName vs $ReferenceTag | $gamesLabel, tc=$Tc, concurrency=$Concurrency" -ForegroundColor Cyan
    if ($Sprt -ne '') {
        Write-Host "    SPRT: $Sprt -- elo0=$Elo0 elo1=$Elo1 alpha=$Alpha beta=$Beta model=$SprtModel" -ForegroundColor Cyan
    }
    Write-Host "    PGN: $pgnOut"

    # Engine specs as arrays so -CandidateOptions/-ReferenceOptions (e.g. option.Threads=4)
    # splat in as extra fastchess "-engine" option tokens without disturbing the base spec.
    $candidateEngineArgs = @("cmd=$CandidateExe", "name=$candidateName", "dir=$dirA", 'args=uci')
    if ($CandidateOptions) { $candidateEngineArgs += $CandidateOptions -split '\s+' }
    $referenceEngineArgs = @("cmd=$refExe", "name=$ReferenceTag", "dir=$dirB", 'args=uci')
    if ($ReferenceOptions) { $referenceEngineArgs += $ReferenceOptions -split '\s+' }

    # SPRT tokens splatted the same way as the -engine option arrays above: empty
    # array = argument absent entirely, so the non-SPRT path is byte-identical to
    # what it was before this feature existed.
    $sprtArgs = @()
    if ($Sprt -ne '') {
        $sprtArgs = @('-sprt', "elo0=$Elo0", "elo1=$Elo1", "alpha=$Alpha", "beta=$Beta", "model=$SprtModel")
    }

    # Run from the artifacts dir: fastchess drops a config.json (tournament resume
    # state) into its cwd, which must land under gitignored logs/elo, not the repo.
    Push-Location $pgnDir
    & $fastchess `
        -engine @candidateEngineArgs `
        -engine @referenceEngineArgs `
        -each "tc=$Tc" `
        -rounds $rounds -repeat -concurrency $Concurrency -recover `
        -autosaveinterval $AutosaveInterval `
        @sprtArgs `
        -openings "file=$book" format=pgn order=sequential `
        -draw movenumber=40 movecount=8 score=10 `
        -resign movecount=4 score=800 `
        -pgnout "file=$pgnOut" notation=san `
        2>&1 | Tee-Object -FilePath $matchLog

    $fcExit = $LASTEXITCODE
    Pop-Location
}

# --- Parse + report ----------------------------------------------------------
# fastchess 1.8 result block: "Elo: -88.74 +/- 95.36, nElo: ..." and
# "Games: 20, Wins: 6, Losses: 11, Draws: 3, Points: 7.5 (37.50 %)"
#
# With -sprt the same block gains an LLR line, and a verdict line is printed once
# a bound is crossed. Both patterns below were derived from the actual output of
# the pinned fastchess 1.8.0 build (calibration run 20260726-175952), NOT written
# from the documentation -- the exact wording varies across fastchess versions and
# a pattern that silently matches nothing would degrade every run to
# "inconclusive", a failure mode that looks like a result:
#
#   LLR: -3.48 (-118.2%) (-2.94, 2.94) [0.00, 200.00]
#   SPRT ([0.00, 200.00]) completed - H0 was accepted
$log = Get-Content $matchLog -Raw
$eloLine   = ($log -split "`n" | Select-String -Pattern '^\s*Elo:' | Select-Object -Last 1)
$scoreLine = ($log -split "`n" | Select-String -Pattern 'Games: \d+' | Select-Object -Last 1)
$llrLine     = ($log -split "`n" | Select-String -Pattern '^\s*LLR:' | Select-Object -Last 1)
$verdictLine = ($log -split "`n" | Select-String -Pattern 'SPRT\s*\(.*\)\s*completed\s*-\s*(H0|H1) was accepted' | Select-Object -Last 1)

$disasters = ($log -split "`n" | Select-String -Pattern 'illegal|disconnect|stall|loses on time' )
$hardFail = $false
if ($fcExit -ne 0) { Write-Host "fastchess exited with code $fcExit" -ForegroundColor Red; $hardFail = $true }
if ($disasters) {
    Write-Host "`nHARNESS/ENGINE FAILURES DETECTED (not strength data):" -ForegroundColor Red
    $disasters | Select-Object -First 10 | ForEach-Object { Write-Host "  $($_.Line)" }
    $hardFail = $true
}

Write-Host "`n--- Result ---" -ForegroundColor Cyan
if ($scoreLine) { Write-Host $scoreLine.Line.Trim() }
if ($eloLine)   { Write-Host $eloLine.Line.Trim() }

# --- Resolve the SPRT verdict -------------------------------------------------
# Hitting the game cap without crossing a bound is a real outcome ("smaller than
# elo1, or we ran out of budget"), not a failure -- but it must never be rendered
# as though a decision was reached, so it is both recorded AND flagged loudly.
$sprtVerdict = ''
if ($Sprt -ne '') {
    if ($llrLine) { Write-Host $llrLine.Line.Trim() }
    if ($verdictLine -and ($verdictLine.Line -match '(H0|H1) was accepted')) {
        $accepted = $Matches[1]
        $sprtVerdict = "$accepted accepted"
        $meaning = ($accepted -eq 'H1') ? "candidate is stronger than elo1=$Elo1" : "candidate is no better than elo0=$Elo0"
        Write-Host "SPRT [$Elo0, $Elo1]: $sprtVerdict -- $meaning" -ForegroundColor Green
    } else {
        $sprtVerdict = "inconclusive @ $($null -ne $scoreLine ? 'see games column' : '?') games"
        Write-Host "SPRT [$Elo0, $Elo1]: INCONCLUSIVE -- hit the -Games cap without crossing a bound." -ForegroundColor Yellow
        Write-Host '  This is NOT a measurement of zero: the true difference is simply smaller than' -ForegroundColor Yellow
        Write-Host '  elo1, or the game budget ran out. Widen the bounds or raise -Games to decide.' -ForegroundColor Yellow
    }
}

# --- Append to Docs/EloLog.md -------------------------------------------------
$eloText = if ($eloLine) { ($eloLine.Line -replace '^\s*Elo:\s*', '' -replace ',\s*nElo.*$', '').Trim() } else { 'n/a' }
$kind = $Smoke ? 'smoke' : 'match'
$actualGames = $Games
if ($scoreLine -and ($scoreLine.Line -match 'Games:\s*(\d+)')) { $actualGames = $Matches[1] }
if ($Sprt -ne '') {
    # Re-render now that $actualGames is known, so an inconclusive row states the
    # game count it gave up at rather than a placeholder.
    if ($sprtVerdict -like 'inconclusive*') { $sprtVerdict = "inconclusive @ $actualGames games" }
    $kind = "SPRT $Sprt [$Elo0, $Elo1] — $sprtVerdict"
    # Sanity check: an accepted H1 alongside a negative point estimate means the
    # parse latched onto the wrong line. Better a loud warning than a wrong row.
    if ($sprtVerdict -eq 'H1 accepted' -and $eloText -match '^\s*-') {
        Write-Host "WARNING: H1 accepted but the Elo estimate is negative ($eloText) — verdict parse is suspect." -ForegroundColor Red
    }
}
$row = "| $(Get-Date -Format 'yyyy-MM-dd') | $candidateName | $ReferenceTag | $actualGames | $Tc | $eloText | $kind$($hardFail ? ' — FAILURES, discard' : '') |"
$eloLog = Join-Path $RepoRoot 'Docs\EloLog.md'
if (Test-Path $eloLog) {
    Add-Content -Path $eloLog -Value $row
    Write-Host "Recorded in Docs/EloLog.md"
} else {
    Write-Host "Docs/EloLog.md not found — record manually: $row" -ForegroundColor Yellow
}

if ($hardFail) { exit 1 }
Write-Host "`nELO match complete." -ForegroundColor Green
exit 0
