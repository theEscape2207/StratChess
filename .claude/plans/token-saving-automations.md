# Token-Saving Claude Code Automations

## Goal
Reduce weekly token consumption so more development sessions fit within the limit.
Secondary goal: improve engine quality assurance via a search-algorithm reviewer subagent.

Sorted by token-reduction impact. GitHub MCP excluded (low impact for this workflow).

---

## Status Summary

| Item | Type | Token Impact | Status |
|---|---|---|---|
| Subagent-driven development | Skill (installed) | Very High | ✅ Use consistently |
| Dispatching parallel agents | Skill (installed) | Very High | ✅ Use consistently |
| Fail-only build hook | Hook | High | ❌ Removed — clangd-lsp already provides inline diagnostics with lower token cost |
| Verification-before-completion | Skill (installed) | High | ✅ Use consistently |
| `run-tests` skill | Skill (create) | Medium | ✅ Created — `.claude/skills/run-tests/SKILL.md` |
| Disable explanatory style | Plugin toggle | Medium | ❌ Not done — manual habit |
| `self-play-validate` skill | Skill (create) | Low-Medium | ✅ Created — `.claude/skills/self-play-validate/SKILL.md` |
| Search-algorithm reviewer | Subagent (create) | Quality / isolation | ✅ Created — `.claude/agents/search-reviewer.md` |

---

## 1. Subagent-Driven Development + Dispatching Parallel Agents
**Token impact: Very High**
**Status: Already installed — needs consistent use**

Subagents run in isolated context windows and do not carry the full conversation history.
When Claude implements a plan by dispatching subagents (implement + write tests + update docs),
each agent only sees its task and the files it needs. The main conversation receives a summary.

- Invoke `superpowers:subagent-driven-development` when executing any multi-step plan
- Invoke `superpowers:dispatching-parallel-agents` when 2+ tasks are independent
  (e.g. "implement LMR + write [search] tests" — both can run in parallel)

No files to create.

---

## 2. Fail-Only Build Hook
**Token impact: High**
**Status: Probe test pending (hook-probe.txt test in progress)**

Eliminates the manual "let me build to check" turn after every edit.
Critical constraint: hook output must be filtered to errors only — verbose MSBuild output
on every edit would increase tokens, not reduce them.

### Files
- `.claude/settings.json` — hook configuration
- `build-check.ps1` — PowerShell wrapper (already created at worktree root)

### Probe test
After restarting in Claude Code CLI, make any edit and check for `C:\Users\thees\hook-probe.txt`.
If it exists: hooks are working, replace probe command with real build command.
If it does not exist: investigate settings.json location or CLI version hook support.

### Final settings.json hook command (replace probe once confirmed working)
```json
{
  "hooks": {
    "PostToolUse": [
      {
        "matcher": "Edit|Write",
        "hooks": [
          {
            "type": "command",
            "command": "powershell -ExecutionPolicy Bypass -NoProfile -File build-check.ps1",
            "description": "Incremental compile check — silent on success, errors-only on failure"
          }
        ]
      }
    ]
  }
}
```

### build-check.ps1 behaviour
- Runs `.\build.ps1 tests` (builds test project only — faster than full solution)
- On success (exit 0): no output
- On failure: outputs only lines matching `error` (last 15 lines max)
- Exit code propagated so Claude receives it as a tool error

---

## 3. Verification-Before-Completion
**Token impact: High**
**Status: Already installed — needs consistent use**

Prevents the most expensive token pattern: Claude claims done → compile error found →
explain → fix → re-verify. Each re-work cycle replays the full conversation context.

- Invoke `superpowers:verification-before-completion` before claiming any task complete
- Runs `.\build.ps1 run-tests` and confirms output before any success claim

No files to create.

---

## 4. `run-tests` Skill
**Token impact: Medium**
**Status: Not created**

Eliminates clarification rounds about test invocation and prevents Claude from re-reading
CLAUDE.md's build section to resolve invocation quirks (PowerShell double-slash flags,
working directory requirements, tag syntax).

### File to create: `.claude/skills/run-tests/SKILL.md`
```markdown
---
name: run-tests
description: Build and run StratChessTests with optional Catch2 tag filter. Use when the
  user asks to run tests, verify a change, or check test results.
---

Build the test project, then run with an optional tag filter.

## Steps

1. Build tests:
   \`\`\`
   .\build.ps1 tests
   \`\`\`

2. Run all tests (no filter):
   \`\`\`
   StratChessTests\x64\Release\StratChessTests.exe
   \`\`\`
   Or with a tag filter (e.g. [sort], [tactical], [perft], [tt], [eval], [repetition],
   [formatter], [board]):
   \`\`\`
   StratChessTests\x64\Release\StratChessTests.exe [tag]
   \`\`\`

3. Report: pass/fail count, any assertion failures, total assertions.

## Notes
- Run from worktree root (not StratChessTests/ subdirectory)
- Binary is under StratChessTests\x64\Release\ (not x64\Release\)
- Build with /v:normal instead of /v:minimal when diagnosing build errors
```

---

## 5. Disable Explanatory Output Style During Implementation
**Token impact: Medium**
**Status: Not done**

The `explanatory-output-style` plugin adds insight boxes and educational context
to every response — valuable when learning, but adds ~200-500 output tokens per
response during focused implementation sessions.

### Action
Toggle off at the start of implementation-heavy sessions.
Toggle back on for review/learning sessions.

This is a manual toggle in Claude Desktop / CLI session settings — no file to create.

---

## 6. `self-play-validate` Skill
**Token impact: Low-Medium**
**Status: Not created**

Encodes the self-play validation workflow so Claude can execute it without re-reading
CLAUDE.md's self-play section or hitting the working-directory gotcha.

### File to create: `.claude/skills/self-play-validate/SKILL.md`
```markdown
---
name: self-play-validate
description: Run an AI vs AI self-play game to validate search behavior after changes
  to AIPerplex, evaluation, or move ordering. Use after any search-related change.
---

Run a headless AI vs AI game and verify expected log output.

## Setup
1. Ensure working directory is `StratChessEvolved/` (not repo root, not worktree root)
2. Ensure `StratChessEvolved/game_settings.json` has:
   - Both players set to `"type": 6` (AIPerplex vs AIPerplex)
   - FEN set to starting position (not a test FEN left over from a session)
   - `logs/` subdirectory exists under `StratChessEvolved/`

## Run
```powershell
$proc = Start-Process ..\x64\Release\StratChessEvolved.exe `
  -PassThru -NoNewWindow -RedirectStandardOutput out.txt
$proc.WaitForExit(30000)   # 30s timeout; kill if needed
Get-Content out.txt
```

## Verify
Each move should log: `GetMove complete: move=..., depth=..., time=...ms, nodes=..., stable=...`
Game should terminate on checkmate or stalemate.
No infinite loops, crashes, or missing move logs.

## Notes
- game_settings.json uses C-style /* */ comments — do NOT parse with PowerShell ConvertFrom-Json
- Reset FEN to starting position before committing
```

---

## 7. Search-Algorithm Reviewer Subagent
**Token impact: Quality assurance / context isolation**
**Status: Not created — implement after LMR is merged**

Runs a focused review of search algorithm changes in an isolated subagent context.
Prevents long search-correctness review discussions from inflating the main conversation.

### File to create: `.claude/agents/search-reviewer.md`
```markdown
---
name: search-reviewer
description: Review changes to AIPerplex search algorithm for correctness, ELO impact,
  and adherence to engine invariants. Dispatch after any change to pvs(), qsearch(),
  move ordering, pruning conditions, or evaluation integration.
---

You are a chess engine search algorithm reviewer with expertise in alpha-beta search,
move ordering heuristics, and ELO impact analysis.

## Your Task
Review the diff or files provided and evaluate:

### Correctness
- Alpha-beta window management: are alpha/beta updates applied correctly?
- Fail-soft vs fail-hard consistency
- TT probe/store conditions: flag types (EXACT/LOWER/UPPER), depth conditions
- Quiescence search: delta pruning threshold, stand-pat logic, capture-only filtering
- Repetition detection: is it checked at the right points?
- Null move pruning conditions (if present): zugzwang risk, verification search
- LMR conditions (if present): which moves are reduced, by how much, re-search logic

### Move Ordering Impact
- Are killer moves applied at the right ply?
- History table update conditions (only on beta cutoff, not all quiet moves)
- MVV-LVA or SEE scoring consistency

### ELO Impact Assessment
- Expected direction of change (positive / neutral / regression risk)
- Which positions or tactical patterns are most affected
- Any risk of search instability (score oscillation, depth oscillation)

### Invariants That Must Hold
- sizeof(Move) == 2
- TT access is thread-safe (per-bucket shared_mutex)
- No raw board mutation without DoMove/UndoMove symmetry
- Deterministic behavior: same position + same depth = same result

## Output Format
1. **Verdict**: LGTM / Needs Changes / Blocking Issue
2. **Correctness findings** (numbered, with file:line references)
3. **ELO assessment** (1-2 sentences)
4. **Invariant check** (pass/fail per invariant)
5. **Suggested follow-up tests** (Catch2 tags or specific positions to verify)
```

---

## Implementation Order

1. ✅ Confirm probe hook works (current task) → replace with real build hook
2. Create `run-tests` skill (15 min)
3. Create `self-play-validate` skill (20 min)
4. Create `search-reviewer` subagent after LMR lands
5. Explanatory style toggle — manual habit, no file needed
