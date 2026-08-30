# Self-test coverage as an enforced rule — Design

**Issue:** #395 (items 2 and 3), #364 (item 4)

## Goal

Seven scripts carry a `-SelfTest` switch and `Validate-PrePR.ps1` runs the self-test of any changed
script that has one. Nothing requires a script to have one, so the scripts that gate validation
itself are the ones missing it: `Validate-PrePR.ps1`, `Validate-PreCommit.ps1` and
`New-PullRequest.ps1` have no self-test, and `Validate-PrePR.ps1`'s discovery **skips any script
lacking one** — so a change to the validator runs no test of the validator. That is the
self-concealment hazard `Get-ChangeTier.ps1` already names as its reason for putting those files in
Build tier, left unguarded on the test side.

Separately, a self-test only ever runs when someone touches its own script. A script broken by a
change elsewhere — a renamed parameter, an altered shared helper — stays broken until the next
person edits it.

## Scope

**This change will:**

- Add a coverage check: every **Build-tier** script must carry a `-SelfTest` switch, with a
  deliberate, commented exemption list.
- Add `-SelfTest` to the three Build-tier scripts that lack one, plus `Get-BuildArtifact.ps1`
  (see D4).
- Name `BuildFreshness.ps1` and `Get-BuildArtifact.ps1` in `Get-ChangeTier.ps1` as Build tier, with
  self-test cases (#364 item 4, extended by D4).
- Run the whole self-test set nightly, not only the changed scripts'.

**This change will not:**

- Introduce a shared assertion module (#395 item 1). The dominant idiom here is a table of cases plus
  one comparison loop, which such a module does not replace; the measured duplication is ~25 lines
  across three files. Recorded on #395.
- Add `-SelfTest` to Tooling-tier scripts. `Sync-Master.ps1`, `New-Worktree.ps1` and
  `New-TaskBranch.ps1` are named in #395 but are Tooling, and the rule proposed there is keyed on
  Build tier. Widening the rule is a separate decision with a much larger bill.
- Extract the duplicated UCI driver (#364 item 1). It depends on `Run-Bench.ps1` gaining a self-test,
  which this change does not provide either — `Run-Bench.ps1` is Tooling tier.

## Decisions

### D1: The check lives in `Validate-PrePR.ps1` and runs unconditionally

The rule is a property of the **whole tree**, not of the changed file, so scoping it to the diff
would let a new Build-tier script land uncovered whenever the PR touching it also touched nothing
else in scope. It is cheap enough to run every time — it parses ~23 param blocks with the PowerShell
AST parser and touches no toolchain.

Rejected: a standalone `Test-SelfTestCoverage.ps1`. It would itself be a Build-tier script needing a
self-test, and the check is ~20 lines; a new file for that is not worth the extra surface.

Rejected: putting it in `Validate-PreCommit.ps1`. The pre-commit hook should stay fast, and a
coverage regression cannot reach `main` without passing the PR gate.

### D2: The exemption is an explicit allowlist, not a structural rule

`BuildFreshness.ps1` is a dot-sourced library with no `param()` block, so it cannot take a
`-SelfTest` switch; it is covered by `build.ps1 -SelfTest`, which its own header comment states.

The tempting structural rule — "no `param()` block means it is a library, so exempt it" — is
**wrong**, and measurably so: `Validate-PreCommit.ps1` and `Sync-Master.ps1` also have no `param()`
block and are ordinary top-level scripts. `Validate-PreCommit.ps1` is one of the three files this
change exists to cover, so that rule would exempt its main target.

So the exemption is a named list carrying its reason, mirroring the convention `Get-ChangeTier.ps1`
already uses for its Tooling allowlist: enumerated, fail-closed, so a new library lands as a failure
until someone classifies it deliberately. The check additionally asserts that the **covering script
named in the exemption exists and itself carries a `-SelfTest`**, so the exemption cannot become a
silent hole if `build.ps1` ever loses its own.

### D3: `Validate-PrePR.ps1` gets a `-SelfTest`, and its discovery is the reason

Its discovery loop reads `if ($names -notcontains 'SelfTest') { continue }`, so today a change to the
validator is the one change guaranteed to run no validator test. Giving it a `-SelfTest` closes that
by the existing mechanism rather than a special case — and the coverage check above is one of the
things that self-test asserts.

### D4: `Get-BuildArtifact.ps1` moves to Build tier as well

#364 item 4 names only `BuildFreshness.ps1`. `Get-BuildArtifact.ps1` reaches Engine through the same
fail-closed default and is the more load-bearing of the two: it decides **which binary a measurement
reads**, and it defaults to the shipping build specifically so a measurement cannot silently use an
MSVC one. A bug there produces a confident wrong number — the same self-concealment hazard, so the
same tier. It has a `param()` block, so unlike `BuildFreshness.ps1` it gets a real `-SelfTest`
rather than an exemption.

### D5: The nightly full run goes on the Windows job, not the Linux one

`Scripts/*.ps1` are Windows developer tooling: `build.ps1` drives clang-cl, the worktree scripts
manage a Windows checkout. Two self-tests (`Run-Lint`, `New-TidyCompileDatabase`) run on Linux today
because the *lint jobs* are Linux and those two scripts serve them — not because the set is expected
to be portable.

So the sweep goes in `lint-deep-windows`, replacing its "Validate lint scripts" step, and the Linux
`lint-tree` job keeps running the two lint-machinery self-tests it needs for its own work. Inventing
a Linux requirement for `build.ps1 -SelfTest` would be a new constraint this change has no reason to
impose.

Rejected: a new dedicated nightly job. The sweep takes seconds and `lint-deep-windows` already
checks out the tree and runs before its own heavy work, so it fails fast at no extra runner cost.

## Assumptions I cannot verify from the code

- **That all seven current self-tests would also pass on Linux.** Not verified, and D5 makes it
  unnecessary — but it is the assumption that would have to hold if the sweep were ever moved to the
  Linux job. WSL here has no `pwsh` and installing one needs an interactive sudo, so settling it
  needs either that install or a throwaway CI run. Recorded rather than assumed away.
- **That `Validate-PrePR.ps1` and `New-PullRequest.ps1` have testable pure logic to assert on.**
  Both are heavily I/O-bound (git, `gh`, the build). The self-tests added here pin the pure parts —
  argument handling, tier dispatch, the discovery predicate — not the network paths. If a script
  turns out to have no pure core worth pinning, the honest answer is to say so in its self-test
  rather than to write a test that asserts nothing.

## Invariants

- `Validate-PrePR.ps1 -SelfTest` exits non-zero if any Build-tier script lacks a `-SelfTest` and is
  not on the exemption list.
- Every exemption names a covering script that exists and itself carries a `-SelfTest`.
- The self-test set stays pure and toolchain-free — that property is why the pre-commit hook can run
  the discovered ones.
- `Get-ChangeTier.ps1`'s classification of every already-named path is unchanged; only
  `BuildFreshness.ps1` and `Get-BuildArtifact.ps1` move, and both move Engine → Build, which is a
  relaxation no gate depends on being strict.

## Validation

Build tier (the diff touches `Get-ChangeTier.ps1` and both validators). Evidence:

- **Falsification, per script.** Each new `-SelfTest` must fail against the unfixed behaviour it
  pins — for the coverage check, temporarily removing `-SelfTest` from a Build-tier script must make
  `Validate-PrePR.ps1 -SelfTest` exit non-zero, and removing `build.ps1`'s must trip the
  exemption's covering-script assertion.
- All existing self-tests still pass, run as a set.
- `Validate-PrePR.ps1` at Build tier passes end to end.

No Elo match: no engine source changes, so search behaviour is untouched by construction.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Why the exemption is a list and not "has no `param()` block" (D2) | source comment on the exemption list |
| Why `Get-BuildArtifact.ps1` is Build tier (D4) | source comment beside its rule in `Get-ChangeTier.ps1`, as the neighbouring rules already do |
| Why the sweep is Windows-only (D5) | comment on the nightly step |
| That the coverage rule exists at all, and where it runs | `.claude/skills/write-powershell/SKILL.md` → the `-SelfTest` convention |
| The Linux-portability assumption, if it is ever wanted | #395 as a comment |
