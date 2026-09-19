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

- Add `/clang:-falign-functions=64` to the clang-cl branch of `strat_configure_target` in
  `CMakeLists.txt`.
- Record in `Docs/Workflow.md` → Speed and nps what the flag does and does not remove, as one
  number and one sentence appended to the code-placement paragraph (`Workflow.md:361-368`),
  replacing nothing in the existing `/ORDER` escalation guidance. Its closing "Whether to pin the
  layout permanently is open (#555)" stays: the flag pins alignment, not placement, and #555's two
  other remedies remain open.

**This change will not:**

- Add `-falign-loops`. It reaches the frontend but leaves no trace in the IR, and Release links with
  LTO, so the backend runs inside `lld-link` and never sees it (D2).
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

Measured (#578, stock `main` `b1b217f`, 10-pair series at depth 14, `Threads=1`, node-identical):

| | median | range |
|---|---|---|
| aligned vs baseline nps | **+0.94%** | −0.92 .. +1.83% |

i.e. no measurable cost. Layout swing under three cold-code perturbations (5 / 20 / 45 ops inserted
into `AIPerplex::StartNewGame`), each built both ways:

| perturbation | baseline swing | aligned swing |
|---|---|---|
| 5 ops | +1.03% | +0.21% |
| 20 ops | +0.48% | +0.05% |
| 45 ops | +0.62% | +0.17% |

The price is a **+1.6% image** (3,525,632 → 3,581,952 bytes) from the padding.

### D2: functions only — `-falign-loops` is excluded because LTO drops it

Both spellings survive the clang-cl driver: `clang-cl -###` shows `-function-alignment 64` and
`-falign-loops=32` reaching the frontend. Only function alignment lands in the IR, as `align 64` on
each definition, which link-time codegen honours. Loop alignment leaves no IR trace, and this build
sets `INTERPROCEDURAL_OPTIMIZATION_RELEASE`, so codegen happens inside `lld-link` after the driver
flags are gone. Shipping it would be a flag that looks set and does nothing — exactly the clang-cl
trap `CMakeLists.txt` already documents for `-fconstexpr-steps=`.

Rejected alternative: pass loop alignment through to the LTO backend (`-mllvm` on the link line).
That is a second, differently-spelled mechanism for an effect nothing has measured, and it would
have moved two variables in one experiment.

### D3: keep the manual `/ORDER` escalation, and say what the flag does not fix

Function alignment pins offsets **modulo 64** only. A code-size change still moves hot functions
across cache sets and pages, which is the other half of #555's mechanism. The spike also never
reproduced a #555-scale swing: the largest it produced on an unaligned build was +1.03%, so the
evidence supports "shrinks a sub-1% swing to ~0.1%", not "tames a 4% one". Presenting the flag as the
fix would re-create the false-positive #577 just corrected.

## Assumptions I cannot verify from the code

- **The +1.6% image costs nothing at runtime.** Not verified directly. The spike's nps series is the
  only evidence, and it bounds any I-cache or page-fault cost at the instrument's noise — it does not
  isolate it. It would be settled, if it ever mattered, by the same paired series on a machine with a
  smaller L2.
- **The flag behaves the same on the CI Windows runner as on the dev machine.** Not verified.
  Alignment is a codegen property, so the emitted image is machine-independent; only the nps figures
  are machine-relative, and CI reads no nps. A `/MAP` from a CI build would settle it and is not
  worth a run.

Both are about magnitude, not correctness: no assumption here affects what the engine computes.

## Invariants

- Search behaviour is unchanged: identical node counts and best moves at `Threads=1` against the
  merge base. The flag changes placement, not instructions.
- The build stays `/W4 /WX`-clean on clang-cl, and the MSVC and GCC command lines are byte-identical
  to what they are today.
- `/Brepro` still holds: two clean builds of one commit produce the same bytes.

## Validation

Tooling tier, plus one speed pass — the change is a build flag and touches no engine source.

- `Compare-SearchEquivalence.ps1 -BaselineRef origin/main` → IDENTICAL. Closes the behaviour
  invariant.
- `Validate-PrePR.ps1`, which scopes itself to the tier, plus a clang-cl **and** an MSVC Release
  build to confirm the MSVC branch is untouched.
- The paired bench series from #578 is the speed evidence and is not re-run: it was taken on this
  exact flag against this exact commit, and its numbers are in the issue.

**No Elo match.** The flag is node-identical by construction, and the spike's nps delta is inside
layout noise in both directions — there is nothing an Elo instrument could resolve that ±4 Elo would
not swamp.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Why the flag is `-falign-functions` alone, and that `-falign-loops` is inert under LTO | source comment beside the flag in `CMakeLists.txt` |
| What the flag removes (mod-64 offsets) and what it leaves (cache set, page) | `Docs/Workflow.md` → Speed and nps, one sentence, next to the existing `/ORDER` escalation |
| The measured cost and swing numbers | issue #578 comment (already recorded), and the PR body |
| The flag now pins hot-function alignment in the shipping build | `Docs/Changelog.md` |
