---
name: exec-plan
description: Use when asked to carry out or resume an implementation plan that has already been approved; do not use to design or approve one.
---

# Execute an Approved Plan

Treat the approved plan as the contract. This workflow is independent of `/goal`: do not create, read, update, pause, or complete a goal to track it.

When `superpowers:executing-plans` also matches, use this skill instead.

## Establish the contract

1. Read the complete plan and applicable repository instructions before changing files. In StratChessEvolved, also read `.claude/plans/TEMPLATE.md` and the design-document lifecycle in `Docs/Workflow.md`.
2. Confirm the plan is approved. Do not execute a draft or a plan with an unresolved decision. An unverified assumption is allowed when the plan states how it affects the work and how it will be verified.
3. Capture a compact working baseline:
   - Scope, including both "will" and "will not"
   - numbered Decisions and rejected alternatives
   - unverified assumptions and their verification
   - Invariants
   - required Validation
   - Harvest destinations
4. Note and preserve pre-existing workspace changes.
5. Apply the repository's lifecycle when work begins. In StratChessEvolved, place a plan in `in-progress/` when implementation begins or resumes; a `not-started` plan may remain where `Docs/Workflow.md` permits. Repair references affected by a move.

## Execute through checkpoints

Use checkpoints named by the contract. If it has none, keep a scratch sequence at meaningful risk or deliverable boundaries; execution detail stays out of the durable plan unless it has lasting value.

At each checkpoint:

1. Identify the contract clauses and evidence it advances.
2. Implement only that slice.
3. Run the Validation scheduled by the contract plus proportionate checks for the slice. Run whole-change gates when the contract or repository workflow requires them; do not invent a conflicting schedule.
4. Reconcile the result with Scope, Decisions, Invariants, and relevant assumptions.
5. Record evidence, remaining risks, and deviations before continuing.

Continue autonomously across passed checkpoints unless the contract requires review or new authority.

## Delegate bounded work

Delegate when a subtask is bounded and the expected benefit from parallelism, specialization, a lower model tier, or reduced context exceeds coordination and integration cost. The controller still owns contract interpretation, Scope and deviation decisions, checkpoint acceptance, integration, final Validation, and delivery.

- Give each subagent explicit file or responsibility ownership, required inputs, applicable contract constraints, expected output, and validation.
- Use the lowest-cost model tier that can reliably handle the assignment; reserve stronger tiers for ambiguous, cross-cutting, or high-risk reasoning.
- Set context inheritance explicitly. Give self-contained work a fresh prompt containing everything needed (Codex: set `fork_turns: "none"`). Inherit only the minimum recent history that is essential.
- Respect repository-specific delegation rules. Tell subagents they share the workspace, must preserve others' changes, and must not broaden scope. Treat their completion as evidence to review, not acceptance.

## Handle deviations

Record every departure from the contract promptly with what changed, why, and its evidence.

- Minor: remains inside Scope, preserves every Decision and Invariant, and does not weaken Validation. Record it and continue.
- Material: crosses a "will not" boundary, changes an approved Decision, weakens or replaces required Validation, violates an Invariant, invalidates an assumption the approved approach depends on, or discovers excluded work required for correctness or safety. Stop and ask for approval, stating the impact and smallest contract change needed.

After approval, update changed Decisions or acceptance criteria in the contract and reconcile the deviation in Harvest.

## Close the plan

1. Complete remaining contract and repository Validation, then report actual evidence, including failures and checks that could not run.
2. Compare the final diff and behavior with every Scope item, Decision, Invariant, and verified assumption.
3. Complete Harvest before PR work. Give every durable decision, rationale, non-obvious contract, and measurement a discoverable destination, including approved deviations and why they changed.
4. Apply the repository lifecycle. In StratChessEvolved, delete only when there are no inbound references, no deliberate spec/ADR role, and every durable item is harvested; otherwise use the correct named state and repair references when moved.
5. Report checkpoint outcomes, Validation, deviations, remaining risks, and lifecycle disposition.

Execution approval is not permission to publish, push, open a PR, or take other external actions not already authorized.
