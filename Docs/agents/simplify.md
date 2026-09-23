# Simplify lens

A standards source for code review, read beside CLAUDE.md → Development Guidelines, which wins where
the two disagree. It asks what the change could do without. Every item it raises is a judgement
call.

## Per hunk

1. **Comments.** A comment that restates the code, refers to a task, a PR or an earlier version, or
   runs past two lines without recording a key fact or tripwire.
2. **Over-detail.** Anything the change does not need:
   - an option or parameter only ever given one value;
   - an abstraction with a single caller;
   - validation against a caller that cannot exist (CLAUDE.md → Threat model);
   - diagnostics left over from debugging.
3. **Reuse.** New code that repeats a helper already in the codebase or the standard library. Name
   the helper.

## Nearby debt

4. Scan the whole of every function the diff changes and the rest of each touched file, and no other
   file, for code that is already duplicated, dead or stale, or kept alive only by a workaround.
   Report it under a separate `Nearby debt` heading with `file:line`. It is not a finding, unless
   the change makes it worse: copies the duplication, extends the workaround.

What the author does with each finding and debt item: skill `open-pull-request` step 1.
