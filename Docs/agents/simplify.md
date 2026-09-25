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

4. Read each touched file in full, and no other file. Check every function in it and every comment:
   - **dead:** a function with no caller (search the repo for callers);
   - **stale:** a comment the code contradicts, or one naming a task, a PR or an earlier version;
   - **duplicated:** code that repeats another function in the file or an existing helper;
   - **workaround:** code kept alive only to route around another defect.

   Report each under a separate `Nearby debt` heading with `file:line`. It is not a finding, unless
   the change makes it worse: copies the duplication, extends the workaround.

## Intended patterns

Before reporting an item, check it against this list and drop it on a match. Each was rejected with
the same reason on two PRs; add a pattern when that happens again.

- **`SearchTelemetry`'s per-struct `reset`/`add`/`append_info` edits.** Not Shotgun Surgery unless
  the change adds a new kind of edit.
- **A self-test's expected list that repeats the script's list.** It is the oracle: the test fails
  when a key is dropped from the script.

What the author does with each finding and debt item: skill `open-pull-request` step 1.
