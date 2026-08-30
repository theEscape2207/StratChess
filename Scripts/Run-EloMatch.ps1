<#
.SYNOPSIS
    ELO match: measure a candidate build against the pinned reference build.

.DESCRIPTION
    Runs a fastchess match (color-swapped opening pairs, draw/resign adjudication)
    between a candidate StratChessEvolved.exe and the pinned reference build, then
    reports the ELO difference with its error bound and appends a record line to
    Docs/EloLog.md.

    The reference exe is cached in EngineTesting\ beside the main checkout and is
    rebuilt on demand from its git tag via a temporary worktree, so the procedure
    survives a wiped cache. The candidate is NOT built by this script — build it
    first (.\build.ps1 main), and note that build.ps1 defaults to the shipping
    clang-cl build, which is the only one comparable against the reference.

.WHEN TO USE
    After any change that can affect playing strength (search, evaluation, move
    ordering, time management) — tactical suites verify correctness only.
    Setup and interpretation guide: Docs/EloMeasurement.md. Past results:
    Docs/EloLog.md.

.HOW TO INVOKE (from bash, cmd, or PowerShell)
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\Run-EloMatch.ps1
    Smoke run (pipeline check, 20 games):
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\Run-EloMatch.ps1 -Smoke
    SPRT (stops as soon as the result is decisive; -Games becomes an upper bound).
    Needs a reference that isolates the change, so build the merge base first:
    pwsh ... Run-EloMatch.ps1 -Sprt NonRegression -ReferenceExe <mb.exe> -ReferenceTag <commit>
    pwsh ... Run-EloMatch.ps1 -Sprt Gain -ReferenceExe <mb.exe> -ReferenceTag <commit>

.NOTES
    Must be invoked with -File, not dot-sourced -- a dot-sourced script runs in the
    caller's scope, where its variables collide and its exit ends the caller's session.
    Losses on illegal moves / disconnects / stalls are harness or engine BUGS, not
    strength data — the script exits 1 when fastchess reports any.
    Every run appends a row to Docs/EloLog.md; there is no way to suppress that.
    Runs whose two sides are the same binary AND the same options are detected and
    the row says so, since their true difference is zero by construction. Same
    binary with DIFFERENT options is a configuration comparison and a real
    measurement — it is recorded as one.
    A fixed 500-game batch resolves only ±25 Elo on this setup, so anything expected
    to be worth less than that needs -Sprt to be decidable at all. See the
    "Choosing SPRT vs a fixed batch" section in Docs/EloMeasurement.md.
    -Sprt is refused against a tag-resolved reference: a fixed anchor turns the
    hypothesis into one about cumulative standing rather than about the change.
    Pass -AnchorSprt when that cumulative verdict is what is actually wanted.
#>

param(
    # Candidate exe. Default: this repo's Release build (must already exist).
    [string]$CandidateExe = '',
    # Git tag of the pinned reference build. v2 is clang-cl/CMake built, matching
    # what ships, so day-to-day measurements compare like with like. Pass
    # -ReferenceTag elo-reference-v1 for the long-run epic comparison; that binary
    # is MSVC-built, so the delta includes the compiler change (see Docs/EloMeasurement.md).
    [string]$ReferenceTag = 'elo-reference-v2',
    # Explicit path to a reference exe. When set, skips the tag-based cache/rebuild
    # lookup entirely and uses this exe directly as the reference side. Pass
    # -ReferenceTag alongside it (e.g. a commit sha) to label the row; left at
    # its default, the row is labelled from the exe's own filename instead of
    # the misleading default tag. Use to compare two configurations of the SAME
    # binary (e.g. threads=4 vs threads=1), or a merge-base build against HEAD.
    [string]$ReferenceExe = '',
    # Total games (2 games per opening pair). Default resolves ≈ ±25 Elo at 95%
    # -- the figure every 500-game row in Docs/EloLog.md actually came back with
    # (±25.70, ±27.62, ±28.36), and the one the .NOTES block above quotes. Under
    # -Sprt this is only an upper bound, not a resolution target; see
    # "Adjusting -Games" in Docs/EloMeasurement.md before changing it.
    [int]$Games = 500,
    # Opening book. Empty auto-resolves: a large book in EngineTesting\ if one is
    # present, otherwise the committed 250-position smoke book. Accepts .pgn or
    # .epd -- the format flag passed to fastchess follows the extension.
    #
    # Large books are NOT committed: they are third-party data of varying
    # provenance, and this repository is public. They live beside the checkout
    # with fastchess and the reference binaries, which is where every other
    # external test asset already lives.
    [string]$Book = '',
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
    # a fixed batch simply cannot resolve them (see Docs/EloMeasurement.md).
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
    # the same scale Docs/EloLog.md records everywhere. fastchess's own default is
    # 'normalized' (nElo) -- a DIFFERENT scale, on which "elo1=10" would silently
    # mean something else entirely. Override only if you know which scale you want.
    [ValidateSet('logistic', 'normalized', 'bayesian')]
    [string]$SprtModel = 'logistic',
    # Opt out of the fixed-anchor SPRT guard below, declaring that a verdict about
    # cumulative standing is the one wanted. The EloLog row is labelled as such, so
    # a deliberate anchor SPRT stays distinguishable from a per-change one.
    [switch]$AnchorSprt
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
    # A tag-resolved reference is a FIXED anchor, so the hypothesis under test
    # becomes "main plus this change beats the anchor by more than elo1" -- a
    # statement about the sum, which the margin main had already accumulated can
    # satisfy on its own. No per-change reading of such a verdict exists, and none
    # can be recovered by differencing anchor rows, so the combination is refused
    # rather than warned about. An explicit -ReferenceExe is the per-change path:
    # it points at a build of the merge base, which the tag lookup cannot reach.
    if ($ReferenceExe -eq '' -and -not $AnchorSprt) {
        Write-Host "-Sprt against the fixed anchor '$ReferenceTag' tests the SUM, not this change." -ForegroundColor Red
        Write-Host "H1 would mean 'main plus this change beats the anchor by more than elo1', which main's" -ForegroundColor Red
        Write-Host 'pre-existing margin can satisfy alone -- the verdict would describe the wrong quantity.' -ForegroundColor Red
        Write-Host ''
        Write-Host 'To measure THIS change:' -ForegroundColor Yellow
        Write-Host '  - dispatch .github/workflows/strength.yml with reference_ref=merge-base (resolves +/-4 Elo), or' -ForegroundColor Yellow
        Write-Host '  - build the merge base and pass -ReferenceExe <exe> -ReferenceTag <commit> here.' -ForegroundColor Yellow
        Write-Host 'For a deliberate cumulative reading, re-run with -AnchorSprt; the EloLog row is labelled as one.' -ForegroundColor Yellow
        Write-Host 'Background: Docs/EloMeasurement.md -- "The anchor measures the sum, not your change".' -ForegroundColor Yellow
        exit 1
    }
}

$RepoRoot = Split-Path $PSScriptRoot -Parent
$GameDir  = Join-Path $RepoRoot 'StratChessEvolved'

# --- Locate EngineTesting\ ---------------------------------------------------
# fastchess and the cached reference binaries live beside the MAIN checkout, so
# every worktree shares them instead of re-downloading per branch.
#
# The main checkout is resolved through git rather than by probing for a marker
# file: a worktree's .git is a file, and --git-common-dir always points at the
# main repository's .git regardless of how deeply the worktree is nested.
$mainRoot      = (git -C $RepoRoot rev-parse --path-format=absolute --git-common-dir) -replace '[\\/]\.git[\\/]?$', ''
$DepsRoot      = Split-Path $mainRoot -Parent
$EngineTesting = Join-Path $DepsRoot 'EngineTesting'
$fastchess     = Join-Path $EngineTesting 'fastchess.exe'
$smokeBook     = Join-Path $RepoRoot 'Tests\openings\openings-250.pgn'

# --- Resolve the opening book ------------------------------------------------
# Explicit -Book wins. Otherwise prefer a large book dropped into EngineTesting\
# (any openings-large.* file), falling back to the committed smoke book.
if ($Book -ne '') {
    $book = $Book
} else {
    $largeBook = Get-ChildItem -Path $EngineTesting -Filter 'openings-large.*' -File `
                     -ErrorAction SilentlyContinue |
                 Sort-Object Name | Select-Object -First 1
    $book = if ($largeBook) { $largeBook.FullName } else { $smokeBook }
}

$bookFormat = if ([System.IO.Path]::GetExtension($book).ToLowerInvariant() -eq '.epd') { 'epd' }
              else { 'pgn' }

# Counts openings so the run can say whether it will exhaust the book. A PGN
# opening is one [Event tag; an EPD opening is one non-blank line.
function Get-OpeningCount([string]$path, [string]$format) {
    if (-not (Test-Path $path)) { return 0 }
    if ($format -eq 'epd') {
        return @(Get-Content $path | Where-Object { $_.Trim() -ne '' }).Count
    }
    return @(Select-String -Path $path -Pattern '^\[Event' -AllMatches).Count
}
$refExe        = Join-Path $EngineTesting "StratChess-$ReferenceTag.exe"
if ($ReferenceExe -ne '') { $refExe = $ReferenceExe }

# What names the reference side in fastchess's engine list, the console banner
# and the EloLog.md row. -ReferenceTag names it correctly by construction when
# it drove the tag-based cache/rebuild lookup above. Pointing -ReferenceExe at
# an arbitrary binary while leaving -ReferenceTag at its default would still
# read as the pinned anchor, so it falls back to the exe's own basename instead
# -- an explicit -ReferenceTag passed alongside -ReferenceExe (e.g. a commit
# sha, as in the SPRT examples above) still labels it, since that pairing is
# the documented way to say what the exe actually is.
$refName = $ReferenceTag
if ($ReferenceExe -ne '' -and -not $PSBoundParameters.ContainsKey('ReferenceTag')) {
    $refName = "ref-$([IO.Path]::GetFileNameWithoutExtension($ReferenceExe))"
}

# Defaults to the shipping (clang-cl) build deliberately. Elo is only comparable
# between binaries from the same compiler: an MSVC candidate measured against the
# clang reference would show the ~25% nps compiler gap as a phantom regression.
# -AllowMissing: -Resume needs no candidate build, and the non-resume path does
# its own Test-Path below with build instructions.
if ($CandidateExe -eq '') {
    $CandidateExe = & (Join-Path $PSScriptRoot 'Get-BuildArtifact.ps1') -AllowMissing
}

# --- Preflight ---------------------------------------------------------------
if (-not (Test-Path $fastchess)) {
    Write-Host "MISSING: $fastchess" -ForegroundColor Red
    Write-Host 'One-time setup: download the fastchess Windows x64 release from'
    Write-Host '  https://github.com/Disservin/fastchess/releases'
    Write-Host "and place fastchess.exe in $EngineTesting (see Docs/EloMeasurement.md for the pinned version)."
    exit 1
}

# Set by the fresh-match path below once both exes are known to exist. Resume mode
# ignores the exe parameters entirely (fastchess restores the original pairing from
# config.json), so it cannot decide this and leaves the row unannotated.
$noStrengthData = $false

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

    # Label the EloLog row from the RESTORED state, never from this invocation's
    # parameters. -ReferenceTag/-ReferenceExe/-Sprt are all ignored when resuming, so
    # leaving them at their defaults records the pinned anchor as the opponent and drops
    # the SPRT verdict entirely -- a row that reads as a plausible anchor comparison for
    # a match that was neither (#388).
    $resumedEngines = @($resumeState.engines)
    if ($resumedEngines.Count -ge 2 -and $resumedEngines[1].name) {
        $refName = $resumedEngines[1].name
    }
    # Likewise the candidate. Recomputing it from HEAD names whatever the tree happens to
    # be on now, which after any commit is a revision that never played a game -- while
    # fastchess relaunches the binary the saved path points at, not this one.
    if ($resumedEngines.Count -ge 1 -and $resumedEngines[0].name) {
        $candidateName = $resumedEngines[0].name
    }

    if ($resumeState.sprt -and $resumeState.sprt.enabled) {
        $Elo0 = [int]$resumeState.sprt.elo0
        $Elo1 = [int]$resumeState.sprt.elo1
        # Recover the preset name from the bounds, so the row reads the way the original
        # invocation would have written it. Anything else was -Sprt Custom by definition.
        $Sprt = if ($Elo0 -eq -5 -and $Elo1 -eq 0) { 'NonRegression' }
                elseif ($Elo0 -eq 0 -and $Elo1 -eq 10) { 'Gain' }
                else { 'Custom' }

        # -AnchorSprt is an intent, not state fastchess saves. An SPRT whose reference is
        # the pinned anchor can only have been asked for with it -- the guard below refuses
        # that pairing otherwise -- so infer it rather than silently dropping the
        # "cumulative standing" caveat the row would otherwise carry.
        if ($refName -eq $ReferenceTag) { $AnchorSprt = $true }
    }

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
        if ($book -eq $smokeBook) {
            Write-Host "MISSING: $book (committed opening book — repo checkout incomplete?)" -ForegroundColor Red
        } else {
            Write-Host "MISSING: $book" -ForegroundColor Red
            Write-Host 'Drop a book at EngineTesting\openings-large.pgn (or .epd), or pass -Book <path>.'
        }
        exit 1
    }
    # Resolve to absolute now that existence is confirmed: fastchess is launched
    # from an isolated per-engine directory ($dirA/$dirB below), so a relative
    # path here would re-resolve under that directory and fail to launch even
    # though the check above just passed against the caller's cwd (#268).
    $book = (Resolve-Path $book).Path

    # An opening pair is two games, so N openings yield 2N distinct games. Past
    # that fastchess wraps and replays them, which narrows the error bars of a
    # result without adding information to it.
    $openingCount = Get-OpeningCount $book $bookFormat
    $distinctGames = 2 * $openingCount
    Write-Host ("Opening book : {0} ({1} openings, format={2})" -f $book, $openingCount, $bookFormat)
    if ($openingCount -gt 0 -and $Games -gt $distinctGames) {
        Write-Host ("WARNING: {0} games requested but the book yields only {1} distinct games." -f $Games, $distinctGames) -ForegroundColor Yellow
        Write-Host '         Openings will repeat; the extra games tighten the error bar without adding information.' -ForegroundColor Yellow
        Write-Host '         Use a larger book (EngineTesting\openings-large.pgn|.epd) for batches this size.' -ForegroundColor Yellow
    }
    if (-not (Test-Path $CandidateExe)) {
        Write-Host "MISSING candidate exe: $CandidateExe" -ForegroundColor Red
        Write-Host 'Build it first: .\build.ps1 main'
        exit 1
    }
    $CandidateExe = (Resolve-Path $CandidateExe).Path
    if ($ReferenceExe -ne '' -and -not (Test-Path $refExe)) {
        Write-Host "MISSING reference exe: $refExe" -ForegroundColor Red
        exit 1
    }
    if ($ReferenceExe -ne '') { $refExe = (Resolve-Path $refExe).Path }

    # --- Ensure reference exe (rebuild from tag on cache miss; skipped entirely when -ReferenceExe is set) ---
    if ($ReferenceExe -eq '' -and -not (Test-Path $refExe)) {
        Write-Host "==> Reference exe not cached; rebuilding from tag '$ReferenceTag'" -ForegroundColor Cyan
        git -C $RepoRoot rev-parse --verify --quiet "refs/tags/$ReferenceTag" | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "Tag '$ReferenceTag' not found. git fetch origin --tags and retry." -ForegroundColor Red
            exit 1
        }
        # The temp worktree MUST live under <main-repo>\.claude\worktrees\ when the
        # reference tag predates the CMake migration: that is the layout the
        # Directory.Build.props of those tags detects to resolve DepsRoot, and
        # anywhere else the reference build cannot find spdlog/json. Tags after the
        # migration fetch their own dependencies and do not care.
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

            # A reference tag built before the CMake migration is MSVC-compiled and
            # lands in x64\Release; one built after is clang-cl and lands in
            # build\<preset>\. Accept either so historical references stay
            # reproducible, but say something when the compilers differ: that gap
            # alone is worth roughly +40 Elo at 10+0.1 (issue #84), and it would be
            # credited to whatever change is under test.
            if (-not (Test-Path (Join-Path $tmpWt 'CMakePresets.json'))) {
                Write-Host "WARNING: reference tag '$ReferenceTag' predates the CMake migration." -ForegroundColor Yellow
                Write-Host "         Its binary is MSVC-built while the candidate is clang-cl. The compiler" -ForegroundColor Yellow
                Write-Host "         difference alone is worth roughly +40 Elo (#84) and will be attributed" -ForegroundColor Yellow
                Write-Host "         to the change under test. Re-pin the reference before trusting this." -ForegroundColor Yellow
            }

            $builtExe = @(
                (Join-Path $tmpWt 'build\windows-clang-cl\StratChessEvolved.exe'),
                (Join-Path $tmpWt 'x64\Release\StratChessEvolved.exe')
            ) | Where-Object { Test-Path $_ } | Select-Object -First 1
            if (-not $builtExe) { throw "reference build produced no binary in build\windows-clang-cl\ or x64\Release\" }
            Copy-Item $builtExe $refExe
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

    # A run whose two sides are the same bytes AND the same configuration has a true
    # difference of exactly zero by construction: pipeline checks, time-forfeit
    # checks at a given control, reference re-pin verification. Its Elo column is
    # noise, and a row labelled plain 'match' invites reading it as a result.
    #
    # Detected rather than switched on deliberately. A -NoLog style flag is one that
    # can be forgotten (leaving a junk row, and a dirty tree that mislabels the NEXT
    # run '+dirty'), and one that could be reached for after seeing a number you did
    # not like. Detection is neither.
    #
    # Differing options are NOT this case. Pointing -ReferenceExe at the same binary
    # is the documented way to compare two CONFIGURATIONS -- threads=4 vs threads=1 --
    # and that is a real measurement: Docs/EloLog.md's Lazy SMP row (+128.55 Elo) is
    # one. Comparison is order-sensitive, so a reordered but equivalent option string
    # reads as a measurement; that is the safe direction to err, since mislabelling
    # genuine data as carrying none is the worse mistake.
    $sameBinary = (Get-FileHash $CandidateExe).Hash -eq (Get-FileHash $refExe).Hash
    $sameConfig = $CandidateOptions.Trim() -eq $ReferenceOptions.Trim()
    $noStrengthData = $sameBinary -and $sameConfig

    $stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
    $pgnDir  = Join-Path $GameDir 'logs\elo'
    New-Item -ItemType Directory -Force $pgnDir | Out-Null
    $pgnOut  = Join-Path $pgnDir "$stamp.pgn"
    $matchLog = Join-Path $pgnDir "$stamp.log"

    # Separate working dirs so per-engine incidental output never collides across concurrent
    # games. (SimplePerfStats.txt was the original motivation; since issue #135 only Game::Init
    # creates that logger, so UCI engines no longer emit it -- the isolation is still worth
    # keeping for logs/ and any future per-process output.)
    $dirA = Join-Path $pgnDir "$stamp-cand"
    $dirB = Join-Path $pgnDir "$stamp-ref"
    New-Item -ItemType Directory -Force $dirA, $dirB | Out-Null

    # Report the RESOLVED bounds, not the preset name -- the log should record what
    # was actually tested, not which shorthand was typed.
    $gamesLabel = ($Sprt -ne '') ? "up to $Games games (SPRT cap)" : "$Games games"
    Write-Host "==> $candidateName vs $refName | $gamesLabel, tc=$Tc, concurrency=$Concurrency" -ForegroundColor Cyan
    if ($Sprt -ne '') {
        Write-Host "    SPRT: $Sprt -- elo0=$Elo0 elo1=$Elo1 alpha=$Alpha beta=$Beta model=$SprtModel" -ForegroundColor Cyan
    }
    if ($noStrengthData) {
        Write-Host '    NOTE: both sides are the same binary and configuration, so the true difference' -ForegroundColor Yellow
        Write-Host '          is zero by construction. The Elo column will be noise; the row says so.' -ForegroundColor Yellow
    }
    Write-Host "    PGN: $pgnOut"

    # Engine specs as arrays so -CandidateOptions/-ReferenceOptions (e.g. option.Threads=4)
    # splat in as extra fastchess "-engine" option tokens without disturbing the base spec.
    $candidateEngineArgs = @("cmd=$CandidateExe", "name=$candidateName", "dir=$dirA", 'args=uci')
    if ($CandidateOptions) { $candidateEngineArgs += $CandidateOptions -split '\s+' }
    $referenceEngineArgs = @("cmd=$refExe", "name=$refName", "dir=$dirB", 'args=uci')
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
        -openings "file=$book" format=$bookFormat order=sequential `
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

# 'illegal' alone also matches "Warning; Illegal PV move" -- fastchess's own
# report that an engine's reported PV contained an illegal move, which it
# tolerates and plays through from bestmove (see the Reference-column note
# above; this is the reporting defect fastchess 1.8 emits on both engines
# roughly once a game and is not a played illegal move). The negative lookahead
# excludes only that phrasing, so an actual played illegal move -- any other
# wording containing 'illegal' -- still trips the guard.
$disasters = ($log -split "`n" | Select-String -Pattern 'illegal(?!\s+pv\s+move)|disconnect|stall|loses on time' )
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
    # The guard above only lets an anchor SPRT through when -AnchorSprt asked for
    # one, so say in the row which quantity the verdict is about; a reader months
    # later has no other way to tell the two apart.
    if ($AnchorSprt) {
        $kind = "$kind — **against a fixed anchor: cumulative standing, not the value of this change**"
    }
    # Sanity check: an accepted H1 alongside a negative point estimate means the
    # parse latched onto the wrong line. Better a loud warning than a wrong row.
    if ($sprtVerdict -eq 'H1 accepted' -and $eloText -match '^\s*-') {
        Write-Host "WARNING: H1 accepted but the Elo estimate is negative ($eloText) — verdict parse is suspect." -ForegroundColor Red
    }
}
# A resumed row is assembled from two processes' output: the figures come from the
# final fastchess summary (which covers every game, replayed ones included), but the
# wall time printed above covers only the resuming half.
if ($ResumeDir -ne '') {
    $kind = "$kind — resumed from an interrupted run; wall time covers the resumed portion only"
}

# Applied last so it leads the cell whatever the run was -- an SPRT between two
# identical binaries is every bit as uninformative as a fixed batch between them.
if ($noStrengthData) {
    $kind = "**same binary and configuration — carries no strength information.** $kind"
}
$row = "| $(Get-Date -Format 'yyyy-MM-dd') | $candidateName | $refName | $actualGames | $Tc | $eloText | $kind$($hardFail ? ' — FAILURES, discard' : '') |"
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
