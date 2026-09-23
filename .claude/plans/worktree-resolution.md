# Remove-Worktree resolves its target from git's registry — Design

**Issue:** #610

## Goal

`Remove-Worktree.ps1` builds its target as `<main checkout>\.claude\worktrees\<Name>` and only then
consults `git worktree list`. A worktree Codex Desktop created at
`~\.codex\worktrees\<name>\StratChessEvolved` is therefore never found: the script reports no
directory, cleans nothing, and prints `Cleanup complete` (PR #607). Separately, the `-Branch` override
has never worked. The script assigns the local `$branch = $null` (`Remove-Worktree.ps1:124`), and
because PowerShell names are case-insensitive, that is the parameter `$Branch`. Post-merge cleanup is
supposed to be one script call for either agent, and today it is not.

## Scope

**This change will:**

- make `Remove-Worktree.ps1` resolve its target from `git worktree list --porcelain`, by `-Name`
  (Claude and Codex layouts) or by a new exact `-Path`;
- fix the `-Branch` shadowing;
- add a `-SelfTest` to `Remove-Worktree.ps1`, including one end-to-end run against a fixture
  repository;
- have `Get-Worktrees.ps1` print a working removal command for a worktree outside `.claude\worktrees`;
- update the `open-pull-request` cleanup step and `Docs/Workflow.md` → Worktree removal gotchas.

**This change will not:**

- change `New-Worktree.ps1`. It creates Claude-layout worktrees only, and Codex creates its own, so
  it has nothing to resolve.
- scan `~\.codex\worktrees` for unregistered directories. That root holds other repositories'
  worktrees, #149's residue was Claude-side, and Codex manages its own root.
- delete the empty `~\.codex\worktrees\<name>\` parent left after removal; Codex owns that layout.
- introduce a shared resolver library (D1).
- touch `Run-EloMatch.ps1`'s temporary worktrees or any engine or build behaviour.

## Decisions

### D1: Resolution lives in `Remove-Worktree.ps1`, not a shared library

Only `Remove-Worktree.ps1` resolves a target. `Get-Worktrees.ps1` already enumerates the registry and
lists the Codex worktree today (verified 2026-09-23 against `codex/uci-search-limits`), and
`New-Worktree.ps1` only creates. The issue suggests a resolver shared by the lifecycle scripts, but
it would have one caller. A dot-sourced library would also need a `$SelfTestCoverers` entry and
would share its caller's scope, which costs more than the duplication it would prevent. Rejected
until a second caller exists.

### D2: The registry is the authority; a directory name is a match rule on it

Resolution reads `git worktree list --porcelain` once, drops the main checkout, and compares paths
after normalising them (full path, `\` separators, no trailing separator, case-insensitive).

- **`-Name <n>`** matches a registered path `P` when either
  - **Claude layout:** `parent(P)` is `<main>\.claude\worktrees` and `leaf(P)` is `n`; or
  - **Codex layout:** `leaf(parent(P))` is `n` and `leaf(P)` is `leaf(<main>)`.

  The Codex rule keys on the layout, not on `~\.codex`, so nothing hard-codes a home-directory
  root.
  - **One match:** that is the target.
  - **Several:** refuse, list them, and point to `-Path`.
  - **None, but `<main>\.claude\worktrees\<n>` exists:** keep today's behaviour for that directory
    (an unregistered leftover).
  - **None and no directory:** FAIL with the registered worktree paths. This replaces today's
    `NOTE` followed by `Cleanup complete`, the silent success #610 reported.
- **`-Path <p>`** must equal a registered path after normalisation. Refuse the main checkout and any
  unregistered path, which covers a path registered to another repository (`git worktree list` never
  lists those). The message says which of the two applies.

`-Name` and `-Path` are separate parameter sets, and exactly one is required. The name the script
uses for the Trap 4 sibling-branch match is `leaf(P)` for the Claude layout and `leaf(parent(P))` for
the Codex layout. A `-Path` outside both layouts has no name, so Trap 4 is skipped and the script
says so. `Get-Worktrees.ps1` labels each worktree, and prints its removal command, with that same
name. Today it prints `leaf(P)`, which is `StratChessEvolved` for every Codex worktree.

Rejected alternatives:
- **Configured roots** (a list of worktree directories): one more setting to keep in step, and the
  registry already knows every path.
- **Recursive search of the user profile:** slow, and it would find other repositories' worktrees.

### D3: `-Branch` keeps its name; the local becomes `$resolvedBranch`

The public parameter stays `-Branch`, which callers and docs already use. The local is
`$resolvedBranch`, per `write-powershell` rule 2. Semantics:
- `-Branch` names the branch when the worktree is detached, replacing the sibling-branch guess.
- `-Branch` naming a branch other than the one the worktree has checked out is a FAIL before
  anything is deleted. Today's comment says the override "always wins", but letting it win would
  delete one branch while leaving the checked-out one behind.

### D4: The self-test pairs a pure resolver with one fixture-repository run

- **Pure resolver:** `Resolve-WorktreeTarget` takes the parsed registry entries, the main checkout,
  `-Name` or `-Path`, and `-Branch`, and returns a target or a refusal reason. It has no git calls,
  so a table of cases covers the matching rules cheaply.
- **Fixture run:** `-SelfTest` also builds a throwaway repository with a local bare `origin`:
  - a worktree in the Codex layout on branch `codex/foo`;
  - its commit merged into `main` and pushed;
  - then the script, run on it with `-Name foo`.

  It asserts that the directory, the registration, the local branch and the remote branch are all
  gone. That is issue acceptance criterion 1, and a type or path bug of the #394 kind shows only
  when the whole run goes against real git.
- Both parts use only git and `pwsh`, with no toolchain, per the self-test convention.

## Assumptions I cannot verify from the code

- **Codex Desktop tolerates external removal of a worktree it created.** It keeps its own state
  (`~\.codex\*.sqlite`). Not verified. Would be settled by the first real cleanup of a merged Codex
  worktree: Codex Desktop should show no error and should not recreate the directory. If it does
  either, the fallback is to leave the directory to Codex and remove only the branches (`-KeepDir`),
  which is a follow-up, not part of this change.
- **The Codex layout is `<root>\<name>\<repository directory>` for every Codex worktree.** It is
  seen in both known instances (#607's and `uci-search-limits`), but it is not documented by
  OpenAI. If it changes, `-Path` still works and `-Name` fails with the registered paths listed,
  never with a wrong deletion.

## Invariants

- Nothing is deleted unless the target is a registered worktree of this repository, or today's
  unregistered Claude-layout directory, and is never the main checkout.
- The ancestry check (`merge-base --is-ancestor … origin/main`) precedes every deletion, as today.
- Claude-layout behaviour is unchanged: `-Name`, Traps 1–5, `-Force`, `-KeepRemote`, `-SyncMaster`
  and `-FromInside`.
- An ambiguous or unknown target exits non-zero and deletes nothing.

## Validation

Tooling tier: `Get-ChangeTier.ps1` classifies both changed scripts as Tooling. No Elo or nps
measurement is needed, because no engine or build file changes.

| Risk | Evidence that closes it |
|---|---|
| Matching rules wrong | `-SelfTest` resolver table: Claude name; Codex name; same name in both layouts → ambiguous; `-Path` with case and separator variants → accepted; `-Path` of the main checkout, of an unregistered directory → refused; unknown `-Name` with no directory → FAIL |
| `-Branch` still shadowed | Table cases: detached worktree plus `-Branch` → that branch; `-Branch` conflicting with the checked-out branch → refusal |
| Whole run broken against real git | `-SelfTest` fixture run on the Codex layout: directory, registration, local branch and remote branch all gone |
| Claude layout regressed | Fixture run repeated on a `.claude\worktrees\<n>` worktree |
| `Get-Worktrees.ps1` label wrong | Its output labels the live Codex worktree `uci-search-limits`, not `StratChessEvolved` |
| Test passes against the broken script | Revert the resolution change and watch the fixture run go red (a new test must fail against the old code) |
| Real Codex worktree | After merge, remove the next merged Codex worktree with one call, and check Codex Desktop afterwards (the assumption above) |

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Registry is the authority; the two layout rules; `-Path` refusals | `Remove-Worktree.ps1` help (`.DESCRIPTION`, `.PARAMETER Name`/`Path`) |
| `-Branch` conflict is a refusal, not an override | `.PARAMETER Branch` help |
| Codex cleanup example | `open-pull-request` step 5; `Docs/Workflow.md` → Worktree removal gotchas |
| No shared resolver until a second caller (D1) | PR body |
| Codex Desktop's response to external removal, once observed | `Docs/Workflow.md` → Worktree removal gotchas |
