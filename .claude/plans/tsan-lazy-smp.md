# TSan for the Lazy SMP shared TT (#184)

## Goal

Decide whether TSan's reports on this engine are signal or noise, and wire a CI job only if they are
signal. #184 framed the shared TT as "deliberately racy-by-tolerance" and expected a suppression
file; the survey below shows that premise does not describe the current implementation.

Out of scope: MSan (#189, independent — different tool, different blocker, and the two cannot be
combined), and any change to the TT's synchronisation. Nothing here modifies engine behaviour.

## The survey

Run 2026-08-08 on WSL Ubuntu 24.04, GCC 13.3.0 (`libtsan` ships with it — no install), from a
`git archive` of the branch extracted onto ext4 per the standing Linux-from-Windows method.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSTRAT_SANITIZE=thread -DSTRAT_SANITIZE_RECOVER=ON
```

`STRAT_SANITIZE_RECOVER=ON` is survey mode: report and continue, so one run enumerates every finding
instead of stopping at the first.

### What had to be driven, and why the obvious run proves nothing

The `[smp]` tests are `SetThreads()` **clamp** tests — none of them runs a parallel search. The fast
tier is otherwise single-threaded, so a TSan run over it exercises no helper threads at all. The
survey therefore drives the engine over UCI with `Threads` set, which is the path that actually
spawns helpers.

| Configuration | Warnings | Fatals | bestmoves |
|---|---|---|---|
| Threads=1, startpos d8 (control) | 0 | 0 | 1 |
| Threads=4, startpos d8 | 0 | 0 | 1 |
| Threads=4, Kiwipete d8 | 0 | 0 | 1 |
| Threads=4, three searches + `ucinewgame` between | 0 | 0 | 3 |
| Threads=4 → 8 → 2, reconfigured between searches | 0 | 0 | 3 |
| Threads=8, startpos d10 | 0 | 0 | 1 |
| Fast tier `~[slow]` | 0 | 0 | 281 cases pass |

Helpers demonstrably ran: the same depth-8 startpos search visits 144,260 nodes at `Threads=1` and
421,608 at `Threads=4`; the depth-10 run at `Threads=8` visits 2,249,085.

### Two traps that produce a false clean run

1. **TSan aborts on this kernel without disabling ASLR.** Ubuntu 24.04 raises `vm.mmap_rnd_bits`
   beyond what TSan's shadow mapping tolerates, and the process dies with
   `FATAL: ThreadSanitizer: unexpected memory mapping` before reaching `main`. It reports zero
   warnings while doing so, which is indistinguishable from a clean run if only the warning count is
   read. Every invocation goes through `setarch $(uname -m) -R`.
2. **Piping all UCI commands at once loses the searches.** The engine reads faster than it searches,
   so `position`/`setoption` arrive mid-search and are correctly refused by the UCI guards. The
   driver waits for `uciok`/`readyok`/`bestmove` before sending the next command.

### Positive control

A survey that reports nothing is worthless without evidence the tool would have spoken. A deliberate
unsynchronised `static int` increment injected into `helper_loop` (in the WSL copy only, reverted
after) produced **2 warnings** with correct stacks naming `helper_loop` and the `GetMove` lambda that
spawns it. TSan is instrumenting and detecting on exactly the path the survey exercises.

## Why there is nothing to suppress

`TranspositionTable` is not lock-free. `TranspositionTable.h:68-81`: a `std::shared_mutex` per bucket
(`bucket_locks`), a global `shared_mutex` for resize/clear, and `std::atomic` for `current_age`,
`entry_count` and `pv_count`. Probes take shared locks, stores take exclusive ones. There is no torn
read to tolerate, so the judgement #184 asks for resolves cleanly: **the reports are signal, and
there are none.** No suppression file is written, which is the outcome the issue wanted rather than
the one it expected.

Two consequences worth stating: a lock per bucket is a synchronisation cost the racy-by-tolerance
designs avoid, and that is a strength question (`Run-EloMatch.ps1`), not a correctness one — out of
scope here. And "no races today" is not "no races ever", which is the argument for a job rather than
a one-off survey.

## Design decisions

**A CI job is justified, on the same trigger as `sanitize-linux`.** Measured cost: the four-scenario
drive is 31.6 s locally, and the fast tier is 65.4 s under TSan against 8.2 s for a clean Debug build
(8.0x, inside the expected 5-15x). Same order as the existing sanitizer leg, so it runs per-PR rather
than nightly.

**The job must drive multi-threaded search itself.** Running only `~[slow]` would be a job that
cannot fail for the reason it exists — the point #184 makes about `Threads=1` proving nothing. So the
job runs the committed driver at `Threads=4` and `Threads=8`.

**And it runs *only* that drive — no Catch2 tier.** The tier is single-threaded, so under TSan it
adds no coverage this job does not already have, while costing a second instrumented target and 65 s
of execution. `sanitize-linux` already runs it under ASan/UBSan. See the CI-impact section: this is
what keeps the job off the critical path.

**`STRAT_SANITIZE_RECOVER` stays OFF in CI.** Survey mode exits 0 with findings on screen; the job
must fail on a finding, matching `sanitize-linux`.

**The driver is committed, not inlined in YAML.** `.github/scripts/tsan_smp_drive.py` is runnable
locally against any build, which is what makes a CI failure reproducible. Note `/.github/*` is
blanket-ignored behind an allowlist in `.gitignore`; `!/.github/scripts/*.py` already covers it.

## Expected CI impact

Baseline is PR #241's full-tier run (2026-08-08, uncontended). `classify` runs alone, then five jobs
fan out in parallel, then `build-and-test-result` closes:

| Job | Run time |
|---|---|
| `classify` | 8 s |
| `build-linux (Release)` | **248 s — the critical path** |
| `sanitize-linux` | 231 s |
| `build-and-test (Debug)` | 230 s |
| `build-linux (Debug)` | 192 s |
| `build-and-test (Release)` | 189 s |
| `build-and-test-result` | 2 s |
| **Total wall clock** | **308 s** |

### Uncontended

`tsan-linux` becomes a **sixth parallel job**, not a sixth serial step, so it costs PR feedback time
only if it runs longer than the 248 s critical path. Estimated 200-260 s: the instrumented engine
build should land near `sanitize-linux`'s ~200 s build (fewer translation units — no Catch2
amalgamation, no test sources), plus a drive measured at **31.6 s locally on 24 cores**. The
`Threads=4` scenarios should transfer to a 4-vCPU runner roughly unchanged (four threads, four
cores); only the `Threads=8` scenario is oversubscribed there and should cost about double.

So: **expected wall-clock impact between none and ~+30 s**, and runner minutes up by roughly one job
(~4 min against ~20 job-minutes today, i.e. ~+20%) — free on a public repository. Docs- and
Tooling-tier PRs are unaffected: the job carries the same `is_full == 'true'` condition as its
siblings and skips wholesale.

This is the estimate the design was built around, and it is why the job builds only
`StratChessEvolved` and skips the Catch2 tier. Adding the tier would have cost a second instrumented
target (~+150 s) plus 65 s of measured test execution, pushing the job past the critical path and
making every full-tier PR wait longer — for coverage that is single-threaded and already provided by
`sanitize-linux`. The first real run replaces this estimate.

### During an 18-shard strength run

`strength.yml` takes 18 of the account's 20 concurrent-job slots, deliberately leaving 2 so ordinary
CI still moves (#217). The fan-out then has to drain through those 2 slots:

| | Fan-out jobs | Waves through 2 slots |
|---|---|---|
| Today | 5 | 3 (2 + 2 + **1**, one slot idle) |
| With `tsan-linux` | 6 | 3 (2 + 2 + 2) |

**The sixth job fills the idle slot in a wave that already exists, so the expected added delay is
zero.** The bound, if the scheduler packs unevenly or a Windows leg straggles, is one extra job
duration (~4 min) on a run already measured at ~10 min under contention against a 4.5-5 min
uncontended baseline (#217). Windows jobs count against the same 20-slot allowance, so nothing here
is OS-specific.

If the shard count is ever raised to 20, both the old and new fan-outs block equally — that is a
property of consuming the whole allowance, not of this job, and it is why `strength.yml` documents 18
as the deliberate choice.

## Files changed

- `.github/scripts/tsan_smp_drive.py` — UCI driver: sends commands, waits for the completion token,
  fails on timeout. Takes the engine path, thread counts and depths as arguments.
- `.github/workflows/build-and-test.yml` — new `tsan-linux` job; added to `build-and-test-result`'s
  `needs` and its case blocks, so a finding blocks the merge and a skipped tier still reports success.
- `Docs/CI.md` — what the job runs and why it drives UCI rather than the test binary.
- `Docs/Workflow.md` — standing decision: what TSan covers, and that the absence of suppressions is
  the finding.
- `Docs/Changelog.md`, this plan.

## Validation

- `Tooling`/`Build` tier locally; the job itself is proven by the survey above rather than by a local
  CI run.
- The workflow's first run on the PR is the real check. A green `tsan-linux` on a branch that changes
  no engine code confirms the wiring; the positive control above confirms the instrumentation.

## Invariants afterwards

- TSan and ASan are never combined in one job — they are mutually exclusive at link time.
- Any future change making the TT lock-free reopens this: the survey's result is a property of the
  current locked design, not a permanent one.
- If a suppression file is ever added, each entry names why the race is tolerated. A permanently
  suppressed sanitizer looks like coverage and is worse than none.
