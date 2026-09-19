# Pin hot-code alignment in the shipping build — Design

**Issue:** #578 (spike), parent #555

## Goal

Nothing in the build pins code layout, so an edit that only resizes *cold* code moves every hot
function and changes its cache-line, page and branch-predictor offsets. #556 measured a node-
identical pair reading **−3.90% median nps** for that reason alone, and the project's answer today is
a manual escalation: notice the delta, relink both builds with a shared `/ORDER`, re-measure. That
costs agent and owner time on every refactor whose bench delta looks wrong, and by symmetry it can
mask a real per-node regression behind a lucky layout draw. The spike (#578) measured whether one
build-wide flag removes enough of that variance to be worth paying for permanently.

## Scope

**This change will:**

- Add `/clang:-falign-functions=64` to the clang-cl branch of `strat_configure_target`
  (`CMakeLists.txt:286-291`). That function configures both `StratChessEvolved` and
  `StratChessTests`, so both binaries get the alignment and its padding (D2).
- Amend `Docs/Workflow.md:361`, whose paragraph currently opens "Code placement moves nps, and
  nothing in the build pins it" — false once the flag ships. It becomes a statement that the build
  pins 64-byte function alignment and nothing further.
- Append to that same paragraph what the flag does and does not remove, as one number and one
  sentence, replacing nothing in the existing `/ORDER` escalation guidance. Its closing "Whether to
  pin the layout permanently is open (#555)" stays: the flag pins alignment, not placement, and
  #555's two other remedies remain open.
- Add a one-line `Docs/Changelog.md` entry.

`.claude/skills/measure-strength/reference/regression-check.md:35` needs no edit: its claim is the
`/ORDER` escalation, which survives this change.

**This change will not:**

- Add `-falign-loops`. It reaches the frontend but leaves no trace in the IR, so in the shipping
  Release engine target — where codegen runs inside `lld-link` under LTO — it cannot take effect
  (D2).
- Add a checked-in `/ORDER` file, or move cold setup code out of `AIPerplex.cpp`. Both were parked in
  #555 pending this result and neither is cheaper than the flag.
- Retire the manual `/ORDER` escalation documented by #577. The flag reduces the residual swing; it
  does not eliminate it (D3), so the escalation stays the answer when a delta matters.
- Touch the MSVC or GCC branches. MSVC is not measured and Linux is a correctness gate, not a speed
  one.
- Claim or measure an Elo effect.

## Decisions

### D1: `-falign-functions=64` as a compile flag, over the two alternatives #555 listed

Chosen because it is the only one of the three that costs nothing to maintain: no file to keep in
step with the code, no refactor, one line in `CMakeLists.txt`. A checked-in `/ORDER` file goes stale
on every edit and needs a check to notice; moving cold setup out of `AIPerplex.cpp` is a refactor
with its own rationale and would still leave the next cold-code edit free to move the hot path.

The flag was verified in the shipped image, not from a driver exit code — the `-fconstexpr-steps=`
trap at `CMakeLists.txt:269-270` is why. From the `/MAP` of both spike builds, over 3,184 code
symbols:

| | at ≥64-byte alignment | `pvs` | `quiescence` |
|---|---|---|---|
| baseline | 730 (22.9%) | `%64 = 16` | `%64 = 48` |
| `-falign-functions=64` | 2,951 (92.7%) | `%64 = 0` | `%64 = 0` |

Measured cost (#578, stock `main` `b1b217f`, 10 pairs at depth 14 with the first discarded as
warm-up, `Threads=1`, node-identical):

| | median | range |
|---|---|---|
| aligned vs baseline nps, 9 kept pairs | **+0.94%** | −0.92 .. +1.83% |

i.e. no measurable cost. The **sign is not claimable in either direction**: the two builds are
differently laid out, so that +0.94% is not separable from layout noise and must not be quoted as a
speedup. Layout swing under three cold-code perturbations (5 / 20 / 45 ops inserted into
`AIPerplex::StartNewGame`), each built both ways:

| perturbation | baseline swing | aligned swing |
|---|---|---|
| 5 ops | +1.03% | +0.21% |
| 20 ops | +0.48% | +0.05% |
| 45 ops | +0.62% | +0.17% |

The aligned column is an **upper bound, not a measurement**: per-pair sd across these series was
0.75-1.11%, so those figures are not distinguishable from zero — nor from 0.5%.

The price is a **+1.6% image** (3,525,632 → 3,581,952 bytes) from the padding.

### D2: functions only — `-falign-loops` is excluded because it leaves no IR trace

Both spellings survive the clang-cl driver: `clang-cl -###` shows `-function-alignment 64` and
`-falign-loops=32` reaching the frontend. Only function alignment lands in the IR, as `align 64` on
each definition, which link-time codegen honours. Loop alignment leaves no IR trace at all.

That makes it inert **where it would matter**. `INTERPROCEDURAL_OPTIMIZATION_RELEASE` is set on
`StratChessEvolved` only, Release only, conditional on `check_ipo_supported`
(`CMakeLists.txt:395-403`), so in the shipping build codegen happens inside `lld-link` after the
driver flags are gone. In Debug, and in `StratChessTests` (which `strat_configure_target` also
configures, `:408`, and which never links with LTO), loop alignment *would* take effect — on
binaries nothing measures. Shipping it would therefore be a flag that reads as set, does nothing
where speed is measured, and does something unmeasured everywhere else — exactly the clang-cl trap
`CMakeLists.txt` already documents.

Rejected alternative: pass loop alignment through to the LTO backend (`-mllvm` on the link line).
That is a second, differently-spelled mechanism for an effect nothing has measured, and it would
have moved two variables in one experiment.

### D3: keep the manual `/ORDER` escalation, and say what the flag does not fix

Function alignment pins offsets **modulo 64** only. A code-size change still moves hot functions
across cache sets and pages, which is the other half of #556's mechanism. The spike also never
reproduced a #556-scale swing: the largest it produced on an unaligned build was +1.03%, so the
evidence supports "bounds the residual swing at ~0.2% where it was ~0.5-1%", not "tames a 4% one".
Presenting the flag as the fix would re-create the false-positive #577 just corrected.

## Assumptions I cannot verify from the code

- **The +1.6% image costs nothing at runtime.** Not verified directly. The spike's nps series is the
  only evidence, and it bounds any I-cache or page-fault cost at the instrument's noise — it does not
  isolate it. It would be settled, if it ever mattered, by the same paired series on a machine with a
  smaller L2.
- **The flag behaves the same on the CI Windows runner as on the dev machine.** Not verified.
  Alignment is a codegen property, so the emitted image is machine-independent; only the nps figures
  are machine-relative, and CI reads no nps. A `/MAP` from a CI build would settle it and is not
  worth a run.
- **The spike's throwaway builds applied the flag the same way the PR does.** The spike injected it
  via `CMAKE_CXX_FLAGS`; the PR puts it in `target_compile_options`. Both reach the same frontend
  invocation, but that is an inference, and it is what the `/MAP` step in Validation exists to close
  on the real build.

The first two are about magnitude, not correctness: no assumption here affects what the engine
computes.

## Invariants

- Search behaviour is unchanged: identical node counts and best moves at `Threads=1` against the
  merge base. The flag changes placement, not instructions.
- The build stays `/W4 /WX`-clean on clang-cl, and the MSVC and GCC command lines are byte-identical
  to what they are today — closed by inspection of the diff, which touches only the `elseif` clang-cl
  branch at `CMakeLists.txt:286`.
- `StratChessTests` also gains 64-byte function alignment and a comparable size increase. Harmless —
  nothing measures that binary's speed — but it is a consequence of the chosen insertion point.
- Whatever reproducibility basis the Release build has, this change neither strengthens nor weakens
  it. Alignment padding is a deterministic function of the IR, so it introduces no new nondeterminism
  on either side of `/Brepro`'s two halves (`CMakeLists.txt:316-320`). This is deliberately *not*
  phrased as "`/Brepro` still holds": per #513, `/Brepro`'s compile half is inert on the engine
  target's ThinLTO objects and no Release reproducibility gate exists to hold — #381 established one
  for Debug only. Closed by inspection; nothing here is in a position to validate it.

## Validation

**Build tier** (`Get-ChangeTier.ps1:117` classifies any `CMakeLists.txt` diff as Build;
`Docs/Workflow.md:51`): the clang-format check, a full build, extended `[slow]` tests, the tactical
suite and self-play. `Validate-PrePR.ps1` scopes itself there — this is not the Tooling fast path.

On top of the tier:

- **A `/MAP` of the PR's own clang-cl Release build**, confirming `pvs` and `quiescence` at
  `%64 = 0` and the ≥64-aligned share of code symbols near 92%. This is the one check in the set that
  can fail, and it is the only thing that proves the flag survived the move from the spike's
  `CMAKE_CXX_FLAGS` injection to `target_compile_options`. One extra link step.
- `Compare-SearchEquivalence.ps1 -After .\build\windows-clang-cl\StratChessEvolved.exe -BaselineRef
  origin/main` → IDENTICAL. A **build-accident check**, not behaviour evidence: for a placement-only
  flag the result is IDENTICAL by construction.
- An MSVC Release build, confirming the MSVC branch still configures. The diff, not this run, is what
  establishes that branch is untouched.
- The paired bench series from #578 is the speed evidence and is not re-run: it was taken on this
  exact flag against this exact commit, and its numbers are in the issue.

**No Elo match.** The flag is node-identical by construction, and the spike's nps delta is inside
layout noise in both directions — there is nothing an Elo instrument could resolve that ±4 Elo would
not swamp.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Why the flag is `-falign-functions` alone, and that `-falign-loops` leaves no IR trace | source comment beside the flag in `CMakeLists.txt` |
| What the flag removes (mod-64 offsets) and what it leaves (cache set, page) | `Docs/Workflow.md:361-368`, amending the lead clause and appending one sentence beside the `/ORDER` escalation |
| The measured cost and swing numbers, and that the aligned swings are bounds | issue #578 comment (already recorded), and the PR body |
| The flag now pins hot-function alignment in the shipping build | `Docs/Changelog.md` |
