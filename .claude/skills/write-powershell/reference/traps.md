# Traps: evidence and examples

Each section backs one rule in `SKILL.md` with the measured behaviour and the bug it shipped as.

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

## 6. `& pwsh -File` returns strings, not objects

`& pwsh -File script.ps1` returns strings. Process boundaries serialise through
stdout. A script whose contract is an object must be invoked in-process:

```powershell
$tier = & pwsh -File $t   # String    -> $tier.Tier throws
$tier = & $t              # PSCustomObject -> $tier.Tier is 'Docs'
```

`Get-ChangeTier.ps1` is the one that matters: it returns a `PSCustomObject`, so callers dot-invoke.

## 7. `$PSScriptRoot` is the defining file's directory

It holds in every case that has a file. Dot-sourcing
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

## 8. Dot-source only a shared library

The reason to prefer `-File` over dot-sourcing is scope, not paths: a dot-sourced script runs in the
caller's scope, so its variables and functions collide with yours and its `exit` terminates *your*
session. Dot-source only a deliberate shared library. There are two — `BuildFreshness.ps1`
(`build.ps1`, `Get-BuildArtifact.ps1`) and `UciDriver.ps1` (`Compare-SearchEquivalence.ps1`,
`Run-Bench.ps1`) — both taken via `Join-Path $PSScriptRoot`.

Keep a new one **flat in `Scripts/`**, not in a subdirectory. The Build-tier self-test census is
`Get-ChildItem Scripts -Filter *.ps1` with no `-Recurse`, so a library one level down is never asked
whether it needs a `-SelfTest` — it does not fail that check, it escapes it.

## 9. `[Console]::In.ReadLineAsync()` is not asynchronous

`Console.In` is a *synchronized* `TextReader`, and its async methods run the read on the
calling thread and hand back an already-completed task. So a timed poll silently becomes an
untimed one:

```powershell
$t = [Console]::In.ReadLineAsync()
$t.Wait(300)      # blocks until a line arrives, however long that takes
```

Read from the handle instead — a plain `StreamReader` is genuinely async:

```powershell
$stdin = [System.IO.StreamReader]::new([Console]::OpenStandardInput())
```

`FakeUciEngine.ps1` needs this to model an engine noticing a stop mid-search. The
`$proc.StandardOutput` of a redirected child process is already a plain `StreamReader`, so
`UciDriver.ps1` is unaffected.
