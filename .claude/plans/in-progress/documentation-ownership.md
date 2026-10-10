# Engine documentation ownership — Design

**Origin:** user-approved restructuring in this chat, 2026-10-10.

## Goal

Give engine documentation distinct purposes so usage guidance, architecture and correctness
obligations stop accumulating competing descriptions.

## Review focus

- Preserve diagnostic output definitions and their consumers while removing duplicate internals.
- Check current claims against source, particularly TT concurrency and logging.

## Scope

Restructure Architecture and rename Engine-Readme to EngineGuide; adjust navigation, contracts,
workflow links, profile-script help and the measurement skill. Correct stale documentation found
in that scope. Engine behaviour, historical records and measurement procedures remain unchanged.

## Decisions

### D1: Separate architecture, usage and obligations

Architecture owns responsibilities, dependencies, state lifetimes and flows. EngineGuide owns
practical usage and output interpretation. EngineContracts owns non-obvious obligations. Each
opens with its purpose and ownership boundaries and ends with maintenance guidance. Keeping
parallel descriptions was rejected because they already disagree.

### D2: Keep diagnostics discoverable

Keep the output catalogue in EngineGuide, with links from its consumers; do not discard it with
the old algorithm manual or create another reference document without need. Workflow retains
measurement methodology and routes runtime-file lookup to the guide.

### D3: Preserve history and keep navigation short

README keeps a minimal quick start and a documentation map. CLAUDE routes tasks to the owners.
Dated reviews and changelog history keep their original references. Record a new changelog entry.

## Assumptions I cannot verify from the code

None. This change documents repository behaviour, not external client guarantees.

## Invariants

No executable behaviour changes. Non-obvious contracts and useful telemetry meanings remain
discoverable. Live links resolve after the rename. Each durable topic has one reference owner.

## Validation

Check claims against implementation, local Markdown links and anchors, stale-name references and
the diff. The script-help edit makes this Tooling tier: run required script validation and the
Standards/Spec reviews before the PR. No Elo measurement applies to documentation and help text.

## Cost

Over 200 lines changed across roughly nine files, chiefly removing duplicate material. Tooling
validation and two review axes; no engine build or measurement planned unless the gate requires it.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1: ownership boundaries | Architecture, EngineGuide and EngineContracts purpose/maintenance text |
| D2: diagnostic reference | EngineGuide diagnostics; Workflow, script and skill links |
| D3: navigation and historical references | README, CLAUDE and new Changelog entry |

No approved decision changed. Self-review completed: scope, invariants and validation agree;
all source-dependent claims will be checked during the documentation edits. The user-approved
artifact has already converged; no further design round is needed (Workflow, Cross-agent review).
