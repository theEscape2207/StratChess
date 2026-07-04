# StratChess Engine Roadmap

**Last Updated**: July 4, 2026
**Current Version**: AIPerplex 2.0

---

## Overview

The active backlog lives in **GitHub Issues** as of July 2026 (see PR #80) — this file no
longer tracks open work. Use the issue tracker, filtered by:

- **`category:`** labels — `search`, `eval`, `test`, `elo`, `infra`, `refactor`,
  `build-tooling`
- **`priority:`** labels — `critical`, `high`, `medium`, `longterm`
- **`status:incoming`** — a new idea, minimally captured (what + why/potential),
  awaiting a triage sweep before it earns a priority label
- **`type:epic`** — a tracking issue grouping related sub-issues via GitHub's native
  sub-issue links (parent/child, with a progress bar)
- **`low-hanging-fruit`** — small, isolated, no dependencies; good to grab anytime
  without much upfront thought
- GitHub's native **"Blocked by" / "Blocking"** issue links express `#A requires #B`
  relationships directly on the issue, independent of the epic/sub-issue tree

This file retains: general engineering principles (below). The historical record of
completed work has moved to `Docs/Changelog.md`.

---

## Measurement & Success Criteria

### Performance Metrics
- **Nodes per second**: Track baseline, target +20% with optimizations
- **Depth reached**: 15 seconds reaches depth 13-15 (with LMR; was 8-9 before)
- **Win rate**: Maintain or improve vs AIAgent baseline

### Code Quality Metrics
- **Test coverage**: Target 80% for search algorithms
- **Build warnings**: Zero with `/W4` or `-Wall -Wextra`
- **Static analysis**: Zero critical issues (PVS-Studio)

### Regression Prevention
- Save positions from each bug fix as test cases
- Run full test suite before each release
- Track performance on standard benchmark suite

---

## Decision Framework

### When to Implement
Consider this order:
1. **Bug fixes** - Always first
2. **Blocking items** - Required for parallel search
3. **High-impact perf** - LMR, killer moves
4. **Infrastructure** - Testing, profiling (enables confidence)
5. **Advanced features** - NNUE, tablebases (after solid baseline)

### When to Skip
Avoid these traps:
- ❌ Premature optimization (profile first!)
- ❌ Feature creep (parallel search is the north star)
- ❌ Perfect-is-enemy-of-good (working > elegant)
- ❌ Over-engineering (YAGNI principle)

---

**Document Version**: 2.1 — completed-work history moved to `Docs/Changelog.md`
**Owner**: Thees
