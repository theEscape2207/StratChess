<#
.SYNOPSIS
    Compare two engine builds for behavioural identity at fixed depth.

.DESCRIPTION
    The gate CLAUDE.md and Docs/Workflow.md name for a change that claims to
    preserve behaviour: "identical node counts and best moves at Threads=1".
    Run-Bench measures nps and Run-EloMatch measures strength; this is the third
    question — do two binaries search the same tree and reach the same answer.

    Per position, at Threads=1 and a fixed depth, it compares EVERY per-iteration
    'info depth' line — depth, score, nodes and the full PV — plus the final line
    and 'bestmove', with only the wall-clock 'time' field stripped, since that is
    the one field that legitimately varies between runs. Comparing just the last
    line is a weaker check: two builds can agree on the answer and disagree on how
    they reached it, and for a refactor the path is exactly what is under test.

    The 'info string treenodes' split (issue #312) is compared as well when BOTH
    builds emit it. A build predating #312 emits none, and that is reported rather
    than counted as a difference — comparing against such a build is the normal
    case for an old baseline.

    WHAT THIS CANNOT ANSWER. Where node counts change by design — an evaluation
    term, a move-ordering change — a difference here is the intended effect, not a
    finding. Run-Bench plus an SPRT is the route for those. This is a gate only for
    changes that claim to change nothing.

    Nor does it cover behaviour under abort: a fixed-depth search never runs the
    clock out, so anything about what an interrupted search stores or reports is
    outside it (that is what #299's SPRT was for).

.PARAMETER After
    The candidate binary. Required.

.PARAMETER Before
    The baseline binary, when you already have one. Mutually exclusive with
    -BaselineRef.

.PARAMETER BaselineRef
    Build the baseline from a git ref instead (typically origin/main), in a
    throwaway detached worktree, and cache the result per commit under -CacheRoot.
    Mutually exclusive with -Before.

    This exists because building the "before" side by hand is where the check goes
    wrong. Checking out a file list over the working tree cannot restore a file
    that the baseline never had, so a NEW file stays in the tree and is compiled
    into the "before" build — and CMakeLists.txt globs with CONFIGURE_DEPENDS, so
    a stray new .cpp is picked up with nothing to notice it. That build is not a
    baseline; it is the candidate minus part of the diff. Building a whole tree at
    a ref is correct by construction and needs no file list.

    Not implemented with 'git stash push -u' either, though that is also correct
    by construction: the stash stack is shared with every other worktree of this
    repo and another session can pop what this one pushed (CLAUDE.md).

.PARAMETER Depth
    Fixed search depth. Default 12, matching Run-Bench.ps1 and the equivalence
    results already recorded in Docs/Changelog.md, so one depth is quoted across
    the measurement scripts. Deeper is a strictly stronger check here — every
    additional iteration is another compared line — and unlike Run-Bench there is
    no lower bound to respect, since nothing is being timed. The cost is search
    time on both binaries, twelve searches for the built-in set.

    Completion is strict: every position must emit both `info depth <Depth>` and
    `bestmove`. A terminal, mate, or other early-stop custom position that ends
    before -Depth is rejected rather than compared as a fixed-depth result.
    Choose nonterminal positions or lower -Depth for custom suites.

.PARAMETER Positions
    Optional file of positions, one per line; blank lines and lines starting with
    '#' are ignored. Each line is either a UCI position argument ('startpos',
    'startpos moves e2e4 e7e5', 'fen <FEN>') or a bare FEN, which is accepted and
    prefixed with 'fen'. Every FEN needs its side-to-move field or the engine
    silently answers for the wrong side (bug #46).

    Defaults to the built-in set of six from issue #330.

.PARAMETER Compiler
    Compiler for a -BaselineRef build. Defaults to the candidate's own, inferred
    from its build/<preset>/ path, else clang-cl.

.PARAMETER Config
    Configuration for a -BaselineRef build. Defaults to the candidate's own,
    inferred the same way, else Release.

.PARAMETER CacheRoot
    Where -BaselineRef builds are cached. Defaults to StratChessEquivalence beside
    the main checkout, the same placement build.ps1 uses for its shared FetchContent
    cache.

.PARAMETER AllowMixedCompiler
    Compare a clang-cl build against an MSVC one. Refused by default: a difference
    then cannot be attributed to the source change, which is the only question this
    script exists to answer. Meaningful only when the compilers ARE what is being
    compared.

.PARAMETER AllowSameBinary
    Compare a binary against itself. Refused by default, because a run that
    silently did that reports "identical" exactly as confidently as a real pass.
    Deliberately doing it is the harness's own determinism check.

.PARAMETER SelfTest
    Run the comparator's assertion table and exit. No engine, no build.

.OUTPUTS
    Exit code 0 when every position is identical, 1 otherwise.

.HOW TO INVOKE (from bash, cmd, or PowerShell)
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\Compare-SearchEquivalence.ps1 -After <exe> -BaselineRef origin/main

.EXAMPLE
    # The usual gate: candidate you just built, baseline built from origin/main.
    Compare-SearchEquivalence.ps1 -After .\build\windows-clang-cl\StratChessEvolved.exe -BaselineRef origin/main

.EXAMPLE
    # Two binaries you already have.
    Compare-SearchEquivalence.ps1 -Before .\before.exe -After .\build\windows-clang-cl\StratChessEvolved.exe -Depth 14

.EXAMPLE
    Compare-SearchEquivalence.ps1 -SelfTest
#>
[CmdletBinding()]
param(
    [string]$After = '',

    [string]$Before = '',

    [string]$BaselineRef = '',

    [ValidateRange(1, 30)]
    [int]$Depth = 12,

    [string]$Positions = '',

    [ValidateSet('clang-cl', 'msvc')]
    [string]$Compiler = '',

    [ValidateSet('Release', 'Debug')]
    [string]$Config = '',

    [string]$CacheRoot = '',

    [switch]$AllowMixedCompiler,

    [switch]$AllowSameBinary,

    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Threads is deliberately not a parameter. Lazy SMP makes node counts
# non-deterministic, so at Threads>1 there is no equivalence to check and a
# reported difference would mean nothing.
$ThreadCount = 1

# Issue #330's set: two positions reached by a move list and four FENs spanning
# middlegame, endgame and a sharp tactical position. Every FEN carries its
# side-to-move field (bug #46).
$DefaultPositions = @(
    @{ Name = 'startpos';    Spec = 'startpos' }
    @{ Name = 'ruy-lopez';   Spec = 'startpos moves e2e4 e7e5 g1f3 b8c6 f1b5' }
    @{ Name = 'kiwipete';    Spec = 'fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1' }
    @{ Name = 'rook-endgm';  Spec = 'fen 8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1' }
    @{ Name = 'attack-mid';  Spec = 'fen rn3k1r/pp2bBpp/2p2n2/q5N1/3P4/1P6/P1P3PP/R1BQ1RK1 w - - 0 1' }
    @{ Name = 'quiet-mid';   Spec = 'fen 4rrk1/pp1n1pp1/3bp2p/3p4/2PP4/1P1B1N2/P4PPP/R2QR1K1 w - - 0 1' }
)

# ---------------------------------------------------------------------------
# Pure helpers — everything -SelfTest asserts lives here
# ---------------------------------------------------------------------------

function ConvertTo-PositionSpec {
    <#
        A line of a positions file into a UCI 'position' argument. A bare FEN is
        accepted so a Run-Bench positions file works here unchanged.
    #>
    param([Parameter(Mandatory)][string]$Line)

    $t = $Line.Trim()
    if ($t -match '^(startpos|fen)\b') { return $t }
    return "fen $t"
}

function ConvertTo-ComparableLines {
    <#
        Engine output into the lines that carry search behaviour, normalised.

        'time' is stripped because it is wall clock and legitimately differs
        between two runs of the SAME binary; every other field on these lines is a
        property of the search. Everything else the engine prints — the 'uci'
        banner, option echoes, position diagnostics — is dropped: it says nothing
        about the tree that was searched.
    #>
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Output)

    $kept = foreach ($raw in ($Output -split "`r?`n")) {
        $line = $raw.Trim()
        if ($line -match '^info depth \d+' -or
            $line -match '^bestmove ' -or
            $line -match '^info string treenodes ') {
            $line -replace ' time \d+', ''
        }
    }
    return @($kept)
}

function Remove-NodeSplitLines {
    param([Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Lines)
    return @($Lines | Where-Object { $_ -notmatch '^info string treenodes ' })
}

function Test-FixedDepthTranscript {
    <#
        A fixed-depth measurement is valid only if the engine actually reached
        that depth and completed it with a best move.  In particular, a
        transcript from a search aborted by an eagerly queued `quit` must not
        compare as an apparently successful empty run.
    #>
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Lines,
        [Parameter(Mandatory)][int]$SearchDepth
    )

    return [pscustomobject]@{
        ReachedDepth = @($Lines | Where-Object { $_ -match "^info depth $SearchDepth\b" }).Count -gt 0
        HasBestMove  = @($Lines | Where-Object { $_ -match '^bestmove \S+' }).Count -gt 0
    }
}

function Compare-Transcript {
    <#
        Two normalised transcripts. Returns the FIRST difference, because that is
        the one that explains the rest: once two searches diverge, every later
        line differs as a consequence.
    #>
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$BeforeLines,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$AfterLines
    )

    $n = [Math]::Max($BeforeLines.Count, $AfterLines.Count)
    for ($i = 0; $i -lt $n; $i++) {
        $b = if ($i -lt $BeforeLines.Count) { $BeforeLines[$i] } else { '(no line — transcript ends here)' }
        $a = if ($i -lt $AfterLines.Count)  { $AfterLines[$i] }  else { '(no line — transcript ends here)' }
        if ($b -cne $a) {
            return [pscustomobject]@{
                Identical = $false
                Index     = $i + 1
                Before    = $b
                After     = $a
                Compared  = $i
            }
        }
    }

    return [pscustomobject]@{
        Identical = $true
        Index     = 0
        Before    = ''
        After     = ''
        Compared  = $n
    }
}

function Get-BuildFlavour {
    <#
        Compiler and configuration from a binary's path, which is the only place
        they are recorded: CMake presets emit into build/<preset>/, and a cached
        baseline mirrors that layout on purpose so this keeps working for it.
        Returns $null when the path says nothing.
    #>
    param([Parameter(Mandatory)][string]$Path)

    $p = $Path.Replace('\', '/')
    if ($p -match '/windows-(clang-cl|msvc)(-debug)?/[^/]+\.exe$') {
        return [pscustomobject]@{
            Compiler = $Matches[1]
            Config   = if ($Matches[2]) { 'Debug' } else { 'Release' }
        }
    }
    return $null
}

# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

if ($SelfTest) {
    $failures = 0

    function Assert-Case {
        param([string]$Name, [bool]$Ok, [string]$Detail = '')
        if ($Ok) {
            Write-Host ("  PASS  {0}" -f $Name) -ForegroundColor Green
        } else {
            Write-Host ("  FAIL  {0}{1}" -f $Name, $(if ($Detail) { " — $Detail" } else { '' })) -ForegroundColor Red
            $script:failures++
        }
    }

    $sampleOut = @(
        'id name StratChessEvolved'
        'uciok'
        'readyok'
        'info string position: ok'
        'info depth 1 score cp 24 nodes 21 time 3 pv e2e4'
        'info depth 2 score cp 12 nodes 97 time 5 pv e2e4 e7e5'
        'info depth 2 score cp 12 nodes 140 time 9 pv e2e4 e7e5'
        'info string treenodes main 100 qs 40'
        'bestmove e2e4'
    ) -join "`n"

    $lines = ConvertTo-ComparableLines -Output $sampleOut
    Assert-Case 'keeps only info depth / treenodes / bestmove lines' ($lines.Count -eq 5) "got $($lines.Count)"
    Assert-Case 'strips the time field' ($lines[0] -eq 'info depth 1 score cp 24 nodes 21 pv e2e4')

    # The whole point of stripping time: two runs of ONE binary differ there.
    $slower = $sampleOut -replace ' time 5 ', ' time 5000 '
    $r = Compare-Transcript (ConvertTo-ComparableLines $sampleOut) (ConvertTo-ComparableLines $slower)
    Assert-Case 'time-only difference compares identical' $r.Identical "first diff at line $($r.Index)"

    $r = Compare-Transcript (ConvertTo-ComparableLines $sampleOut) (ConvertTo-ComparableLines $sampleOut)
    Assert-Case 'identical output compares identical' $r.Identical
    Assert-Case 'identical run reports the compared-line count' ($r.Compared -eq 5) "got $($r.Compared)"

    # A divergence at an EARLY iteration with the same final answer — the case a
    # last-line-only probe misses, and the reason this script exists.
    $earlyDiff = $sampleOut -replace 'nodes 97', 'nodes 96'
    $r = Compare-Transcript (ConvertTo-ComparableLines $sampleOut) (ConvertTo-ComparableLines $earlyDiff)
    Assert-Case 'early-iteration node difference is caught' ((-not $r.Identical) -and $r.Index -eq 2) "diff at $($r.Index)"

    $pvDiff = $sampleOut -replace 'pv e2e4 e7e5', 'pv e2e4 c7c5'
    $r = Compare-Transcript (ConvertTo-ComparableLines $sampleOut) (ConvertTo-ComparableLines $pvDiff)
    Assert-Case 'pv difference is caught' ((-not $r.Identical) -and $r.Index -eq 2) "diff at $($r.Index)"

    $bestDiff = $sampleOut -replace 'bestmove e2e4', 'bestmove d2d4'
    $r = Compare-Transcript (ConvertTo-ComparableLines $sampleOut) (ConvertTo-ComparableLines $bestDiff)
    Assert-Case 'bestmove difference is caught' ((-not $r.Identical) -and $r.Index -eq 5) "diff at $($r.Index)"

    # One side stopping an iteration earlier is a difference, not a shorter pass.
    $truncated = @(ConvertTo-ComparableLines $sampleOut)[0..2]
    $r = Compare-Transcript (ConvertTo-ComparableLines $sampleOut) $truncated
    Assert-Case 'a shorter transcript is a difference' ((-not $r.Identical) -and $r.Index -eq 4) "diff at $($r.Index)"

    $r = Compare-Transcript @() @()
    Assert-Case 'two empty transcripts compare identical' ($r.Identical -and $r.Compared -eq 0)

    $noSplit = Remove-NodeSplitLines (ConvertTo-ComparableLines $sampleOut)
    Assert-Case 'node-split lines can be dropped' ($noSplit.Count -eq 4)

    $completion = Test-FixedDepthTranscript -Lines (ConvertTo-ComparableLines $sampleOut) -SearchDepth 2
    Assert-Case 'fixed-depth transcript requires the requested depth and a bestmove' ($completion.ReachedDepth -and $completion.HasBestMove)
    $aborted = ConvertTo-ComparableLines "info depth 0 score cp 0 nodes 0 time 0 pv a2a4`nbestmove a2a4"
    $completion = Test-FixedDepthTranscript -Lines $aborted -SearchDepth 2
    Assert-Case 'aborted transcript cannot satisfy a fixed-depth search' ((-not $completion.ReachedDepth) -and $completion.HasBestMove)
    $missingBestMove = ConvertTo-ComparableLines 'info depth 2 score cp 24 nodes 97 time 5 pv e2e4 e7e5'
    $completion = Test-FixedDepthTranscript -Lines $missingBestMove -SearchDepth 2
    Assert-Case 'requested depth without bestmove is incomplete' ($completion.ReachedDepth -and (-not $completion.HasBestMove))

    Assert-Case 'FEN line becomes a fen spec' ((ConvertTo-PositionSpec '8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1') -eq 'fen 8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1')
    Assert-Case 'startpos passes through'      ((ConvertTo-PositionSpec 'startpos') -eq 'startpos')
    Assert-Case 'move list passes through'     ((ConvertTo-PositionSpec 'startpos moves e2e4 e7e5') -eq 'startpos moves e2e4 e7e5')
    Assert-Case 'fen spec passes through'      ((ConvertTo-PositionSpec 'fen 8/8/8/8/8/8/8/K1k5 w - - 0 1') -eq 'fen 8/8/8/8/8/8/8/K1k5 w - - 0 1')

    $f = Get-BuildFlavour 'C:\repo\build\windows-clang-cl\StratChessEvolved.exe'
    Assert-Case 'clang-cl Release inferred' ($null -ne $f -and $f.Compiler -eq 'clang-cl' -and $f.Config -eq 'Release')
    $f = Get-BuildFlavour 'C:/repo/build/windows-msvc-debug/StratChessEvolved.exe'
    Assert-Case 'msvc Debug inferred' ($null -ne $f -and $f.Compiler -eq 'msvc' -and $f.Config -eq 'Debug')
    Assert-Case 'unknown path infers nothing' ($null -eq (Get-BuildFlavour 'C:\tmp\before.exe'))

    Write-Host ''
    if ($failures -gt 0) {
        Write-Host "$failures self-test case(s) FAILED." -ForegroundColor Red
        exit 1
    }
    Write-Host 'All self-test cases passed.' -ForegroundColor Green
    exit 0
}

# ---------------------------------------------------------------------------
# Engine driving
# ---------------------------------------------------------------------------

# Invoke-UciSearchToBestMove lives in the shared library because Run-Bench.ps1 drives
# the engine the same way; a fix to the shutdown sequence has to reach both.
. (Join-Path $PSScriptRoot 'UciDriver.ps1')

function Invoke-FixedDepthSearch {
    <#
        One position in a FRESH engine process, so no transposition-table state
        carries over from the previous position and each result depends only on
        the binary and the position.
    #>
    param(
        [Parameter(Mandatory)][string]$ExePath,
        [Parameter(Mandatory)][string]$WorkDir,
        [Parameter(Mandatory)][string]$Spec,
        [Parameter(Mandatory)][int]$SearchDepth
    )

    $commands = @(
        'uci'
        'isready'
        "setoption name Threads value $ThreadCount"
        "position $Spec"
        "go depth $SearchDepth"
    )

    $out = Invoke-UciSearchToBestMove -ExePath $ExePath -WorkDir $WorkDir -Commands $commands `
                                      -SearchDepth $SearchDepth -Description $Spec

    $lines = ConvertTo-ComparableLines -Output $out
    if ($lines.Count -eq 0) {
        throw "No 'info depth' or 'bestmove' output from $ExePath for position: $Spec`nEngine output:`n$out"
    }

    $completion = Test-FixedDepthTranscript -Lines $lines -SearchDepth $SearchDepth
    if (-not $completion.ReachedDepth -or -not $completion.HasBestMove) {
        throw ("Fixed-depth search did not complete depth $SearchDepth for position: $Spec" +
               "`nReached requested depth: $($completion.ReachedDepth); emitted bestmove: $($completion.HasBestMove)." +
               "`nEngine output:`n$out")
    }
    return $lines
}

function Resolve-PositionList {
    param([string]$Path)

    if (-not $Path) { return $DefaultPositions }
    if (-not (Test-Path $Path)) { throw "Positions file not found: $Path" }

    $i = 0
    $list = foreach ($line in Get-Content $Path) {
        $t = $line.Trim()
        if (-not $t -or $t.StartsWith('#')) { continue }
        $i++
        @{ Name = "pos-$i"; Spec = (ConvertTo-PositionSpec $t) }
    }

    if (-not $list) { throw "No positions found in $Path" }
    return @($list)
}

function Get-MainCheckoutParent {
    param([Parameter(Mandatory)][string]$RepoPath)

    $commonDir = & git -C $RepoPath rev-parse --path-format=absolute --git-common-dir 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $commonDir) {
        throw "Not a git repository (or git unavailable): $RepoPath"
    }
    $mainCheckout = $commonDir -replace '[\\/]\.git[\\/]?$', ''
    return (Split-Path $mainCheckout -Parent)
}

function New-BaselineBinary {
    <#
        Build $Ref in a throwaway detached worktree and cache the binary per
        commit. Cached under build/<preset>/ layout so Get-BuildFlavour can still
        read its compiler and configuration off the path.
    #>
    param(
        [Parameter(Mandatory)][string]$RepoPath,
        [Parameter(Mandatory)][string]$Ref,
        [Parameter(Mandatory)][string]$CompilerName,
        [Parameter(Mandatory)][string]$ConfigName,
        [Parameter(Mandatory)][string]$Root
    )

    $sha = (& git -C $RepoPath rev-parse "$Ref^{commit}" 2>$null)
    if ($LASTEXITCODE -ne 0 -or -not $sha) { throw "Cannot resolve ref '$Ref' to a commit." }
    $sha = $sha.Trim()
    $short = $sha.Substring(0, 12)

    $preset = "windows-$CompilerName"
    if ($ConfigName -eq 'Debug') { $preset += '-debug' }

    $cached = Join-Path $Root "$short\build\$preset\StratChessEvolved.exe"
    if (Test-Path $cached) {
        Write-Host "Baseline: reusing cached build of $Ref ($short)" -ForegroundColor DarkGray
        return $cached
    }

    $wt = Join-Path $Root "worktree-$short"
    # A previous run killed mid-build leaves both the directory and git's
    # registration behind; prune first so 'worktree add' does not refuse.
    if (Test-Path $wt) {
        & git -C $RepoPath worktree remove --force $wt 2>$null | Out-Null
        if (Test-Path $wt) { Remove-Item -Recurse -Force $wt }
    }
    & git -C $RepoPath worktree prune 2>$null | Out-Null

    Write-Host "Baseline: building $Ref ($short) as $preset — this takes a few minutes" -ForegroundColor Cyan
    # Out-Host, not bare: a native command's stdout inside a function joins the
    # function's OUTPUT stream, so 'HEAD is now at ...' and every line of the build
    # below would be returned alongside the binary path and the caller would receive
    # an array. Out-Host keeps them visible and out of the return value.
    & git -C $RepoPath worktree add --detach $wt $sha | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "git worktree add failed for $sha" }

    Push-Location $wt
    try {
        # Invoked FROM the baseline worktree, not merely by its path. build.ps1 takes
        # its own root from $PSScriptRoot, but the `cmake --preset` it runs resolves
        # CMakePresets.json against the CURRENT DIRECTORY -- so calling it by path
        # from another worktree configures and builds that other tree instead, quietly
        # succeeds, and leaves nothing at the path this function then looks for.
        # Setting the location cannot be delegated to build.ps1: -BaselineRef builds
        # whatever build.ps1 that commit shipped.
        #
        # Its shared FetchContent cache resolves through the common git dir, so no
        # dependency is re-cloned for the throwaway tree.
        & pwsh -ExecutionPolicy Bypass -File (Join-Path $wt 'build.ps1') main -Config $ConfigName -Compiler $CompilerName | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "Baseline build failed (exit $LASTEXITCODE) for $Ref ($short)." }

        $built = Join-Path $wt "build\$preset\StratChessEvolved.exe"
        if (-not (Test-Path $built)) { throw "Baseline build produced no binary at $built" }

        New-Item -ItemType Directory -Force -Path (Split-Path $cached -Parent) | Out-Null
        Copy-Item $built $cached -Force
    }
    finally {
        Pop-Location
        & git -C $RepoPath worktree remove --force $wt 2>$null | Out-Null
        if (Test-Path $wt) { Remove-Item -Recurse -Force $wt -ErrorAction SilentlyContinue }
        & git -C $RepoPath worktree prune 2>$null | Out-Null
    }

    return $cached
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if (-not $After)                        { throw "-After is required (the candidate binary)." }
if ($Before -and $BaselineRef)          { throw "-Before and -BaselineRef are mutually exclusive: pick a baseline binary or a ref to build one from." }
if (-not $Before -and -not $BaselineRef) { throw "Supply a baseline: -Before <exe>, or -BaselineRef origin/main to build one." }

$RepoRoot = Split-Path $PSScriptRoot -Parent
$afterPath = (Resolve-Path $After).Path
$afterFlavour = Get-BuildFlavour $afterPath

if ($BaselineRef) {
    $compilerName = if ($Compiler) { $Compiler } elseif ($afterFlavour) { $afterFlavour.Compiler } else { 'clang-cl' }
    $configName   = if ($Config)   { $Config }   elseif ($afterFlavour) { $afterFlavour.Config }   else { 'Release' }
    if (-not $CacheRoot) {
        $CacheRoot = Join-Path (Get-MainCheckoutParent -RepoPath $RepoRoot) 'StratChessEquivalence'
    }
    $beforePath = New-BaselineBinary -RepoPath $RepoRoot -Ref $BaselineRef `
                                     -CompilerName $compilerName -ConfigName $configName -Root $CacheRoot
} else {
    $beforePath = (Resolve-Path $Before).Path
}

$beforeFlavour = Get-BuildFlavour $beforePath

$beforeHash = (Get-FileHash -Path $beforePath -Algorithm SHA256).Hash.Substring(0, 12).ToLower()
$afterHash  = (Get-FileHash -Path $afterPath  -Algorithm SHA256).Hash.Substring(0, 12).ToLower()

if ($beforeHash -eq $afterHash -and -not $AllowSameBinary) {
    throw ("Both sides are the same binary (sha $beforeHash). An equivalence run that quietly" +
           " compares a binary against itself reports 'identical' just as confidently as a real" +
           " pass. Pass -AllowSameBinary if that self-check is what you meant.")
}

if ($beforeFlavour -and $afterFlavour) {
    if ($beforeFlavour.Compiler -ne $afterFlavour.Compiler -and -not $AllowMixedCompiler) {
        throw ("Mixed-compiler comparison refused: before is $($beforeFlavour.Compiler), after is" +
               " $($afterFlavour.Compiler). A difference could then be code generation rather than" +
               " the source change. Pass -AllowMixedCompiler if comparing the compilers IS the point.")
    }
    if ($beforeFlavour.Config -ne $afterFlavour.Config) {
        Write-Host ("WARNING : configuration differs — before is $($beforeFlavour.Config), after is" +
                    " $($afterFlavour.Config).") -ForegroundColor Yellow
    }
} else {
    Write-Host "NOTE    : compiler could not be read off both paths; the mixed-compiler check is not active." -ForegroundColor Yellow
}

$positionList = @(Resolve-PositionList -Path $Positions)

# A common working directory for both engines, so neither can pick up a
# game_settings.json or a logs/ directory the other does not see.
$workDir = Join-Path ([System.IO.Path]::GetTempPath()) "StratChessEquivalence-run"
New-Item -ItemType Directory -Force -Path $workDir | Out-Null

Write-Host ''
Write-Host "Before  : $beforePath  (sha $beforeHash)"
Write-Host "After   : $afterPath  (sha $afterHash)"
$setName = if ($Positions) { Split-Path -Leaf $Positions } else { 'builtin' }
Write-Host "Depth   : $Depth    Threads: $ThreadCount    Positions: $($positionList.Count) ($setName)"
Write-Host ''

$totalCompared = 0
$differing     = [System.Collections.Generic.List[object]]::new()
$splitDropped  = $false

foreach ($p in $positionList) {
    $b = Invoke-FixedDepthSearch -ExePath $beforePath -WorkDir $workDir -Spec $p.Spec -SearchDepth $Depth
    $a = Invoke-FixedDepthSearch -ExePath $afterPath  -WorkDir $workDir -Spec $p.Spec -SearchDepth $Depth

    # One build predating #312 emits no node split. Dropping it from both sides is
    # the honest comparison; counting its absence as a divergence is not.
    $bHasSplit = @($b | Where-Object { $_ -match '^info string treenodes ' }).Count -gt 0
    $aHasSplit = @($a | Where-Object { $_ -match '^info string treenodes ' }).Count -gt 0
    if ($bHasSplit -ne $aHasSplit) {
        $b = Remove-NodeSplitLines $b
        $a = Remove-NodeSplitLines $a
        $splitDropped = $true
    }

    $r = Compare-Transcript -BeforeLines $b -AfterLines $a
    $totalCompared += $r.Compared

    if ($r.Identical) {
        Write-Host ("  IDENTICAL  {0,-12} {1,3} lines" -f $p.Name, $r.Compared) -ForegroundColor Green
    } else {
        Write-Host ("  DIFFERENT  {0,-12} first difference at compared line {1}" -f $p.Name, $r.Index) -ForegroundColor Red
        Write-Host ("      position: $($p.Spec)")
        Write-Host ("      before  : $($r.Before)")
        Write-Host ("      after   : $($r.After)")
        $differing.Add($p)
    }
}

Write-Host ''
if ($splitDropped) {
    Write-Host "NOTE    : one build emits no 'info string treenodes' split (predates #312); it was" -ForegroundColor Yellow
    Write-Host "          excluded from both sides rather than counted as a difference."            -ForegroundColor Yellow
}

if ($differing.Count -eq 0) {
    Write-Host "IDENTICAL across $totalCompared compared lines, $($positionList.Count) positions at depth $Depth." -ForegroundColor Green
    exit 0
}

Write-Host "DIFFERENT: $($differing.Count) of $($positionList.Count) positions diverge at depth $Depth." -ForegroundColor Red
Write-Host "The two builds do not search the same tree. If that was expected, this check is not the" -ForegroundColor Red
Write-Host "right gate for the change — Run-Bench plus an SPRT is."                                   -ForegroundColor Red
exit 1
