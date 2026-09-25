<#
.SYNOPSIS
    Fail when a skill, a subagent, CLAUDE.md or AGENTS.md cites something that does not exist.

.DESCRIPTION
    Four shape checks over the working tree:

    - Frontmatter: each skill, Codex adapter and agent file opens with `---` delimiters, its
      `name` equals its directory or file name, and its `description` is non-empty.
    - Codex parity: each `.claude/skills/<n>/` has an `.agents/skills/<n>/SKILL.md` citing it, each
      adapter's `.claude/skills/...` citation resolves, and each `.claude/agents/<n>.md` has a
      `.codex/agents/<n>.toml` naming and citing it, and the reverse. Every skill and agent must be
      reachable from CLAUDE.md, and separately from AGENTS.md: named in backticks there, or in the
      body of a reachable skill or agent.
    - Paths: a backticked span containing `/` whose first segment is a tracked top-level directory,
      or a directory beside the citing file, must be tracked or gitignored.
    - Identifiers, in `.claude/agents/*.md` only: a camelCase, PascalCase, g_/m_ or UPPER_SNAKE span
      must occur as a whole word in a tracked .cpp or .h file.

    A pass means the citations exist, not that a doc is right or a dispatch rule complete. A false
    positive is fixed in the rule, with a self-test case, or by rewording the doc.

.PARAMETER Root
    Repository to check. Defaults to the repository containing this script.

.PARAMETER SelfTest
    Run the checks against an in-memory fixture tree and exit.

.HOW TO INVOKE
    pwsh -File Scripts/Test-AgentDocs.ps1
    pwsh -File Scripts/Test-AgentDocs.ps1 -SelfTest
#>

[CmdletBinding(DefaultParameterSetName = 'Run')]
param(
    [Parameter(ParameterSetName = 'Run')]
    [string]$Root,

    [Parameter(Mandatory, ParameterSetName = 'SelfTest')]
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-Span {
    # Each backticked span in the text, with its 1-based line number.
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)
    $lineNo = 0
    foreach ($line in $Text -split "`n") {
        $lineNo++
        foreach ($m in [regex]::Matches($line, '`([^`]+)`')) {
            [pscustomobject]@{ Line = $lineNo; Span = $m.Groups[1].Value.Trim() }
        }
    }
}

function Join-RepoPath {
    param([AllowEmptyString()][string]$Dir, [string]$Relative)
    $parts = [System.Collections.Generic.List[string]]::new()
    foreach ($segment in "$Dir/$Relative" -split '/') {
        if ($segment -eq '..' -and $parts.Count -gt 0 -and $parts[-1] -ne '..') { $parts.RemoveAt($parts.Count - 1) }
        elseif ($segment -and $segment -ne '.') { $parts.Add($segment) }
    }
    $parts -join '/'
}

function Get-AgentDocFailure {
    param(
        # Every tracked path mapped to its LF-normalised text; files no check reads may map to ''.
        [Parameter(Mandatory)][hashtable]$Files,
        [Parameter(Mandatory)][scriptblock]$IsIgnored
    )
    $failures = [System.Collections.Generic.List[object]]::new()
    function Add-Failure($Path, $Line, $Rule, $Span) {
        $failures.Add([pscustomobject]@{ File = $Path; Line = $Line; Rule = $Rule; Span = $Span })
    }

    $paths = @($Files.Keys | Sort-Object)
    $tracked = [System.Collections.Generic.HashSet[string]]::new([string[]]$paths, [StringComparer]::Ordinal)
    $dirs = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($p in $paths) {
        $parts = $p -split '/'
        for ($i = 1; $i -lt $parts.Count; $i++) { [void]$dirs.Add($parts[0..($i - 1)] -join '/') }
    }
    $skills = @($dirs | Where-Object { $_ -match '^\.claude/skills/[^/]+$' } | ForEach-Object { $_.Split('/')[2] } | Sort-Object)
    $agents = @($paths | Where-Object { $_ -match '^\.claude/agents/[^/]+\.md$' } | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) })
    $tomls = @($paths | Where-Object { $_ -match '^\.codex/agents/[^/]+\.toml$' } | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) })
    $adapters = @($paths | Where-Object { $_ -match '^\.agents/skills/[^/]+/SKILL\.md$' })
    $docs = @($paths | Where-Object { $_ -match '^(\.claude/skills/.+\.md|\.claude/agents/[^/]+\.md|CLAUDE\.md|AGENTS\.md)$' })
    $counts = [ordered]@{ Docs = $docs.Count; Skills = $skills.Count; Agents = $agents.Count; Paths = 0; Identifiers = 0 }

    # Frontmatter.
    $frontmatterFiles = @($skills | ForEach-Object { ".claude/skills/$_/SKILL.md" }) + $adapters +
        @($agents | ForEach-Object { ".claude/agents/$_.md" })
    foreach ($f in $frontmatterFiles) {
        $expected = if ($f -like '*/SKILL.md') { $f.Split('/')[2] } else { [IO.Path]::GetFileNameWithoutExtension($f) }
        if (-not $tracked.Contains($f)) { Add-Failure $f 0 'frontmatter' 'missing file'; continue }
        if ($Files[$f] -notmatch '\A---\n([\s\S]*?)\n---(\n|\z)') { Add-Failure $f 1 'frontmatter' '---'; continue }
        $head = $Matches[1]
        $name = if ($head -match '(?m)^name:[ \t]*["'']?([^"''\n]*?)["'']?[ \t]*$') { $Matches[1] } else { '' }
        if ($name -cne $expected) { Add-Failure $f 1 'name' "name: $name" }
        if ($head -notmatch '(?m)^description:[ \t]*(\S|\n[ \t]+\S)') { Add-Failure $f 1 'description' 'description:' }
    }

    # Codex parity: adapters, agent tomls, then routes.
    foreach ($n in $skills) {
        $adapter = ".agents/skills/$n/SKILL.md"
        if (-not $tracked.Contains($adapter) -or -not $Files[$adapter].Contains(".claude/skills/$n/SKILL.md")) {
            Add-Failure $adapter 0 'adapter' ".claude/skills/$n/SKILL.md"
        }
    }
    foreach ($a in $adapters) {
        foreach ($m in [regex]::Matches($Files[$a], '(\.\./)*\.claude/skills/[^\s`''")\]]+')) {
            $cited = $m.Value -replace '[.,;:]+$'
            $target = if ($cited.StartsWith('../')) { Join-RepoPath ($a -replace '/[^/]+$') $cited } else { $cited }
            if (-not ($tracked.Contains($target) -or $dirs.Contains($target.TrimEnd('/')))) { Add-Failure $a 0 'adapter' $cited }
        }
    }
    foreach ($n in @($agents + $tomls | Sort-Object -Unique)) {
        $toml = ".codex/agents/$n.toml"
        $paired = $tracked.Contains(".claude/agents/$n.md") -and $tracked.Contains($toml) -and
            $Files[$toml] -cmatch "(?m)^name\s*=\s*`"$([regex]::Escape($n))`"" -and $Files[$toml].Contains(".claude/agents/$n.md")
        if (-not $paired) { Add-Failure $toml 0 'codex-agent' ".claude/agents/$n.md" }
    }
    $bodies = @{}
    foreach ($n in $skills) { $bodies[$n] = [string]$Files[".claude/skills/$n/SKILL.md"] }
    foreach ($n in $agents) { $bodies[$n] = [string]$Files[".claude/agents/$n.md"] }
    foreach ($routeFile in 'CLAUDE.md', 'AGENTS.md') {
        $reached = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        $queue = [System.Collections.Generic.Queue[string]]::new()
        $queue.Enqueue([string]$Files[$routeFile])
        while ($queue.Count -gt 0) {
            foreach ($s in Get-Span $queue.Dequeue()) {
                if ($bodies.ContainsKey($s.Span) -and $reached.Add($s.Span)) { $queue.Enqueue($bodies[$s.Span]) }
            }
        }
        foreach ($n in @($bodies.Keys | Sort-Object)) {
            if (-not $reached.Contains($n)) { Add-Failure $routeFile 0 'route' $n }
        }
    }

    # Paths and identifiers.
    $sourceWords = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($p in $paths) {
        if ($p -match '\.(cpp|h)$') { $sourceWords.UnionWith([string[]][regex]::Split($Files[$p], '\W+')) }
    }
    foreach ($doc in $docs) {
        $docDir = $doc -replace '/?[^/]+$'
        $isAgent = $doc -like '.claude/agents/*'
        foreach ($s in Get-Span $Files[$doc]) {
            $span = $s.Span
            if ($span.Contains('/') -and $span -notmatch '^[/-]|[<>*{}…\s]|\.\.\.|^\w+:') {
                $first = $span.Split('/')[0]
                if ($dirs.Contains($first) -or ($docDir -and $dirs.Contains("$docDir/$first"))) {
                    $counts['Paths']++
                    $target = ($span -replace '(:\d+(-\d+)?|#.*)$').TrimEnd('/')
                    $fromDoc = Join-RepoPath $docDir $target
                    $found = $tracked.Contains($target) -or $dirs.Contains($target) -or
                        $tracked.Contains($fromDoc) -or $dirs.Contains($fromDoc) -or (& $IsIgnored $target)
                    if (-not $found) { Add-Failure $doc $s.Line 'path' $span }
                }
            }
            if ($isAgent) {
                $token = $span -replace '(\(\)|::.*)$'
                if ($token -cmatch '^[A-Za-z_]\w*$' -and $token -cmatch '[a-z][A-Z]|^[gm]_|^[A-Z][A-Z0-9]*_[A-Z0-9_]+$') {
                    $counts['Identifiers']++
                    if (-not $sourceWords.Contains($token)) { Add-Failure $doc $s.Line 'identifier' $span }
                }
            }
        }
    }

    # Fail closed: a check that saw nothing proves nothing.
    foreach ($k in @($counts.Keys)) {
        if ($counts[$k] -eq 0) { Add-Failure '(tree)' 0 'empty' "no $k" }
    }
    [pscustomobject]@{ Failures = $failures; Counts = $counts }
}

if ($SelfTest) {
    function New-Doc($Name, $Body) { "---`nname: $Name`ndescription: Fixture.`n---`n$Body" }
    $base = @{
        'CLAUDE.md'                               = 'Use `alpha`; read `Docs/Guide.md#setup`, `Docs/Guide.md:12`, `Docs/local/run.log`. Not paths: `origin/main`, `/code-review`, `-Name a/b`, `Docs/<n>.md`, `https://x.y/z`.'
        'AGENTS.md'                               = 'Use skill `alpha`.'
        'Docs/Guide.md'                           = ''
        '.claude/skills/alpha/SKILL.md'           = New-Doc 'alpha' 'Load `beta`, dispatch `rev`, see `reference/notes.md`.'
        '.claude/skills/alpha/reference/notes.md' = 'Built from `Src/Eval.cpp:3`.'
        '.claude/skills/beta/SKILL.md'            = New-Doc 'beta' 'Nothing.'
        '.agents/skills/alpha/SKILL.md'           = New-Doc 'alpha' 'Read `../../../.claude/skills/alpha/SKILL.md`.'
        '.agents/skills/beta/SKILL.md'            = New-Doc 'beta' 'Read `../../../.claude/skills/beta/SKILL.md`.'
        '.agents/skills/tdd/SKILL.md'             = New-Doc 'tdd' 'A vendored skill with no project counterpart.'
        '.claude/agents/rev.md'                   = New-Doc 'rev' 'Check `g_iValue`, `PlayScore::Eval()` and `MAX_PLY`.'
        '.codex/agents/rev.toml'                  = "name = `"rev`"`ninstructions = `"Read .claude/agents/rev.md`""
        'Src/Eval.cpp'                            = 'int g_iValue; struct PlayScore {}; constexpr int MAX_PLY = 64;'
    }
    $emptied = @{}
    foreach ($k in $base.Keys) { $emptied[$k] = $null }
    $cases = @(
        @{ Name = 'clean fixture passes';               Change = @{}; Expect = @() }
        @{ Name = 'no frontmatter';                     Change = @{ '.claude/skills/beta/SKILL.md' = 'Nothing.' }; Expect = @('frontmatter') }
        @{ Name = 'name differs from directory';        Change = @{ '.agents/skills/beta/SKILL.md' = New-Doc 'bta' '`../../../.claude/skills/beta/SKILL.md`' }; Expect = @('name') }
        @{ Name = 'empty description';                  Change = @{ '.claude/agents/rev.md' = "---`nname: rev`ndescription:`n---`n``g_iValue``" }; Expect = @('description') }
        @{ Name = 'folded description passes';          Change = @{ '.claude/agents/rev.md' = "---`nname: rev`ndescription:`n  Folded.`n---`n``g_iValue``" }; Expect = @() }
        @{ Name = 'missing adapter';                    Change = @{ '.agents/skills/beta/SKILL.md' = $null }; Expect = @('adapter') }
        @{ Name = 'dangling adapter';                   Change = @{ '.agents/skills/gone/SKILL.md' = New-Doc 'gone' '`../../../.claude/skills/gone/SKILL.md`' }; Expect = @('adapter') }
        @{ Name = 'agent without Codex toml';           Change = @{ '.codex/agents/rev.toml' = $null }; Expect = @('codex-agent') }
        @{ Name = 'Codex toml without agent';           Change = @{ '.codex/agents/old.toml' = "name = `"old`"`n.claude/agents/old.md" }; Expect = @('codex-agent') }
        @{ Name = 'toml names another agent';           Change = @{ '.codex/agents/rev.toml' = "name = `"re`"`n.claude/agents/rev.md" }; Expect = @('codex-agent') }
        @{ Name = 'skill unreachable from AGENTS.md';   Change = @{ 'AGENTS.md' = 'Use `beta` and `rev`.' }; Expect = @('route') }
        @{ Name = 'unresolved path from root';          Change = @{ 'AGENTS.md' = 'Use `alpha`; read `Docs/Gone.md`.' }; Expect = @('path') }
        @{ Name = 'unresolved path beside the doc';     Change = @{ '.claude/skills/alpha/SKILL.md' = New-Doc 'alpha' '`beta`, `rev`, `reference/gone.md`' }; Expect = @('path') }
        @{ Name = 'identifiers absent from source';     Change = @{ '.claude/agents/rev.md' = New-Doc 'rev' '`PlayState`, `iMinScore`, `gameStage`' }; Expect = @('identifier', 'identifier', 'identifier') }
        @{ Name = 'no identifier spans fails closed';   Change = @{ '.claude/agents/rev.md' = New-Doc 'rev' 'Nothing to cite.' }; Expect = @('empty') }
        @{ Name = 'empty tree fails closed';            Change = $emptied; Expect = @('empty') * 5 }
    )
    $failed = 0
    foreach ($case in $cases) {
        $tree = $base.Clone()
        foreach ($k in $case.Change.Keys) {
            if ($null -eq $case.Change[$k]) { $tree.Remove($k) } else { $tree[$k] = $case.Change[$k] }
        }
        $result = Get-AgentDocFailure -Files $tree -IsIgnored { param($p) $p -like 'Docs/local/*' }
        $got = @($result.Failures | ForEach-Object Rule | Sort-Object) -join ','
        $want = @($case.Expect | Sort-Object) -join ','
        if ($got -ceq $want) { Write-Host "PASS  $($case.Name)" }
        else { Write-Host "FAIL  $($case.Name): expected [$want], got [$got]" -ForegroundColor Red; $failed++ }
    }
    Write-Host "$($cases.Count - $failed)/$($cases.Count) self-test cases passed."
    exit [int]($failed -gt 0)
}

if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
$clock = [System.Diagnostics.Stopwatch]::StartNew()
$listing = @(git -C $Root -c core.quotepath=off ls-files)
if ($LASTEXITCODE -ne 0) { throw "git ls-files failed in $Root" }
$tree = @{}
foreach ($p in $listing) {
    $full = Join-Path $Root $p
    $read = $p -match '^(\.claude/|\.agents/|\.codex/|CLAUDE\.md$|AGENTS\.md$)|\.(cpp|h)$' -and (Test-Path -LiteralPath $full -PathType Leaf)
    $tree[$p] = if ($read) { [string](Get-Content -LiteralPath $full -Raw) -replace "`r`n", "`n" } else { '' }
}
$result = Get-AgentDocFailure -Files $tree -IsIgnored {
    param($p)
    git -C $Root check-ignore -q -- $p
    $LASTEXITCODE -eq 0
}
foreach ($f in $result.Failures) {
    Write-Host "$($f.File):$($f.Line): $($f.Rule): $($f.Span)" -ForegroundColor Red
}
$c = $result.Counts
$seconds = $clock.Elapsed.TotalSeconds.ToString('F1', [cultureinfo]::InvariantCulture)
Write-Host "Checked $($c.Docs) docs, $($c.Skills) skills, $($c.Agents) agents, $($c.Paths) path and $($c.Identifiers) identifier spans in ${seconds}s: $($result.Failures.Count) failure(s)."
exit [int]($result.Failures.Count -gt 0)
