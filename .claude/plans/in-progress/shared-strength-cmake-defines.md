# Shared strength-lab CMake defines — Design

**Issue:** #505

## Goal

Let a strength run compile a gated feature without a throwaway branch while preserving enough
provenance to tell later which configuration the measurement used.

## Scope

**This change will:**

- accept only whitespace-separated `-DNAME=VALUE` arguments, apply them to both builds, and report
  them in the run summary.

**This change will not:**

- require candidate and reference binaries to have different hashes.

## Decisions

### D1: Pass one validated argument list to both configure steps

A single input preserves the lab's same-toolchain, same-flags invariant. Separate per-side inputs are
rejected because their asymmetry could create a plausible but invalid Elo result.

### D2: Do not assert different binary hashes

The alternative would catch some null tests, but an unconditional assertion would also reject the
documented calibration where `reference_ref` is the candidate's own SHA. Hash enforcement therefore
needs a separately specified rule that distinguishes calibration from accidental null measurements.

## Assumptions I cannot verify from the code

GitHub Actions passes a dispatch input assigned through `env` as literal data. A throwaway draft PR
and short calibration run are the required external verification.

## Invariants

- Candidate and reference receive byte-for-byte identical CMake argument tokens.
- Invalid input fails in `setup`, before either engine is built.
- The run report records the exact accepted input, or `none` when empty.

## Validation

This is Build tier. Parse the workflow, exercise accepted and rejected validator cases locally, run
the repository pre-PR gate, then use a throwaway draft PR calibration before relying on the input for
an Elo result. No Elo match is needed because this changes the measurement harness, not the engine.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| shared validated defines and summary provenance | `.github/workflows/strength.yml`, `Docs/CI.md` |
| no binary-hash assertion in this change | PR body |
| user-visible outcome | `Docs/Changelog.md` |

