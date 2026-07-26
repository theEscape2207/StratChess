# ELO Match Resume Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix issue #119 (prerequisite ahead of every other item in epic #110) — add a `-ResumeDir`
parameter to `Scripts\Run-EloMatch.ps1` so a match killed mid-run (e.g. by a background-task
duration cap, as happened during #70's validation — 491/500 games lost most of an hour) can
continue from fastchess's own autosaved checkpoint instead of restarting the full batch.

**Architecture:** fastchess already autosaves tournament state (`config.json`) into its working
directory and supports reloading it via `-config file=...`. The wrapping script currently never
passes `-config`, and generates a fresh timestamped directory on every invocation, so that
checkpoint is written but never used. This plan adds a resume branch to the script that, when
`-ResumeDir <path>` points at an existing `logs\elo\<stamp>` directory, skips all fresh-match setup
(candidate/reference resolution, opening book, adjudication rules, engine specs) and re-invokes
fastchess with just `-config file=config.json`, relying on fastchess itself to restore everything
and continue from the checkpoint.

**Tech Stack:** PowerShell 7 (`pwsh`), fastchess CLI (Windows x64, pinned per `Docs/EloLog.md`).

## Global Constraints

- Must be invoked with `-File`, not dot-sourced (`$PSScriptRoot` is `$null` under dot-source) —
  matches every other script in `StratChessEvolved/Scripts/`.
- Keep `Set-StrictMode -Version Latest` and `$ErrorActionPreference = 'Stop'` intact — don't
  introduce a code path that references an undefined variable under strict mode.
- This is `category:infra` work, not an eval change — no `eval-reviewer`/`search-reviewer` dispatch
  needed for this PR.
- This project's PowerShell-from-Bash-tool quirk: the Bash tool runs Git Bash, not PS7 — write any
  non-trivial PowerShell logic to the `.ps1` file directly and invoke with
  `pwsh -ExecutionPolicy Bypass -File`, never inline complex PS7 syntax into a bash command.
- No unit-test framework covers these `.ps1` scripts — validation is procedural (run it, observe
  the actual behavior), matching how the script's existing `-Smoke` mode is meant to be used.

## Design Decisions

- **`-config file=config.json` is expected to restore the full original tournament setup**
  (engine commands, time control, adjudication rules, opening book position) per fastchess's own
  `-help` text ("Load engine configurations to resume games from previous sessions"). Task 1's
  validation step confirms this empirically — do not assume the exact restored fields without
  checking; the fastchess binary is at `<DepsRoot>EngineTesting\fastchess.exe` and its `-help`
  output already confirms the `-config`/`-autosaveinterval`/`-recover` flags exist as described
  above (verified in this session), but the plan is scoped to require re-verifying the *end-to-end
  resume behavior itself* (does it actually skip completed games?) since that's the property that
  matters and can't be inferred from `-help` text alone.
- **`-CandidateExe`/`-ReferenceTag`/`-ReferenceExe`/`-Games`/`-Tc`/`-CandidateOptions`/
  `-ReferenceOptions` are all ignored in resume mode** — deliberately not re-validated or
  re-resolved, since `-config` is expected to already carry the real engine paths/settings from the
  interrupted run. This keeps the resume path minimal and avoids subtle mismatches (e.g. a
  re-specified `-CandidateExe` that doesn't match what `-config` restores).
- **`-autosaveinterval` becomes a script parameter (default: fastchess's own default, 20)**, and is
  passed on *every* invocation (fresh or resume) — this makes the checkpoint interval visible and
  tunable, and lets Task 1's validation test use a small value (e.g. `2`) to reliably produce a
  checkpoint before deliberately killing a short test match, without waiting through most of a real
  20+ game run.
- **EloLog.md's "Games" column is derived from fastchess's own final `Games: N` summary line**
  (already captured by the existing `$scoreLine` regex), not from the `-Games` parameter — the
  `-Games` param is meaningless in resume mode (ignored) and was already only an *intended* count in
  fresh-match mode (a hard-failed match could technically report fewer). This is a small,
  self-contained correctness fix that both modes benefit from.
- **`Tc` in the EloLog row is not independently re-verified in resume mode** — it still reflects
  whatever `-Tc` default/param was passed to the resume invocation, not fastchess's actual restored
  time control. Documented as a known limitation (pass `-Tc` explicitly matching the original run
  when resuming, for an accurate row) rather than solved — fastchess's summary output doesn't expose
  the active time control in a line this script already parses, and parsing `config.json` directly
  for it is unnecessary complexity for a cosmetic column.

## Empirical Correction (found during Task 1 Step 4 validation)

`$pgnDir` was never a per-match stamped subdirectory -- it has always been the single flat
`logs\elo\` directory (files/subdirs within it are stamp-*prefixed*, e.g. `$stamp.pgn`,
`$stamp-cand\`, but the directory itself is shared). This means fastchess's autosaved
`config.json` is a **single shared file**, overwritten by every invocation (fresh or resume) since
they all `Push-Location` into the same `$pgnDir`. Consequence: **resume promptly, before starting
any other `Run-EloMatch.ps1` invocation** -- a fresh run silently clobbers an interrupted match's
`config.json` before it can be resumed. The stamp needed to keep writing to the *same* `.pgn`/`.log`
as the original interrupted run is recovered by parsing `config.json`'s own recorded
`pgn.file` path (`ConvertFrom-Json` + `GetFileNameWithoutExtension`), not from `$ResumeDir`'s
directory name as originally planned above -- confirmed via an actual kill-and-resume test (2
games completed, killed mid-game-3, resumed via `-ResumeDir StratChessEvolved\logs\elo`, correctly
picked up at game 3, finished at exactly 20/20 with no duplication). This correction is reflected
in the code blocks above.

## Files Changed

- Modify: `StratChessEvolved/Scripts/Run-EloMatch.ps1` — add `-ResumeDir`/`-AutosaveInterval`
  parameters, a resume branch, and the Games-column fix.
- Modify: `Docs/EloLog.md` — document the resume workflow.
- Modify: `Docs/Changelog.md` — one-line entry once validated.

## Step-by-Step Changes

### Task 1: Implement and validate the resume mechanism

**Files:**
- Modify: `StratChessEvolved/Scripts/Run-EloMatch.ps1`

**Interfaces:**
- Consumes: fastchess CLI flags `-config file=NAME`, `-autosaveinterval N`, `-recover` (all
  pre-existing fastchess features, confirmed present via `fastchess -help` in this session — no
  fastchess version change needed).
- Produces: new script parameters `-ResumeDir <path>` and `-AutosaveInterval <int>`, callable via
  `pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-EloMatch.ps1 -ResumeDir <path>`.

- [x] **Step 1: Add the two new parameters**

In `StratChessEvolved/Scripts/Run-EloMatch.ps1`, in the `param(...)` block, change:

```powershell
    # 20-game pipeline check instead of a full measurement.
    [switch]$Smoke
)
```

to:

```powershell
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
    [int]$AutosaveInterval = 20
)
```

- [x] **Step 2: Restructure the preflight + match-setup + fastchess-invocation section**

Replace the entire block starting at `# --- Preflight ---------------------------------------------------------------`
and ending right before `# --- Parse + report ----------------------------------------------------------`
with:

```powershell
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

    $dirA = Join-Path $pgnDir "$stamp-cand"
    $dirB = Join-Path $pgnDir "$stamp-ref"
    New-Item -ItemType Directory -Force $dirA, $dirB | Out-Null

    Write-Host "==> $candidateName vs $ReferenceTag | $Games games, tc=$Tc, concurrency=$Concurrency" -ForegroundColor Cyan
    Write-Host "    PGN: $pgnOut"

    $candidateEngineArgs = @("cmd=$CandidateExe", "name=$candidateName", "dir=$dirA", 'args=uci')
    if ($CandidateOptions) { $candidateEngineArgs += $CandidateOptions -split '\s+' }
    $referenceEngineArgs = @("cmd=$refExe", "name=$ReferenceTag", "dir=$dirB", 'args=uci')
    if ($ReferenceOptions) { $referenceEngineArgs += $ReferenceOptions -split '\s+' }

    # Run from the artifacts dir: fastchess drops a config.json (tournament resume
    # state) into its cwd, which must land under gitignored logs/elo, not the repo.
    Push-Location $pgnDir
    & $fastchess `
        -engine @candidateEngineArgs `
        -engine @referenceEngineArgs `
        -each "tc=$Tc" `
        -rounds $rounds -repeat -concurrency $Concurrency -recover `
        -autosaveinterval $AutosaveInterval `
        -openings "file=$book" format=pgn order=sequential `
        -draw movenumber=40 movecount=8 score=10 `
        -resign movecount=4 score=800 `
        -pgnout "file=$pgnOut" notation=san `
        2>&1 | Tee-Object -FilePath $matchLog

    $fcExit = $LASTEXITCODE
    Pop-Location
}
```

- [x] **Step 3: Fix the EloLog.md row's Games column to reflect the real completed count**

In the `# --- Parse + report ---` section (unchanged in location, still runs for both modes), find:

```powershell
$row = "| $(Get-Date -Format 'yyyy-MM-dd') | $candidateName | $ReferenceTag | $Games | $Tc | $eloText | $kind$($hardFail ? ' — FAILURES, discard' : '') |"
```

Change it to derive the game count from the already-parsed `$scoreLine` instead of the raw `$Games`
parameter (which is meaningless in resume mode and only ever an *intended* count in fresh-match
mode). Add this just above the `$row = ...` line:

```powershell
$actualGames = $Games
if ($scoreLine -and ($scoreLine.Line -match 'Games:\s*(\d+)')) { $actualGames = $Matches[1] }
```

and change the `$row` line to use `$actualGames` instead of `$Games`:

```powershell
$row = "| $(Get-Date -Format 'yyyy-MM-dd') | $candidateName | $ReferenceTag | $actualGames | $Tc | $eloText | $kind$($hardFail ? ' — FAILURES, discard' : '') |"
```

- [x] **Step 4: Validate the resume mechanism end-to-end**

This is a PowerShell script with no unit-test framework — validate procedurally, by deliberately
interrupting a small match and confirming resume actually continues it rather than restarting.

From the `StratChessEvolved/` directory, build the candidate first if not already built
(`.\build.ps1 main`), then start a small, fast, checkpoint-heavy test match in the background:

```
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-EloMatch.ps1 -Games 10 -AutosaveInterval 2"
```

Let it run until the match log (`StratChessEvolved\logs\elo\<new-stamp>\<new-stamp>.log`) shows at
least 2-3 `Finished game` lines (confirms at least one autosave checkpoint has been written — check
for `config.json` in that same directory), then kill the fastchess process
(`Stop-Process -Name fastchess -Force`) to simulate the harness-kill failure mode. Confirm:
- `config.json` exists in the match directory.
- The match log shows fewer than 10 finished games (proving it was genuinely interrupted, not
  already complete).

Then resume it:

```
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-EloMatch.ps1 -ResumeDir StratChessEvolved\logs\elo\<that-stamp> -AutosaveInterval 2"
```

Confirm:
- The resumed run does **not** start from game 1 (check the match log's `Started game N of 10`
  lines pick up from where the kill happened, not from `1 of 10` again).
- The match completes to `10 of 10` and fastchess exits 0 with a final `Elo:`/`Games: 10` summary.
- `Docs/EloLog.md` gets exactly one new row for this test, with `actualGames` correctly showing
  `10` (not the ignored `-Games` default of 500, and not a partial count).

If `-config file=config.json` alone doesn't produce a working resume (e.g. fastchess errors
demanding engine/tournament flags again), that's real information the empirical test is meant to
surface — adapt the resume branch to also re-pass the original `-engine`/`-each`/`-rounds`
`-repeat`/`-openings`/`-draw`/`-resign`/`-pgnout` flags (using the same values the fresh-match
branch would compute) alongside `-config file=config.json`, and re-run the same validation steps
until they pass. Do not guess this — the point of Step 4 is to observe fastchess's actual behavior
and adjust the implementation to match, not to assume the minimal form works.

Remove the test match directories created during this validation
(`StratChessEvolved\logs\elo\<test-stamps>*`) before committing — they're gitignored anyway, but
keep the working tree tidy. Remove the corresponding test rows this generates from `Docs/EloLog.md`
too if it's the tracked file (the whole point of this validation is a throwaway smoke test, not a
real measurement worth preserving in the log's history).

- [ ] **Step 5: Commit**

```bash
git add StratChessEvolved/Scripts/Run-EloMatch.ps1
git commit -m "Add -ResumeDir support to Run-EloMatch.ps1 (#119)"
```

### Task 2: Document the resume workflow and wrap up

**Files:**
- Modify: `Docs/EloLog.md`
- Modify: `Docs/Changelog.md`

**Interfaces:**
- Consumes: the completed, validated Task 1 script change.
- Produces: nothing new — documentation only.

- [x] **Step 1: Document the resume workflow in `Docs/EloLog.md`**

Read the file's current structure first (it has a "Measurement setup" section near the top). Add a
new subsection right after that section, before "## Interpreting results", along these lines
(adjust exact wording/heading level to match the file's existing style once you've read it):

```markdown
## Resuming an interrupted match

If a match gets killed mid-run (check `logs\elo\<stamp>\<stamp>.log` for a trailing `Started game
N` with no matching `Finished game N` — or the process/log simply stops advancing), resume it
instead of restarting from scratch:

```
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-EloMatch.ps1 -ResumeDir StratChessEvolved\logs\elo\<stamp>"
```

fastchess autosaves tournament state (`config.json`) into the match directory every
`-AutosaveInterval` games (default 20) and restores the full original engine/tournament
configuration from it — `-CandidateExe`/`-ReferenceTag`/`-Games`/`-Tc`/etc. are all ignored when
`-ResumeDir` is set. At most `-AutosaveInterval` games get replayed (whatever completed since the
last checkpoint before the kill), not the whole batch.
```

- [x] **Step 2: Add a one-line `Docs/Changelog.md` entry**

Read the file first to match its exact dated-entry format (see the existing mop-up evaluation entry
for the most recent example of this project's convention). Add an entry summarizing: added
`-ResumeDir`/`-AutosaveInterval` support to `Run-EloMatch.ps1` so an interrupted ELO match can
resume from fastchess's own checkpoint instead of restarting, fixing issue #119 (discovered during
#70's validation, where a match was killed at 491/500 games after ~60 minutes).

- [ ] **Step 3: Commit**

```bash
git add Docs/EloLog.md Docs/Changelog.md
git commit -m "Document ELO match resume workflow (#119)"
```

## Validation Plan

- Procedural (no unit-test framework for `.ps1` scripts): Task 1 Step 4's kill-and-resume test is
  the primary validation — it must demonstrate resume genuinely continuing from a checkpoint, not
  restarting.
- No build/C++ test suite changes are needed (this plan touches only `.ps1`/`.md` files) — skip
  `build.ps1`/Catch2 entirely for this PR.
- No `eval-reviewer`/`search-reviewer` dispatch needed (`category:infra`, not an eval/search change).

## Key Correctness Properties

- A match interrupted after at least one autosave checkpoint must be resumable via `-ResumeDir`
  without replaying more than `-AutosaveInterval` games.
- Resuming must not silently restart the tournament from game 1 with no error — if `-config` alone
  can't resume, the script must fail loudly (non-zero exit, clear message), not produce a
  misleadingly-labeled fresh 500-game (or default) run under the resumed match's directory/name.
- `Docs/EloLog.md`'s appended row after a resumed match must report the actual completed game
  count (from fastchess's own final summary), not the ignored `-Games` parameter default.
- Fresh (non-resume) invocations must behave identically to before this change — same preflight
  checks, same reference-exe caching/rebuild behavior, same fastchess flags (plus the new
  `-autosaveinterval`).
