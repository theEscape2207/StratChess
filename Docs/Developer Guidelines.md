# Developer Guidelines – Modern C++ Chess Engine
## Context
This project focuses on developing a modern chess engine using C++20. The aim is to improve playing strength (ELO) 
while maintaining clarity, efficiency, and robustness in design and implementation.

## Objectives
### Performance
- Continuously improve the engine’s perceived ELO through algorithmic and low-level optimizations.

- Use CPU and memory efficiently, profiling regularly to identify and resolve bottlenecks.

- Prioritize optimizations that yield measurable gains in both speed and search accuracy.

### Maintainability
- Keep code clear, modular, and consistent in style.
- Document non-trivial logic, performance-critical paths, and design decisions.
- Favor modern C++ idioms (RAII, strong types, move semantics, constexpr) to minimize complexity and unintended side effects.
- Language in naming and commenting should be precise, unambiguous and in English.

### Quality Assurance
- Verify all changes through testing, benchmarking, or controlled match comparisons.
- Maintain deterministic behavior to ensure reproducibility during testing and debugging.
- Extend test coverage when introducing or modifying core logic.
- Track performance metrics to ensure no unintentional regressions in search accuracy or ELO.

### Reuse and Dependencies
- Leverage the C++ standard library whenever feasible.
- Use only approved header-only dependencies for non-chess domains:
  * spdlog (logging)
  * nlohmann/json (configuration, serialization)
  * Catch2 (unit testing)
- Do not introduce new dependencies unless justified and reviewed through design documentation.

Engine features and data structures: `Docs/Engine-Readme.md`. Non-obvious API contracts:
`Docs/EngineContracts.md`.

## Development Constraints
- Must maintain or improve search accuracy and ELO; any regression must be explicitly justified.
- No new external dependencies without approval. Ask for exceptions with a clear rationale.
- All changes must be thread-safe, especially those touching shared data paths or transposition tables.
- Integrate changes cleanly within the existing codebase and established architecture.
- Prioritize clarity over low-level micro-optimizations unless significantly justified by measurable results.
- Provide documentation for algorithms and heuristics that impact search or evaluation behavior.
- Favor compile-time computation (constexpr), move semantics, and zero-cost abstractions to reduce runtime overhead.

## Best Practices
- Keep pull requests small, logically scoped, and well-documented
- Include motivation, design reasoning, and expected impact for major changes PRs or when checking in.
- Benchmark search speed and strength before and after optimizations.
- Maintain consistency in formatting, naming, and file structure.
- Seek review for any changes impacting evaluation, move ordering, or search algorithms.
- Focus on incremental improvement, ensuring stability through continuous validation and self-play testing.