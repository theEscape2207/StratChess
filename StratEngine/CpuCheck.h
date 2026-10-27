// CpuCheck.h -- what the CPU must provide to run this build, and the verdict on what it reports.
#pragma once

#include <cstdint>

// The build is compiled with -mavx2 -mbmi2, so the check must itself run on a CPU that has neither:
// CpuCheck.cpp and StratChessEvolved/StartupCpuCheck.cpp are built without them, and every rule that
// keeps them safe is in StartupCpuCheck.cpp and Docs/EngineContracts.md. For this header it means no
// inline function and no STL type: an inline function is a COMDAT the linker may resolve to an
// AVX2-compiled copy from another object. The structs below are therefore plain aggregates (no
// default member initializers, whose implicit constructor would be one); value-initialise them with {}.
namespace CpuCheck {

	// The CPUID and XGETBV words the verdict depends on, as the hardware returns them.
	struct CpuidSnapshot {
		uint32_t max_leaf;   // CPUID.0:EAX, the highest standard leaf
		uint32_t vendor_ebx; // CPUID.0 vendor string, "AuthenticAMD" = "Auth" "enti" "cAMD"
		uint32_t vendor_edx; //   in EBX, EDX, ECX order
		uint32_t vendor_ecx;
		uint32_t leaf1_eax; // CPUID.1: family, model, stepping
		uint32_t leaf1_ecx; // CPUID.1: AVX and OSXSAVE bits
		uint32_t leaf7_ebx; // CPUID.(7,0): AVX2 and BMI2 bits; 0 when max_leaf < 7
		uint64_t xcr0;      // XGETBV(0): the state the OS saves; 0 when OSXSAVE is clear
	};

	struct Verdict {
		bool avx2_missing; // not in the CPU, or the OS does not save YMM state
		bool bmi2_missing;
		bool slow_pext; // AMD family 17h (Zen 1, Zen+, Zen 2): PEXT is microcoded
	};

	// Reads this CPU. Safe on any x86-64 CPU.
	CpuidSnapshot ReadCpu() noexcept;

	// Pure: the same snapshot always gives the same verdict.
	Verdict Assess(const CpuidSnapshot& cpu) noexcept;

	// One line, no trailing newline, naming the missing feature(s); nullptr when none is missing.
	const char* FatalMessage(const Verdict& verdict) noexcept;

	// One line, no trailing newline, for a CPU that runs the engine badly; nullptr when there is none.
	const char* WarningMessage(const Verdict& verdict) noexcept;

} // namespace CpuCheck
