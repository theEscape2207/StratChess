---
name: exec-plan
description: Use when asked to carry out or resume an implementation plan that has already been approved; do not use to design or approve one.
---

# Execute an Approved Plan

Treat the approved plan as the contract. When `superpowers:executing-plans` or
`superpowers:subagent-driven-development` also matches, use this skill instead.

## Establish the contract

1. Work on a branch forked from `origin/main`, never on `master` or `main`: `Scripts/New-Worktree.ps1`
   or `Scripts/New-TaskBranch.ps1`. Resuming: continue on the plan's existing branch.
2. Read the complete plan, `.claude/plans/TEMPLATE.md` and `Docs/Workflow.md` → Design document
   lifecycle before changing files.
3. Confirm the plan is approved. Do not execute a draft or a plan with an unresolved decision. An
   unverified assumption is allowed when the plan states how it affects the work and how it will be
   verified.
4. Capture a compact working baseline: Scope ("will" and "will not"), numbered Decisions and rejected
   alternatives, unverified assumptions and their verification, Invariants, Validation, Harvest.
5. Note and preserve pre-existing workspace changes.
6. Move the plan to `.claude/plans/in-progress/` when implementation begins or resumes; a
   `not-started` plan may stay where `Docs/Workflow.md` permits. Repair references affected by a move.

## Track progress

Keep a ledger beside the plan: `<plan>.md` → `<plan>.progress.md` (gitignored, never committed). Its
first line names the plan file. Append one entry per passed checkpoint: checkpoint, commit SHA,
evidence, deviations.

**Resuming** — after compaction or in a new session, rebuild state from the ledger and `git log`, not
from memory. A checkpoint with a ledger entry and its commit is done; resume at the first without.

## Execute through checkpoints

Use checkpoints named by the contract. If it has none, keep a scratch sequence at meaningful risk or
deliverable boundaries; execution detail stays out of the durable plan.

At each checkpoint:

1. Identify the contract clauses and evidence it advances.
2. Implement only that slice.
3. Run the Validation scheduled by the contract plus proportionate checks for the slice. Run
   whole-change gates when the contract or repository workflow requires them.
4. Reconcile the result with Scope, Decisions, Invariants, and relevant assumptions.
5. Commit, then append the ledger entry.

Continue autonomously across passed checkpoints unless the contract requires review or new authority.

## Delegate bounded work

Delegate only a bounded subtask whose benefit (parallelism, specialization, cheaper model, less
context) exceeds the coordination cost; follow CLAUDE.md → Subagent Dispatch. The controller keeps
contract interpretation, deviation decisions, checkpoint acceptance, integration and final
Validation, and treats a subagent's completion as evidence to review, not acceptance. Give
self-contained work a fresh prompt (Codex: set `fork_turns: "none"`).

## Handle deviations

Record every departure from the contract in the ledger with what changed, why, and its evidence.

- Minor: remains inside Scope, preserves every Decision and Invariant, and does not weaken Validation.
  Record it and continue.
- Material: crosses a "will not" boundary, changes an approved Decision, weakens or replaces required
  Validation, violates an Invariant, invalidates an assumption the approved approach depends on, or
  discovers excluded work required for correctness or safety. Stop and ask for approval, stating the
  impact and smallest contract change needed.

After approval, update changed Decisions or acceptance criteria in the contract and reconcile the
deviation in Harvest.

## Close the plan

1. Complete remaining Validation, then report actual evidence, including failures and checks that
   could not run.
2. Compare the final diff and behavior with every Scope item, Decision, Invariant, and verified
   assumption.
3. Complete Harvest before PR work.
4. Delete the plan with its `.review.md` and `.progress.md`, unless told otherwise or something still
   cites it (`Docs/Workflow.md` → Plan states).
5. Report checkpoint outcomes, Validation, deviations, remaining risks, and lifecycle disposition.

Execution approval is not permission to publish, push, open a PR, or take other external actions not
already authorized.
