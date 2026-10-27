# Startup CPU check — Design

**Issue:** #928

## Goal

The exe is compiled with `-mavx2 -mbmi2`, and nothing checks the CPU. On a CPU without them the
engine dies on an illegal instruction, with no message; on AMD Zen 1/2 (family 17h) PEXT is
microcoded, so the engine runs correctly at a fraction of its speed with no hint why. Add a startup
check that names a missing feature and exits cleanly, and warns about slow PEXT.

## Review focus

- D1: the check really runs before the first AVX2/BMI2 instruction. It is the claim that decides
  whether the feature works at all.
- D2: the no-ISA translation unit stays free of shared (COMDAT) STL code and of the target's flags.

## Scope

**This change will:**

- Read CPUID leaves 0, 1 and 7 and XCR0 in a translation unit built without AVX/BMI, and decide a
  verdict in a pure function.
- Run the verdict in an early-init hook of the engine exe: a missing feature prints one `Fatal:`
  line on stderr and exits 1; family 17h prints a `Warning:` line and continues.
- Send the family 17h warning once more as `info string` after `uciok`.

**This change will not:**

- Add a non-PEXT fallback or runtime dispatch (#928 out of scope).
- Put the hook in `StratChessTests`; the test exe has its own `main`, and a stderr warning on every
  test run helps nobody.
- Send the fatal line as `info string`: before `uci` arrives the engine has no handshake to put it in.

## Decisions

### D1: an early-init hook, not the top of `main`

`main` is compiled with `-mavx2`, so the compiler may use VEX encodings in its prologue, and static
initializers run before it. Measured on the pre-change Release exe: `??__EkLmrLn@@` (the LMR log
table, `.CRT$XCU`) executes `vcvtsi2sd`/`vmovsd` before `main`. A check at the top of `main` is too
late for any CPU without AVX.

Chosen: `#pragma init_seg(compiler)` on clang-cl (`.CRT$XCC`, which the CRT runs before the user
`.CRT$XCU` group) and `__attribute__((constructor(101)))` on GCC/clang (before every default-priority
constructor). Rejected: top of `main` (above), and `.CRT$XI*` C initializers (undocumented
spelling, no GCC counterpart).

### D2: the check lives in two translation units built with `-mno-avx -mno-avx2 -mno-bmi -mno-bmi2`

`StratEngine/CpuCheck.cpp` (CPUID, XCR0, verdict) and `StratChessEvolved/StartupCpuCheck.cpp` (the
hook) get a per-source `COMPILE_OPTIONS`, appended after the target's `-mavx2 -mbmi2`.
`-mno-avx` is needed as well as `-mno-avx2`: AVX2 implies AVX, and `-mno-avx2` alone leaves VEX
encodings legal for pre-Sandy-Bridge CPUs. Both files use only `<cstdint>`/`<cstdio>`/`<cstdlib>`
and inline asm: an STL template instantiated here is a COMDAT the linker may resolve to an
AVX2-compiled copy from another object, which would put AVX2 back on the check's path.

`cpuid` and `xgetbv` are inline asm, one spelling for clang-cl and GCC. `<intrin.h>`'s `_xgetbv` needs
`-mxsave`, which this TU does not have.

### D3: verdict from raw register values

`Assess(CpuidSnapshot)` takes the raw EAX/EBX/ECX/EDX words and XCR0, so the bit positions and the
family arithmetic (base family 0xF adds the extended family) are the tested part, not just an
`if`. AVX2 counts as present only with the AVX and OSXSAVE bits and XCR0[2:1] == 11b: a CPU that has
the bit under an OS that does not save YMM state still faults.

### D4: the UCI notice is injected

`UciHandler::SetStartupNotice(std::string)`, empty by default, called from `main` with the
warning. `cmd_uci` sends it as `info string` after `benchcontract`, so the handshake's `uciok` line
and the existing sequence are unchanged. Tests inject a notice instead of depending on the CPU of the machine
running them.

## Assumptions I cannot verify from the code

- A CPU without AVX2/BMI2 behaves as the check assumes (no machine at hand; Intel SDE is not
  installed). Verified instead by: unit tests over synthetic register values, and a temporary build
  whose `ReadCpu()` reports the feature missing, run to confirm the line, exit code 1 and an empty stdout.
- The CRT runs `.CRT$XCC` before `.CRT$XCU`. Verified from the linked exe: the position of the hook
  in the `__xc_a`..`__xc_z` table, and an instruction scan of every initializer before it.

## Invariants

- On a supported CPU other than family 17h, stdout and stderr are unchanged and no code runs on the
  search path.
- Nothing the hook can reach contains a VEX, AVX2 or BMI instruction (scanned on the Release exe).
- `uciok` is never preceded by the notice; the notice is always an `info string` line.

## Validation

Catch2: verdict over synthetic CPUs (missing AVX2, BMI2, both, OS without YMM state, AMD 17h and
neighbours, Intel with the same family number), message wording, `ReadCpu()` on the test machine,
`cmd_uci` notice placement. Release exe: the initializer-table scan above;
`Compare-SearchEquivalence.ps1` identical; a paired `Compare-Bench.ps1` for nps. No Elo match: the
search is untouched.

## Cost

Size: 50-200 lines, 6 new files, `CMakeLists.txt`, `UCIHandler`, `main`, `EngineGuide.md`. Review: one
code review.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Why a hook and not `main`; why two no-ISA TUs and no STL | comments at the top of `StartupCpuCheck.cpp` and `CMakeLists.txt` |
| The user-visible diagnostic | `Docs/EngineGuide.md` → Versions and requirements |
| How the claim was verified | PR body |
