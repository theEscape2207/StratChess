---
name: write-design-doc
description: Use when a change needs a design document (CLAUDE.md → Design Documents) before implementation, or when asked to write, plan or spec a change; do not use to review or execute one.
---

# Write a Design Document

1. Copy `.claude/plans/TEMPLATE.md` to `.claude/plans/<kebab-name>.md`, named after its content. The
   template carries the audience, proportionality and section guidance; follow it.
2. **Check scope first.** If the change spans independent subsystems, write one document and one PR
   per subsystem.
3. **Multi-PR designs:** state what each PR produces that a later one relies on — exact names,
   signatures, data formats. That contract is durable; the ordering is not.

**The weight only ratchets up.** When implementation uncovers a decision that could go more than one
way, or an assumption the code cannot verify, stop and write the document; never drop one mid-task
because the change "turned out simple". A spike's output is an answer, not code: keeping its code is
a new change that needs its own document or PR.

## Self-review before requesting review

Run it yourself, fix inline, no subagent:

- **No placeholders.** "TBD", "handle edge cases", "verify no regression" or an undecided option in
  Decisions, Invariants or Validation means the document is not ready.
- **Coverage.** Every "will" item is carried by a Decision or Validation; every Invariant has
  Validation that closes it; every assumption states how it is verified.
- **Ambiguity.** A Decision or Invariant that reads two ways gets one reading, stated.
- **Consistency.** Names, types and paths match each other and the code as it stands on
  `origin/main`.
- **Proportion.** Cut any section that is filler; a one-sentence section is complete.

Then land the document in one commit (`Docs/Workflow.md` → Design document lifecycle), route it to
review (skill `cross-agent-review`) and, once approved, execute it (skill `exec-plan`).
