---
name: write-powershell
description: Write or change a PowerShell script in this repo — the language traps that fail
  silently, how scripts resolve the repository and each other, and the -SelfTest convention. Use
  when editing anything under Scripts/, build.ps1 or .githooks/, when a script "does nothing" or
  returns the wrong type, or when adding a self-test.
---

Every recurring defect in this repo's PowerShell has been a **language-semantics trap that fails
silently**, not a logic error, and each rule below is one that shipped as a bug. Scripts require
**PowerShell 7** (`pwsh`).

## Rules

Measured behaviour, examples and the bug each rule came from: `reference/traps.md`, same numbering.

1. **Wrap the call site: `$files = @(Get-TargetFiles)`.** `return` unrolls a one-element array to
   its element and an empty one to `$null`, so `.Count` throws under StrictMode. `return , $array`
   is the wrong fix: it breaks the two-element case.
2. **Name locals apart from every parameter** — `$tracked`, not `$all` beside `-All`. Names are
   case-insensitive, and a local shadows the parameter for every read in that function.
3. **Round explicitly:** `[math]::Truncate()`, `Floor()` or `Ceiling()`. `[int]` rounds
   half-to-even (`[int]2.5` is 2).
4. **Pipe every unassigned native call inside a function to `Out-Host`.** `git`, `pwsh` or engine
   stdout otherwise becomes part of the return value.
5. **`[AllowEmptyCollection()]` beside `Mandatory`** whenever an empty set is a legitimate input.
   `Mandatory` alone rejects `@()`.
6. **Invoke a script in-process (`& $path`) when its contract is an object.** `& pwsh -File` returns
   strings. `Get-ChangeTier.ps1` is the one that matters.
7. **Resolve the repository from `$PSScriptRoot`, and run your own worktree's copy.** It is the
   defining file's directory, even when dot-sourced, and the empty string only for fileless code, so
   a `$null` guard is dead.
8. **Dot-source only a shared library, kept flat in `Scripts/`.** A dot-sourced script shares your
   scope and its `exit` ends your session. The self-test census does not recurse into
   subdirectories.
9. **Read stdin through `[System.IO.StreamReader]::new([Console]::OpenStandardInput())`.**
   `[Console]::In.ReadLineAsync()` blocks on the calling thread, so a timed poll never times out.

## Self-tests

Every Build-tier script carries a `-SelfTest`, and `Validate-PrePR.ps1` fails without one. Keep
self-tests pure and toolchain-free: a table of cases plus one comparison loop, including the
falsification case, against a fixture repository when the behaviour is git-shaped. A dot-sourced
library gets a `$SelfTestCoverers` entry, not a parameter. Adding a script, a library or a case:
read `reference/self-test.md` first.

## Editing a `.ps1` from an agent shell

The `Bash` tool is Git Bash. Multi-line `sed`/bash substitutions mangle backslashes and
line-continuation backticks — write the edit as a small Python script instead. Validate without
executing:

```powershell
[System.Management.Automation.Language.Parser]::ParseInput($c, [ref]$t, [ref]$errors)
```

Invoke with `pwsh -File`; `cmd.exe /c "..."` swallows output, so a failing script looks like a
silent no-op.
