# <Title> — Design

**Issue:** #<n>

> Delete this block when you copy the template.
>
> **Audience: a future maintainer arriving cold**, not the agent doing the work. Everything here has
> to earn its place against that reader. Execution detail — ordering, file-by-file edits, checklists
> — belongs in a scratchpad working note, not in this file, unless the task is complex, paused, or
> handed to someone else.
>
> **Proportionality.** A document longer than the diff it describes is a signal: either the change is
> riskier than it looks (keep it) or the document is padding (cut it). **A one-sentence section is a
> complete section** — these are prompts to think, not blanks to fill. Delete any that genuinely does
> not apply rather than writing filler under it.
>
> Write this before implementing when **either**:
> - the change has a decision that could reasonably go more than one way, or
> - it rests on an assumption you cannot verify from the code in front of you.
>
> File count is not the trigger. A ten-file mechanical rename needs nothing; a one-line change to
> `replacementScore()` needs this.

## Goal

One paragraph: what changes and why it is worth doing. State the problem, not the solution.

## Scope

**This change will:**

- …

**This change will not:**

- …

The will-not list is load-bearing — it is what stops scope creep during implementation and tells a
reviewer which objections are out of bounds. It is not a shield: an exclusion stays challengeable
where the excluded work turns out to be required for correctness or safety.

## Decisions

Number them so a review can cite them individually.

### D1: <decision>

What was chosen, what was rejected, and why. Include the rejected option — a decision recorded
without its alternative reads as an assumption.

### D2: …

## Assumptions I cannot verify from the code

The section a reviewer should read first. Anything depending on behaviour outside this repository —
another tool, a GUI, a client, the OS, the toolchain — plus anything taken on trust from a document
rather than checked.

For each: **how it would be verified**, and whether that was done.

> Example: "fastchess sends `ucinewgame` between games. Not verified. Would be settled by a temporary
> marker plus a 20-game `-Smoke` run."

If this section is empty, say so explicitly rather than omitting it — an empty section is a claim,
a missing one is an oversight.

## Invariants

What must still hold after the change. These become the acceptance criteria and, where they concern
non-obvious contracts, the source comments.

## Validation

Which tier applies and why; what evidence closes each risk. Name the measurement, not the intention:
"identical node counts and best moves at `Threads=1`" rather than "verify no regression".

State explicitly if no Elo match is needed, and why.

## Harvest

**What survives this document, and where it goes.** Fill this in before opening the PR — it is what
lets the file be deleted rather than accumulating.

| Decision / rationale | Lands in |
|---|---|
| e.g. why the fast path skips the age reset | source comment on `clear()` |
| e.g. a non-obvious cross-cutting contract | `CLAUDE.md` → Key Source Facts |
| e.g. a measured before/after result | `Docs/Changelog.md`, and the PR body |

Prefer destinations inside the tree. A PR body is fine for working detail, but it is editable and
lives outside Git, so anything that would be missed later belongs in a source comment, Key Source
Facts or the changelog as well.

Anything with no destination is either not durable — fine, it dies with the working notes — or it
needs one. Nothing durable should exist only here.

**Also record any approved decision that changed during implementation, and why.** The design review
and the diff review look at different artifacts, so this table is where the two are reconciled.
