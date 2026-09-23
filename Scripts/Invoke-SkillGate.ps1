<#
.SYNOPSIS
    PreToolUse hook: interrupts the first edit per session to a file type that has a skill.

.DESCRIPTION
    A skill's description is matched against the task, not the file being edited. This hook fires
    on the edit itself: the first edit to a matching file in a session is denied with "load skill
    X, then retry"; the retry and every later edit pass.

    It interrupts; it does not prove a load. The marker records the denial, so a retry without
    loading the skill also passes. A positive load signal was rejected because Codex loads a skill
    by reading its file, which raises no event, and a missed signal would block every edit.

    Fails open: a missing session id, unparseable input, an unwritable marker or any error allows
    the edit. The marker is created with CreateNew, so concurrent edits yield one denial.

    Reads the hook JSON on stdin from either host:
      Claude Code  Edit/Write          tool_input.file_path
      Codex        apply_patch         tool_input.command (every "*** ... File:" line)
      lean-ctx     ctx_patch           tool_input.path, tool_input.ops[].path

    Not covered: edits made through a shell. ctx_patch is a per-user MCP server, so the tracked
    config does not match it; to opt in, add a PreToolUse matcher `mcp__lean-ctx__ctx_patch` to
    ~/.claude/settings.json running this script. It then costs a pwsh start on every patch.

    $SkillFileSets is the single list of patterns. The Claude config repeats each one as an
    Edit(...) and a Write(...) `if` rule, and -SelfTest fails when the two drift apart.

.PARAMETER SelfTest
    Run the decision cases, the concurrent-marker case and the hook-config checks, then exit.

.HOW TO INVOKE
    Registered in .claude/settings.json and .codex/hooks.json; not run by hand.
    pwsh -File Scripts/Invoke-SkillGate.ps1 -SelfTest
#>

[CmdletBinding()]
param(
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path $PSScriptRoot -Parent
$markerRoot = Join-Path ([IO.Path]::GetTempPath()) 'stratchess-skill-gate'

# Claude permission-rule patterns, used verbatim as the config's `if` rules. They resolve against
# the session's working directory, hence the `**/` prefixes: a session started in a subdirectory
# or another worktree still matches.
$script:SkillFileSets = [ordered]@{
    'write-powershell'   = @('**/*.ps1')
    'writing-for-agents' = @('**/CLAUDE.md', '**/AGENTS.md', '**/.claude/skills/**', '**/.agents/skills/**',
                             '**/.claude/agents/**', '**/.codex/agents/**')
}

function ConvertTo-PathRegex {
    param([Parameter(Mandatory)][string]$Pattern)

    $rx = [regex]::Escape($Pattern)
    $rx = $rx.Replace('\*\*/', '(?:.*/)?').Replace('/\*\*', '/.*').Replace('\*', '[^/]*')
    return "^$rx$"
}

function Get-Field {
    param($Object, [Parameter(Mandatory)][string]$Name)

    if ($null -eq $Object) { return $null }
    $prop = $Object.PSObject.Properties[$Name]
    if ($prop) { return $prop.Value }
    return $null
}

function Get-EditedPath {
    param([Parameter(Mandatory)]$ToolInput)

    $found = @()
    foreach ($name in 'file_path', 'path') {
        $value = Get-Field $ToolInput $name
        if ($value -is [string] -and $value) { $found += $value }
    }
    foreach ($op in @(Get-Field $ToolInput 'ops')) {
        $value = Get-Field $op 'path'
        if ($value -is [string] -and $value) { $found += $value }
    }
    $patch = Get-Field $ToolInput 'command'
    if ($patch -is [string]) {
        $rx = '(?m)^\*\*\* (?:Update File|Add File|Delete File|Move to): (.+?)\s*$'
        foreach ($m in [regex]::Matches($patch, $rx)) { $found += $m.Groups[1].Value }
    }
    return $found
}

# Repo-relative with forward slashes, or $null for a path outside the repository.
function Get-RepoRelativePath {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Base,
        [Parameter(Mandatory)][string]$Root
    )

    $full = [IO.Path]::GetFullPath($Path, $Base)
    $rel = [IO.Path]::GetRelativePath($Root, $full)
    if ($rel -eq '.' -or $rel.StartsWith('..') -or [IO.Path]::IsPathRooted($rel)) { return $null }
    return $rel.Replace('\', '/')
}

# True only for the call that creates the marker: CreateNew is atomic create-if-absent.
function New-GateMarker {
    param(
        [Parameter(Mandatory)][string]$Directory,
        [Parameter(Mandatory)][string]$Session,
        [Parameter(Mandatory)][string]$Skill
    )

    $safeSession = $Session -replace '[^A-Za-z0-9_-]', '_'
    try {
        $null = [IO.Directory]::CreateDirectory($Directory)
        [IO.File]::Open((Join-Path $Directory "$safeSession.$Skill"), [IO.FileMode]::CreateNew).Dispose()
        return $true
    }
    catch {
        return $false
    }
}

# The hook's stdout: deny JSON, or '' to allow. Every failure allows.
function Get-GateOutput {
    param(
        [AllowEmptyString()][string]$Json,
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$MarkerDir
    )

    try {
        $hook = $Json | ConvertFrom-Json
        $session = Get-Field $hook 'session_id'
        $toolInput = Get-Field $hook 'tool_input'
        if (-not ($session -is [string] -and $session) -or $null -eq $toolInput) { return '' }
        $cwd = Get-Field $hook 'cwd'
        if (-not ($cwd -is [string] -and $cwd)) { $cwd = $Root }

        $hits = [ordered]@{}
        foreach ($edited in @(Get-EditedPath $toolInput)) {
            $rel = Get-RepoRelativePath -Path $edited -Base $cwd -Root $Root
            if (-not $rel) { continue }
            foreach ($skill in $script:SkillFileSets.Keys) {
                if ($hits.Contains($skill)) { continue }
                foreach ($pattern in $script:SkillFileSets[$skill]) {
                    if ($rel -match (ConvertTo-PathRegex $pattern)) { $hits[$skill] = $rel; break }
                }
            }
        }

        $due = @(foreach ($skill in $hits.Keys) {
                if (New-GateMarker -Directory $MarkerDir -Session $session -Skill $skill) { $skill }
            })
        if ($due.Count -eq 0) { return '' }

        $asks = @(foreach ($skill in $due) { 'load skill `{0}` before editing {1}' -f $skill, $hits[$skill] })
        $reason = 'Skill gate: ' + ($asks -join '; ') +
            ', then retry this edit. The gate fires once per skill per session.'
        $decision = @{
            hookSpecificOutput = @{
                hookEventName            = 'PreToolUse'
                permissionDecision       = 'deny'
                permissionDecisionReason = $reason
            }
        }
        return ($decision | ConvertTo-Json -Compress -Depth 3)
    }
    catch {
        return ''
    }
}

# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------
if ($SelfTest) {
    $failed = 0
    $testDir = Join-Path ([IO.Path]::GetTempPath()) "skill-gate-selftest-$([guid]::NewGuid())"
    $ps1Abs = Join-Path $repoRoot 'Scripts\Run-Tests.ps1'
    $outside = Join-Path ([IO.Path]::GetTempPath()) 'elsewhere\tool.ps1'

    function New-HookJson {
        param([string]$Session, [string]$Tool, [hashtable]$ToolInput)
        $hook = [ordered]@{ hook_event_name = 'PreToolUse'; cwd = $repoRoot; tool_name = $Tool; tool_input = $ToolInput }
        if ($Session) { $hook.session_id = $Session }
        return ($hook | ConvertTo-Json -Depth 5)
    }

    $patchOne = "*** Begin Patch`n*** Update File: Docs/Workflow.md`n@@`n-a`n+b`n*** Add File: Scripts/New-Thing.ps1`n+x`n*** End Patch"
    $patchTwo = "*** Begin Patch`n*** Update File: CLAUDE.md`n@@`n-a`n+b`n*** Update File: Scripts/Run-Tests.ps1`n@@`n-a`n+b`n*** End Patch"

    $cases = @(
        @{ Name = 'Claude Edit, absolute .ps1 -> write-powershell';  Json = (New-HookJson 's1' 'Edit' @{ file_path = $ps1Abs });                                 Expect = @('write-powershell') }
        @{ Name = 'FALSIFY: same session again -> allowed';          Json = (New-HookJson 's1' 'Edit' @{ file_path = $ps1Abs });                                 Expect = @() }
        @{ Name = 'Claude Write, new .ps1 -> write-powershell';      Json = (New-HookJson 's12' 'Write' @{ file_path = 'Scripts/New-Thing.ps1'; content = 'x' });  Expect = @('write-powershell') }
        @{ Name = 'Claude Write, new SKILL.md -> writing-for-agents'; Json = (New-HookJson 's2' 'Write' @{ file_path = '.claude/skills/new/SKILL.md'; content = 'x' }); Expect = @('writing-for-agents') }
        @{ Name = 'skill reference file -> writing-for-agents';      Json = (New-HookJson 's3' 'Edit' @{ file_path = '.claude/skills/write-powershell/reference/traps.md' }); Expect = @('writing-for-agents') }
        @{ Name = 'nested CLAUDE.md -> writing-for-agents';          Json = (New-HookJson 's4' 'Edit' @{ file_path = 'StratEngine/CLAUDE.md' });                 Expect = @('writing-for-agents') }
        @{ Name = 'Docs/agents is not .claude/agents -> allowed';    Json = (New-HookJson 's5' 'Edit' @{ file_path = 'Docs/agents/simplify.md' });               Expect = @() }
        @{ Name = 'plain doc -> allowed';                            Json = (New-HookJson 's6' 'Edit' @{ file_path = 'Docs/Workflow.md' });                      Expect = @() }
        @{ Name = 'Codex patch, one of two files -> write-powershell'; Json = (New-HookJson 's7' 'apply_patch' @{ command = $patchOne });                        Expect = @('write-powershell') }
        @{ Name = 'Codex patch, two skills at once';                 Json = (New-HookJson 's8' 'apply_patch' @{ command = $patchTwo });                          Expect = @('write-powershell', 'writing-for-agents') }
        @{ Name = 'ctx_patch ops batch -> writing-for-agents';       Json = (New-HookJson 's9' 'mcp__lean-ctx__ctx_patch' @{ ops = @(@{ path = 'AGENTS.md' }) }); Expect = @('writing-for-agents') }
        @{ Name = 'path outside the repo -> allowed';                Json = (New-HookJson 's10' 'Edit' @{ file_path = $outside });                               Expect = @() }
        @{ Name = 'no session id -> allowed';                        Json = (New-HookJson '' 'Edit' @{ file_path = $ps1Abs });                                   Expect = @() }
        @{ Name = 'malformed JSON -> allowed';                       Json = '{not json';                                                                          Expect = @() }
        @{ Name = 'unwritable marker dir -> allowed';                Json = (New-HookJson 's11' 'Edit' @{ file_path = $ps1Abs }); MarkerDir = $ps1Abs;             Expect = @() }
    )

    foreach ($c in $cases) {
        $dir = if ($c.ContainsKey('MarkerDir')) { $c.MarkerDir } else { $testDir }
        $out = Get-GateOutput -Json $c.Json -Root $repoRoot -MarkerDir $dir
        $got = @()
        if ($out) {
            $reason = ($out | ConvertFrom-Json).hookSpecificOutput.permissionDecisionReason
            $got = @([regex]::Matches($reason, '`([a-z-]+)`') | ForEach-Object { $_.Groups[1].Value })
        }
        if ((@($got | Sort-Object) -join ',') -eq (@($c.Expect | Sort-Object) -join ',')) {
            Write-Host "  PASS  $($c.Name)"
        }
        else {
            Write-Host "  FAIL  $($c.Name): denied [$($got -join ', ')], expected [$($c.Expect -join ', ')]"
            $failed++
        }
    }

    # Concurrent matching edits in one session: exactly one call may create the marker.
    $markerDef = ${function:New-GateMarker}.ToString()
    $created = @(1..8 | ForEach-Object -ThrottleLimit 8 -Parallel {
            ${function:New-GateMarker} = $using:markerDef
            New-GateMarker -Directory $using:testDir -Session 'parallel' -Skill 'write-powershell'
        } | Where-Object { $_ })
    if ($created.Count -eq 1) { Write-Host '  PASS  8 concurrent markers -> one denial' }
    else { Write-Host "  FAIL  8 concurrent markers -> $($created.Count) denials, expected 1"; $failed++ }

    # The Claude config must carry an Edit and a Write rule for every pattern, and no other.
    $claude = Get-Content -LiteralPath (Join-Path $repoRoot '.claude/settings.json') -Raw | ConvertFrom-Json
    $rules = @(foreach ($group in @($claude.hooks.PreToolUse)) {
            foreach ($h in @($group.hooks)) {
                if ((@($h.args) -join ' ') -like '*Invoke-SkillGate.ps1*') { $h.if }
            }
        })
    $wanted = @(foreach ($pattern in @($script:SkillFileSets.Values | ForEach-Object { $_ })) {
            "Edit($pattern)"; "Write($pattern)"
        })
    $missing = @($wanted | Where-Object { $rules -notcontains $_ })
    $extra = @($rules | Where-Object { $wanted -notcontains $_ })
    if ($missing.Count -eq 0 -and $extra.Count -eq 0) {
        Write-Host "  PASS  .claude/settings.json covers all $($wanted.Count) rules"
    }
    else {
        Write-Host "  FAIL  .claude/settings.json: missing [$($missing -join ', ')], extra [$($extra -join ', ')]"
        $failed++
    }

    $codex = Get-Content -LiteralPath (Join-Path $repoRoot '.codex/hooks.json') -Raw | ConvertFrom-Json
    $codexHooked = @(foreach ($group in @($codex.hooks.PreToolUse)) {
            if ($group.matcher -eq 'apply_patch') {
                @($group.hooks) | Where-Object { $_.command -like '*Invoke-SkillGate.ps1*' }
            }
        })
    if ($codexHooked.Count -eq 1) { Write-Host '  PASS  .codex/hooks.json runs the gate on apply_patch' }
    else { Write-Host "  FAIL  .codex/hooks.json: $($codexHooked.Count) gate hooks on apply_patch, expected 1"; $failed++ }

    Remove-Item -LiteralPath $testDir -Recurse -Force -ErrorAction SilentlyContinue
    if ($failed) { Write-Host "Invoke-SkillGate self-test: $failed FAILED"; exit 1 }
    Write-Host 'Invoke-SkillGate self-test: all passed'
    exit 0
}

# ---------------------------------------------------------------------------
# Hook
# ---------------------------------------------------------------------------
try {
    $reader = [IO.StreamReader]::new([Console]::OpenStandardInput(), [Text.UTF8Encoding]::new($false))
    $out = Get-GateOutput -Json $reader.ReadToEnd() -Root $repoRoot -MarkerDir $markerRoot
    if ($out) { [Console]::Out.Write($out) }
}
catch {
    # Fail open: a broken gate must never stop edits.
}
exit 0
