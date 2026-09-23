# The -SelfTest convention

Scripts carry a `-SelfTest` switch. `Validate-PrePR.ps1` discovers them by parameter introspection
and runs the self-test of any script your change touches, so a new one is picked up with no
registration step. Keep them **pure and toolchain-free** — that property is why the pre-commit hook
can run them. `Test-UciDriver.ps1` is the one exception, and it spawns `pwsh`, never a compiler.

Two rules are enforced rather than suggested:

- **Every Build-tier script must have one.** Those scripts gate validation itself, so a bug in one
  can exempt a change from the checks and then decline to report it. `Validate-PrePR.ps1` fails on a
  Build-tier script without a `-SelfTest`, on every tier including the Docs and Tooling fast paths.
  A dot-sourced library that has no `param()` block to hang a switch on needs an entry in
  `$SelfTestCoverers` naming the script that covers it. That entry is read twice: it exempts the
  library from this rule, and it is what makes a change to the library run the coverer's
  `-SelfTest` rather than nothing. The coverer named by an entry is itself checked to exist and
  carry a `-SelfTest`, whatever tier the covered file is — otherwise the entry quietly covers
  nothing the moment the coverer loses the switch. Do **not** give the library
  a `$SelfTest` parameter instead — dot-sourcing runs in the caller's scope, so it would overwrite
  the caller's own switch and turn that script's `-SelfTest` into a silent no-op.
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
