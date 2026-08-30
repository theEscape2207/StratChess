---
name: write-powershell
description: Write or change a PowerShell script in this repo — the language traps that fail
  silently, how scripts resolve the repository and each other, and the -SelfTest convention. Use
  when editing anything under Scripts/, build.ps1 or .githooks/, when a script "does nothing" or
  returns the wrong type, or when adding a self-test.
---

PowerShell is ~7,500 lines here, second only to C++. Every recurring defect in it has been a
**language-semantics trap that fails silently**, not a logic error. The measured behaviours below
are what the reviews keep finding; each one is a real bug this repo has shipped.

Scripts require **PowerShell 7** (`pwsh`). Windows PowerShell 5 (`powershell`) fails on the syntax
used throughout.

## 1. A collection of one is not a collection

`return` unrolls. Measured on 7.6.5:

| Written in the function | What the caller receives | `.Count` under StrictMode |
|---|---|---|
| `return @('only')` | `String` | **throws** |
| `return @()` | `$null` | **throws** |
| `return @('a','b')` | `Object[]` | 2 |

**Fix at the call site, always: `$files = @(Get-TargetFiles)`.** That makes all three cases an array
of the right length.

Do **not** "fix" it with the comma operator. `return , $array` yields a one-element `Object[]`
wrapping the original, and `@()` does not flatten it: a two-element result arrives as `Count = 1`
with `[0]` being the inner array. It only looks correct in the one-element case that prompted it.

This shipped as #394 — `$files.Count` threw on any change touching exactly one file, so the lint
gate crashed on precisely the smallest PRs.

## 2. Variable names are case-insensitive, so a local shadows a parameter

`$all` and `$All` are one name. A function-local assignment does not write to the script parameter —
it creates a local that **shadows it for every read inside that function**:

```powershell
param([switch]$All)                 # script scope, stays $false

function Get-TargetFiles {
    $all = @('a.cpp', 'b.cpp')      # intended as a local
    if ($All) { return $all }       # reads the local: non-empty array is truthy -> taken
}
```

The switch itself is never modified (`$script:All` is still `$false` afterwards), which is why this
survives inspection. It shipped as #387: whole-tree lint became unconditional, so changed-file
scoping never engaged and CI lint became the critical path.

**Name every local so it cannot collide with a parameter of the enclosing script or function** —
`$tracked`, not `$all`. `Set-StrictMode` does not catch this; nothing does.

## 3. `[int]` rounds half-to-even

Not truncation, and not the "round half up" people assume:

| Expression | Result |
|---|---|
| `[int]2.5` | **2** |
| `[int]3.5` | **4** |
| `[int]2.7` | 3 |
| `[math]::Truncate(2.7)` | 2 |

Use `[math]::Truncate()`, `[math]::Floor()` or `[math]::Ceiling()` and say which you mean. This
matters wherever a count is derived from a ratio — worker counts, sample sizes, timeouts.

## 4. A function returns native-command stdout too

Anything a function writes to the success stream is part of its return value, including the stdout
of `git`, `pwsh` or the engine:

```powershell
function Get-Verdict {
    & git rev-parse --abbrev-ref HEAD    # leaks into the return value
    return 'VERDICT'
}
$v = Get-Verdict                          # -> @('worktree-x', 'VERDICT'), Count = 2
```

**Pipe every unassigned native call inside a function to `Out-Host`** (or assign it, or `| Out-Null`).
`Write-Host` is already safe — it does not write to the success stream.

## 5. `Mandatory` is what rejects an empty collection

A plain `[string[]]$Files` accepts `@()` happily. Adding `[Parameter(Mandatory)]` makes it throw
*"Cannot bind argument to parameter 'Files' because it is an empty array"*:

```powershell
param([Parameter(Mandatory)][AllowEmptyCollection()][string[]]$ChangedFiles)
```

An empty diff is an ordinary answer, not a caller error. Add `[AllowEmptyCollection()]` whenever an
empty set is a legitimate input — the other half of #387.

## 6. Script boundaries: what crosses and what does not

**`& pwsh -File script.ps1` returns strings, not objects.** Process boundaries serialise through
stdout. A script whose contract is an object must be invoked in-process:

```powershell
$tier = & pwsh -File $t   # String    -> $tier.Tier throws
$tier = & $t              # PSCustomObject -> $tier.Tier is 'Docs'
```

`Get-ChangeTier.ps1` is the one that matters: it returns a `PSCustomObject`, so callers dot-invoke.

**`$PSScriptRoot` is the defining file's own directory, in every case that has a file.** Dot-sourcing
does not change it — a dot-sourced library sees *its own* directory, not the caller's, both at file
scope and inside the functions it defines. It is the **empty string** (never `$null`) only for code
with no backing file: `pwsh -Command '...'`, a bare script block, `Invoke-Expression`. So a
`if ($null -eq $PSScriptRoot)` guard is dead code.

Because scripts resolve the repository from their own `$PSScriptRoot`, **always invoke the copy in
your own worktree** — a sibling worktree's copy operates on that worktree's repo and will report a
confident, wrong answer.

```
pwsh -ExecutionPolicy Bypass -File C:\...\<your-worktree>\Scripts\<name>.ps1
```

The reason to prefer `-File` over dot-sourcing is scope, not paths: a dot-sourced script runs in the
caller's scope, so its variables and functions collide with yours and its `exit` terminates *your*
session. Dot-source only a deliberate shared library — `BuildFreshness.ps1` is the existing one,
dot-sourced by `build.ps1` and `Get-BuildArtifact.ps1` via `Join-Path $PSScriptRoot`.

## 7. The `-SelfTest` convention

Scripts carry a `-SelfTest` switch. `Validate-PrePR.ps1` discovers them by parameter introspection
and runs the self-test of any script your change touches, so a new one is picked up with no
registration step. Keep them **pure and toolchain-free** — that property is why the pre-commit hook
can run them.

Two rules are enforced rather than suggested:

- **Every Build-tier script must have one.** Those scripts gate validation itself, so a bug in one
  can exempt a change from the checks and then decline to report it. `Validate-PrePR.ps1` fails on a
  Build-tier script without a `-SelfTest`, on every tier including the Docs and Tooling fast paths.
  A dot-sourced library that has no `param()` block to hang a switch on needs an exemption entry
  naming the script that covers it — and that covering script is checked too.
- **The whole set runs nightly**, via `Validate-PrePR.ps1 -AllSelfTests`. The PR gate only reaches
  the scripts a diff touched, so a script broken from elsewhere would otherwise stay broken until
  someone next edited it.

The dominant idiom is a table of cases plus one comparison loop, not assertion helpers:

```powershell
if ($SelfTest) {
    $cases = @(
        @{ Name = 'validator -> Build NOT Tooling'; Files = @('Scripts/Validate-PrePR.ps1'); Expect = 'Build' }
        @{ Name = 'docs + cpp -> Engine';           Files = @('CLAUDE.md', 'Eval.cpp');      Expect = 'Engine' }
    )
    ...
}
```

Two rules for the cases themselves:

- **Include the falsification case.** A test that only asserts the success path proves nothing —
  `build.ps1`'s freshness cases assert that a stale artifact *fails and names the file that made it
  stale*, which is the situation the check exists for.
- **Use a fixture repository over mocks** when the behaviour is git-shaped. #394's bug was a wrong
  *type* with right *content*; only spawning the script against a real fixture caught it.

## Editing a `.ps1` from an agent shell

The `Bash` tool is Git Bash. Multi-line `sed`/bash substitutions mangle backslashes and
line-continuation backticks — write the edit as a small Python script instead. Validate without
executing:

```powershell
[System.Management.Automation.Language.Parser]::ParseInput($c, [ref]$t, [ref]$errors)
```

Never wrap a script in `cmd.exe /c "..."` — it swallows output, so a failing script looks like a
silent no-op.
