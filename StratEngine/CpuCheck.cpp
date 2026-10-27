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

		// "AuthenticAMD" as CPUID.0 returns it in EBX, EDX, ECX.
		constexpr uint32_t VENDOR_AMD_EBX = 0x68747541; // "Auth"
		constexpr uint32_t VENDOR_AMD_EDX = 0x69746E65; // "enti"
		constexpr uint32_t VENDOR_AMD_ECX = 0x444D4163; // "cAMD"

		constexpr uint32_t FAMILY_17H = 0x17; // Zen 1, Zen+ and Zen 2

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

		// Base family, plus the extended family when the base is 0xF (Intel's and AMD's rule alike).
		uint32_t family_of(uint32_t leaf1_eax) noexcept
		{
			const uint32_t base = (leaf1_eax >> 8) & 0xF;
			const uint32_t extended = (leaf1_eax >> 20) & 0xFF;
			return base == 0xF ? base + extended : base;
		}
	} // namespace

	CpuidSnapshot ReadCpu() noexcept
	{
		CpuidSnapshot cpu{};
		const Regs leaf0 = cpuid(0, 0);
		cpu.max_leaf = leaf0.eax;
		cpu.vendor_ebx = leaf0.ebx;
		cpu.vendor_edx = leaf0.edx;
		cpu.vendor_ecx = leaf0.ecx;
		if (cpu.max_leaf >= 1) {
			const Regs leaf1 = cpuid(1, 0);
			cpu.leaf1_eax = leaf1.eax;
			cpu.leaf1_ecx = leaf1.ecx;
		}
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

		const bool amd =
		    cpu.vendor_ebx == VENDOR_AMD_EBX && cpu.vendor_edx == VENDOR_AMD_EDX && cpu.vendor_ecx == VENDOR_AMD_ECX;
		verdict.slow_pext = amd && family_of(cpu.leaf1_eax) == FAMILY_17H;
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

	const char* WarningMessage(const Verdict& verdict) noexcept
	{
		if (verdict.slow_pext)
			return "PEXT is slow on this AMD Zen 1/2 CPU (family 17h): StratChess runs correctly but at a fraction of its normal speed.";
		return nullptr;
	}

} // namespace CpuCheck
