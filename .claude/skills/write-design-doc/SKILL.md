---
name: write-design-doc
description: Use when writing a design document or spec before implementing; CLAUDE.md → Design Documents decides whether one is needed. Reviewing one → cross-agent-review; executing one → exec-plan.
---

# Write a Design Document

1. Copy `.claude/plans/TEMPLATE.md` to `.claude/plans/<kebab-name>.md` and follow its guidance.
2. **Check scope first.** If the change spans independent subsystems, write one document and one PR
   per subsystem.
3. **Multi-PR designs:** state what each PR produces that a later one relies on — exact names,
   signatures, data formats. That contract is durable; the ordering is not.

## Self-review before requesting review

Run it yourself, fix inline, no subagent:

- **No placeholders.** "TBD", "handle edge cases", "verify no regression" or an undecided option in
  Decisions, Invariants or Validation means the document is not ready.
- **Coverage.** Every "will" item is carried by a Decision or Validation; every Invariant has
  Validation that closes it; every assumption states how it is verified.
- **Ambiguity.** A Decision or Invariant that reads two ways gets one reading, stated.
- **Consistency.** Names, types and paths match each other and the code as it stands on
  `origin/main`.
- **Simplify.** Every Decision is needed now, not for a later change. Name the nearby debt the
  change touches (duplicated, dead or stale code), and check the plan does not add to it. The same
  lens reviews the diff: `Docs/agents/simplify.md`.

Then land the document in one commit (`Docs/Workflow.md` → Design document lifecycle), route it to
review (skill `cross-agent-review`) and, once approved, execute it (skill `exec-plan`).
