// CpuCheck.cpp -- compiled WITHOUT -mavx2 -mbmi2; read StartupCpuCheck.cpp before adding an include
// or a call here. That is why this file includes CpuCheck.h and nothing else, not Common.h, and why
// CPUID and XGETBV are inline asm: <cpuid.h> and <intrin.h> differ per compiler, and _xgetbv needs
// -mxsave.
#include "CpuCheck.h"

namespace CpuCheck {

	namespace {
		constexpr uint32_t LEAF1_ECX_OSXSAVE = 1u << 27;
		constexpr uint32_t LEAF1_ECX_AVX = 1u << 28;
		constexpr uint32_t LEAF7_EBX_AVX2 = 1u << 5;
		constexpr uint32_t LEAF7_EBX_BMI2 = 1u << 8;
		constexpr uint64_t XCR0_SSE_AND_YMM = 0b110; // XCR0 bits 1 and 2: XMM and YMM state

		struct Regs {
			uint32_t eax, ebx, ecx, edx;
		};

		Regs cpuid(uint32_t leaf, uint32_t subleaf) noexcept
		{
			Regs r{};
			__asm__ volatile("cpuid" : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx) : "a"(leaf), "c"(subleaf));
			return r;
		}

		// Faults unless CPUID.1:ECX.OSXSAVE is set; the caller checks.
		uint64_t xgetbv0() noexcept
		{
			Regs r{}; // NOLINT(misc-const-correctness) - written by the asm below
			__asm__ volatile("xgetbv" : "=a"(r.eax), "=d"(r.edx) : "c"(0));
			return (static_cast<uint64_t>(r.edx) << 32) | r.eax;
		}
	} // namespace

	CpuidSnapshot ReadCpu() noexcept
	{
		CpuidSnapshot cpu{};
		cpu.max_leaf = cpuid(0, 0).eax;
		if (cpu.max_leaf >= 1)
			cpu.leaf1_ecx = cpuid(1, 0).ecx;
		if (cpu.max_leaf >= 7)
			cpu.leaf7_ebx = cpuid(7, 0).ebx;
		if ((cpu.leaf1_ecx & LEAF1_ECX_OSXSAVE) != 0)
			cpu.xcr0 = xgetbv0();
		return cpu;
	}

	Verdict Assess(const CpuidSnapshot& cpu) noexcept
	{
		const bool has_leaf7 = cpu.max_leaf >= 7;
		const bool os_saves_ymm =
		    (cpu.leaf1_ecx & LEAF1_ECX_OSXSAVE) != 0 && (cpu.xcr0 & XCR0_SSE_AND_YMM) == XCR0_SSE_AND_YMM;
		const bool cpu_has_avx = (cpu.leaf1_ecx & LEAF1_ECX_AVX) != 0;

		Verdict verdict{};
		verdict.avx2_missing = !(has_leaf7 && (cpu.leaf7_ebx & LEAF7_EBX_AVX2) != 0 && cpu_has_avx && os_saves_ymm);
		verdict.bmi2_missing = !(has_leaf7 && (cpu.leaf7_ebx & LEAF7_EBX_BMI2) != 0);
		return verdict;
	}

	const char* FatalMessage(const Verdict& verdict) noexcept
	{
		if (verdict.avx2_missing && verdict.bmi2_missing)
			return "unsupported CPU: AVX2 and BMI2 are not available. StratChess needs Intel Haswell (2013) / AMD Excavator (2015) "
			       "or newer; a virtual machine may hide them.";
		if (verdict.avx2_missing)
			return "unsupported CPU: AVX2 is not available (or the operating system does not enable it). StratChess needs "
			       "Intel Haswell (2013) / AMD Excavator (2015) or newer; a virtual machine may hide it.";
		if (verdict.bmi2_missing)
			return "unsupported CPU: BMI2 is not available. StratChess needs Intel Haswell (2013) / AMD Excavator (2015) "
			       "or newer; a virtual machine may hide it.";
		return nullptr;
	}

} // namespace CpuCheck
