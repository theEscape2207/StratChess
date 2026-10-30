# Matt Pocock skills: upstream refresh

On 2026-10-30, the existing 24 Codex skill trees in `skills-lock.json` were compared with and
refreshed from [v1.3.2](https://github.com/mattpocock/skills/releases/tag/v1.3.2), published
2026-10-29. The release resolves to
[`d1478fcb140db0b93c0c8df17bcfd311844749d0`](https://github.com/mattpocock/skills/tree/d1478fcb140db0b93c0c8df17bcfd311844749d0),
not the moving upstream main branch. All vendored files match that revision after LF normalization;
lock hashes use the skills CLI's SHA-256 over sorted relative paths and file contents.

## Integration impact

| Area | Effect of the refresh |
|---|---|
| Subagents | Review, design, research, grilling, wayfinding and chief-of-staff request fresh context windows. Codex dispatches these with `fork_turns: "none"` and a self-contained brief. |
| Code review | Searches for standards files, includes `CODING_STANDARDS.md` / `CONTRIBUTING.md` when present, consumes returned foreground reports, and uses the supplied tracker doc. The local PR skill still selects review mode and supplies `Docs/CodingStandards.md`. |
| Diagnosis / TDD | Verifies forced mutations against pristine copies; proposed test seams explain what they catch and miss. Existing test-design rules still apply. |
| Chief of staff | Adds an interlinked wiki with ingest/query/lint operations, dated claims and commit citations for code. Its location is agreed before writing; this update creates no wiki or schedule. |
| Implementation / questions / teaching | Fetches ticket titles before implementation, words grilling recommendations so yes accepts them, and clarifies teaching workspace paths and quiz answer ordering. |
| Setup / tracker | Checks repo reachability, creates missing configured labels during setup, fixes tracker commands, and offers legacy glossary migration. The project's tracker was refreshed directly, retaining body-file writes, script-mediated PRs, disabled PR triage and existing label mappings. |
| Wayfinder | Uses real issue IDs, resolves ticket types from labels, avoids implementation triage labels and PRs for research branches. Restored tracker operations for the already-vendored skill; project push rules still govern publishing. |
| Wizard | Fixes Readline input, EOF handling, dotenv quoting and defaults, symlink/mode preservation, browser/terminal fallback, and protects stages from edits during a run. |

Some release-note fixes were already present in the installed snapshot (including handoff temp
paths, implement Skill-tool calls, and to-tickets parent links), so those files did not change.
`ask-matt` and `implement-spec` remain outside this repo's selected Codex set. Project-owned
adapters and canonical skills under `.claude/skills/` were retained.

### Known wizard limitations

The upstream `write_env` encodes an apostrophe using shell quoting: `KEY='a'\''b'`. This
round-trips through Bash and `_existing`, but Node's `util.parseEnv` reads the value as `a`.
When generating a wizard, verify its output with the application's actual dotenv consumer before
relying on values containing apostrophes; use that consumer's encoding if needed. The vendored
template remains the upstream release, including this limitation.

Writing through the existing file preserves symlinks and modes, at the cost of atomic replacement:
an interrupted write can leave partial content. This update creates no generated wizard or `.env`.

At this refresh, `Docs/agents/domain.md` correctly points to the existing `CONTEXT.md`. A glossary
rename is a separate migration; the setup skill's new offer does not require renaming it now.

Claude Code uses the marketplace with `autoUpdate: true` in `.claude/settings.json`; its downloaded
plugin cache is outside this commit. The PR updates Codex's repository-local copies and shared
tracker docs, and does not claim to update a machine's Claude plugin cache.

## Future refreshes

Compare every locked skill directory with the chosen release, including support files. Copy only
the selected upstream trees, preserving project adapters. Recompute folder hashes over LF files;
verify unchanged skills too. Refresh generated `Docs/agents/` guidance by merging the applicable
template changes, preserving project rules. Executable templates retain their normal validation
tier; changing a skill's prose does not exempt its scripts.
