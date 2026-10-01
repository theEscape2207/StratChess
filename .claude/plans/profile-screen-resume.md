# Profile screen recovery — Design

**Issue:** #666, slice 1 (checkpoint/resume). Baseline: `origin/main` `859b435`.

## Goal

Retain completed profile searches through a failed exchange or interruption, then resume the exact experiment without replaying successful searches or changing the reported statistics.

## Review focus

- D1/D2: identity must distinguish both sides, seed schedules and repeated position occurrences.
- D3: only a validated, complete exchange becomes reusable; failure/cancellation must release ownership without deleting results.
- D4: cleanup must remove only disposable files, with a usable recovery command and no automatic deletion of completed experiments.

## Scope

This change adds persistent recovery and cleanup to `Compare-SearchProfile.ps1`, with toolchain-free orchestration tests. It does not change the engine, shared driver, screen estimator, timeout policy, or other measurement tools. The Windows idle-sleep guard is slice 2, after this PR merges.

## Decisions

### D1: One immutable experiment per directory

`-RunDirectory` names a new or existing experiment; omission creates a unique `build/profile-runs/<timestamp>-<guid>` directory in this worktree. Print its absolute path and a fully quoted resume command before engine work. A JSON manifest contains format/request-contract version 1, full before/after binary SHA-256 hashes, depth, Threads=1, seed count and actual per-side seeds, and the ordered resolved Name/Fen list. Paths are provenance, excluded from comparison, permitting relocation of identical bytes. Compare the canonical contract JSON, not truncated display hashes. A mismatched or malformed manifest fails before any search. An existing nonempty directory without a manifest is refused, except script-owned lock/temporary files from interrupted initialization. Reusing records across different experiments was rejected because it complicates validation and accounting.

### D2: Pin binaries for an invocation

Open each input binary read-only with FileShare.Read and hash that handle; retain it until cleanup so Windows refuses overwrite/delete while engines run. On non-Windows systems, additionally hash the current pathname before and after each fresh search to detect replacement, since non-cooperating writers do not necessarily honor sharing. No per-search full binary hashing on Windows. Binary paths are resolved once; no binary snapshot or copied dependencies are introduced.

### D3: Validated checkpoints, original request order

The production loop becomes `Invoke-ProfileRun`, accepting a search scriptblock as its only test seam. It owns resources, runs the existing side/position/seed order, and returns the same record lists. Checkpoint filename uses position occurrence index, side and actual seed; the JSON envelope contains the manifest-contract digest, request identity, raw transcript and full transcript SHA-256. Validate driver completion, profile schema and seed echo before writing. Revalidate completion/parser/echo/digest/identity on reuse. Write a sibling `.tmp`, close, then rename without overwriting a committed result. Ignore/remove uncommitted `.tmp` files; corrupt committed files fail with a path and instruction to remove only that result for recomputation. Cache miss launches one engine. A process-held `.run.lock` FileStream with FileShare.None prevents concurrent use; its empty file may remain but never determines ownership. No lock-file deletion after release, avoiding a release/delete race.

Normal reports are generated only after all expected results exist, in original order. Do not drop failed positions or vary seed counts. A completed rerun launches zero engines and has identical numerical Screen and scope tables. Existing zero-metric reporting remains unchanged.

### D4: Retain results, clean disposable state

Use a unique invocation working directory inside the run for engine logs/artifacts. In `finally`, restore the caller seed environment, dispose binary handles, remove that invocation directory and temporary checkpoint writes while still holding the run lock, then dispose the lock. On the next invocation, under the lock, remove stale script-owned work directories/temporary writes left by a killed process. Do not remove manifests or committed checkpoints after success/failure. Print that the run is retained and may be manually deleted once its measurements are recorded and recovery is no longer needed. No purge flag, automatic age-based deletion or unrelated-directory cleanup. Disk-full/write failures remain failures with the same resume guidance; cleanup failures warn without hiding the primary error.

## Assumptions I cannot verify from the code

- File sharing excludes another process and blocks binary replacement on Windows: verify with a spawned pwsh lock attempt and an attempted binary write while pinned.
- Same-directory close/rename commits a complete file across process interruption: test an abandoned `.tmp` and committed/truncated result. This is not physical-disk-flush protection against power loss.
- Numerical output equality is testable independently of engine timing: verify uninterrupted versus interrupted/resumed synthetic records through the existing estimator and table formatter, and a small real profile build smoke run.

## Invariants

Every committed result came from a completed, validated exchange under its recorded identity. The before/after sides and duplicate occurrences remain distinct. An incompatible run launches no engines. Failure preserves previously committed results and restores invocation resources. Completed records preserve the existing report semantics.

## Validation

Tooling tier: syntax parse, existing profile self-test plus recovery cases, applicable pre-commit/pre-PR gates. Assert exact launched request identities on interruption/resume, complete-cache rerun, Seeds=0/seeded/one-position/duplicates/same-binary sides; refuse changed manifest inputs, corrupted envelopes, ignored seed and incomplete search. Verify concurrent process ownership and resource cleanup after failure. Build a current-source profile executable in this worktree for a short real interrupted/resumed report comparison and cache size/time evidence. No Elo match or nps benchmark: no engine code or per-node work changes.

## Cost

Over 200 changed lines in one production script and its self-test, plus brief durable help/changelog. Tooling gates and normal code/design review; no new dependency or gate. The separately priced sleep guard and other tools are outside this slice.

## Harvest

| Durable decision | Destination |
|---|---|
| Immutable identity, validated commits, fail-fast resume | script helper names/comments and self-tests |
| Retained runs and safe manual deletion; transient cleanup | command help, runtime messages, tests |
| Changed command and recovery behavior | Docs/Changelog.md |
| Sleep guard remains a subsequent slice | PR Notes and issue #666 |

Self-review completed: identity includes ordered occurrences; lock file is retained to avoid a release/delete race; cleanup is scoped to script-owned temporary state. These refinements do not change the issue's recovery contract.
