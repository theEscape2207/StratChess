// CpuCheckTests.cpp -- Catch2 [cpu] tests for the startup CPU check's decision logic: the verdict
// over raw CPUID words, the diagnostic wording, and the host read. The pre-main hook itself cannot be
// unit-tested; its placement is verified on the linked exe (see StratChessEvolved/StartupCpuCheck.cpp).

#include <catch2/catch_test_macros.hpp>
#include "CpuCheck.h"
#include <cstring>
#include <string_view>

using namespace CpuCheck;

namespace {
	constexpr uint32_t LEAF1_ECX_OSXSAVE = 1u << 27;
	constexpr uint32_t LEAF1_ECX_AVX = 1u << 28;
	constexpr uint32_t LEAF7_EBX_AVX2 = 1u << 5;
	constexpr uint32_t LEAF7_EBX_BMI2 = 1u << 8;
	constexpr uint64_t XCR0_X87_SSE_AVX = 0b111; // x87, SSE and YMM state enabled

	// CPUID.1:EAX of real parts: family = base, plus the extended family when the base is 0xF.
	constexpr uint32_t EAX_INTEL_HASWELL = 0x000306C3;  // family 6
	constexpr uint32_t EAX_AMD_ZEN1_RYZEN = 0x00800F11; // family 0x17
	constexpr uint32_t EAX_AMD_ZEN2_RYZEN = 0x00870F10; // family 0x17
	constexpr uint32_t EAX_AMD_ZEN3_RYZEN = 0x00A20F10; // family 0x19
	constexpr uint32_t EAX_AMD_ZEN5_RYZEN = 0x00B40F40; // family 0x1A
	constexpr uint32_t EAX_AMD_EXCAVATOR = 0x00660F51;  // family 0x15

	// The 12-byte vendor string, little-endian, in CPUID.0's EBX, EDX, ECX order.
	constexpr uint32_t word(const char (&text)[5])
	{
		return static_cast<uint32_t>(static_cast<unsigned char>(text[0])) |
		       static_cast<uint32_t>(static_cast<unsigned char>(text[1])) << 8 |
		       static_cast<uint32_t>(static_cast<unsigned char>(text[2])) << 16 |
		       static_cast<uint32_t>(static_cast<unsigned char>(text[3])) << 24;
	}

	CpuidSnapshot supported_cpu(uint32_t leaf1_eax, bool amd)
	{
		CpuidSnapshot cpu{};
		cpu.max_leaf = 0xD;
		cpu.vendor_ebx = amd ? word("Auth") : word("Genu");
		cpu.vendor_edx = amd ? word("enti") : word("ineI");
		cpu.vendor_ecx = amd ? word("cAMD") : word("ntel");
		cpu.leaf1_eax = leaf1_eax;
		cpu.leaf1_ecx = LEAF1_ECX_OSXSAVE | LEAF1_ECX_AVX;
		cpu.leaf7_ebx = LEAF7_EBX_AVX2 | LEAF7_EBX_BMI2;
		cpu.xcr0 = XCR0_X87_SSE_AVX;
		return cpu;
	}

	CpuidSnapshot haswell() { return supported_cpu(EAX_INTEL_HASWELL, false); }
	CpuidSnapshot ryzen(uint32_t leaf1_eax) { return supported_cpu(leaf1_eax, true); }
} // namespace

TEST_CASE("CpuCheck: a Haswell-class CPU is supported and unremarkable", "[cpu]")
{
	const Verdict verdict = Assess(haswell());
	REQUIRE_FALSE(verdict.avx2_missing);
	REQUIRE_FALSE(verdict.bmi2_missing);
	REQUIRE_FALSE(verdict.slow_pext);
}

TEST_CASE("CpuCheck: each missing feature is reported by name", "[cpu]")
{
	SECTION("AVX2 bit clear")
	{
		CpuidSnapshot cpu = haswell();
		cpu.leaf7_ebx = LEAF7_EBX_BMI2;
		const Verdict verdict = Assess(cpu);
		REQUIRE(verdict.avx2_missing);
		REQUIRE_FALSE(verdict.bmi2_missing);
	}
	SECTION("BMI2 bit clear")
	{
		CpuidSnapshot cpu = haswell();
		cpu.leaf7_ebx = LEAF7_EBX_AVX2;
		const Verdict verdict = Assess(cpu);
		REQUIRE_FALSE(verdict.avx2_missing);
		REQUIRE(verdict.bmi2_missing);
	}
	SECTION("both clear")
	{
		CpuidSnapshot cpu = haswell();
		cpu.leaf7_ebx = 0;
		const Verdict verdict = Assess(cpu);
		REQUIRE(verdict.avx2_missing);
		REQUIRE(verdict.bmi2_missing);
	}
}

TEST_CASE("CpuCheck: a CPU without leaf 7 has neither feature, whatever EBX holds", "[cpu]")
{
	CpuidSnapshot cpu = haswell();
	cpu.max_leaf = 6;
	const Verdict verdict = Assess(cpu);
	REQUIRE(verdict.avx2_missing);
	REQUIRE(verdict.bmi2_missing);
}

TEST_CASE("CpuCheck: AVX2 needs the OS to save YMM state, not only the CPUID bit", "[cpu]")
{
	SECTION("OSXSAVE clear: XGETBV would fault, so XCR0 is not trusted")
	{
		CpuidSnapshot cpu = haswell();
		cpu.leaf1_ecx = LEAF1_ECX_AVX;
		REQUIRE(Assess(cpu).avx2_missing);
	}
	SECTION("XCR0 enables SSE but not YMM")
	{
		CpuidSnapshot cpu = haswell();
		cpu.xcr0 = 0b011;
		REQUIRE(Assess(cpu).avx2_missing);
	}
	SECTION("XCR0 enables YMM but not SSE state")
	{
		CpuidSnapshot cpu = haswell();
		cpu.xcr0 = 0b101;
		REQUIRE(Assess(cpu).avx2_missing);
	}
	SECTION("the AVX bit is clear")
	{
		CpuidSnapshot cpu = haswell();
		cpu.leaf1_ecx = LEAF1_ECX_OSXSAVE;
		REQUIRE(Assess(cpu).avx2_missing);
	}
	SECTION("extra XCR0 bits (AVX-512 state) do not matter")
	{
		CpuidSnapshot cpu = haswell();
		cpu.xcr0 = 0xE7;
		REQUIRE_FALSE(Assess(cpu).avx2_missing);
	}
	SECTION("BMI2 needs no OS support")
	{
		CpuidSnapshot cpu = haswell();
		cpu.leaf1_ecx = 0;
		cpu.xcr0 = 0;
		const Verdict verdict = Assess(cpu);
		REQUIRE(verdict.avx2_missing);
		REQUIRE_FALSE(verdict.bmi2_missing);
	}
}

TEST_CASE("CpuCheck: PEXT is slow on AMD family 17h only", "[cpu]")
{
	SECTION("Zen 1 and Zen 2 are family 0x17")
	{
		REQUIRE(Assess(ryzen(EAX_AMD_ZEN1_RYZEN)).slow_pext);
		REQUIRE(Assess(ryzen(EAX_AMD_ZEN2_RYZEN)).slow_pext);
	}
	SECTION("the slow-PEXT parts still count as supported")
	{
		const Verdict verdict = Assess(ryzen(EAX_AMD_ZEN2_RYZEN));
		REQUIRE_FALSE(verdict.avx2_missing);
		REQUIRE_FALSE(verdict.bmi2_missing);
	}
	SECTION("Zen 3 and later, and Excavator, are not")
	{
		REQUIRE_FALSE(Assess(ryzen(EAX_AMD_ZEN3_RYZEN)).slow_pext);
		REQUIRE_FALSE(Assess(ryzen(EAX_AMD_ZEN5_RYZEN)).slow_pext);
		REQUIRE_FALSE(Assess(ryzen(EAX_AMD_EXCAVATOR)).slow_pext);
	}
	SECTION("another vendor with the same family number is not")
	{
		REQUIRE_FALSE(Assess(supported_cpu(EAX_AMD_ZEN2_RYZEN, false)).slow_pext);
	}
	SECTION("Intel Haswell is not") { REQUIRE_FALSE(Assess(haswell()).slow_pext); }
	SECTION("the extended family counts only when the base family is 0xF")
	{
		// Base family 6 with extended family 0x11 is family 6; summing them would make 0x17.
		const uint32_t eax = 0x01100600;
		REQUIRE_FALSE(Assess(ryzen(eax)).slow_pext);
	}
}

TEST_CASE("CpuCheck: the fatal line names exactly the missing features", "[cpu]")
{
	const auto contains = [](const char* text, std::string_view needle) {
		return text != nullptr && std::string_view(text).contains(needle);
	};

	Verdict verdict{};
	REQUIRE(FatalMessage(verdict) == nullptr);

	verdict.avx2_missing = true;
	REQUIRE(contains(FatalMessage(verdict), "AVX2"));
	REQUIRE_FALSE(contains(FatalMessage(verdict), "BMI2"));

	verdict = {};
	verdict.bmi2_missing = true;
	REQUIRE(contains(FatalMessage(verdict), "BMI2"));
	REQUIRE_FALSE(contains(FatalMessage(verdict), "AVX2"));

	verdict.avx2_missing = true;
	REQUIRE(contains(FatalMessage(verdict), "AVX2"));
	REQUIRE(contains(FatalMessage(verdict), "BMI2"));
}

TEST_CASE("CpuCheck: every message is one line, since each is sent as one", "[cpu]")
{
	Verdict avx2{};
	avx2.avx2_missing = true;
	Verdict bmi2{};
	bmi2.bmi2_missing = true;
	Verdict both{};
	both.avx2_missing = both.bmi2_missing = true;
	Verdict slow{};
	slow.slow_pext = true;

	for (const char* text : {FatalMessage(avx2), FatalMessage(bmi2), FatalMessage(both), WarningMessage(slow)}) {
		REQUIRE(text != nullptr);
		REQUIRE(std::strlen(text) > 0);
		REQUIRE_FALSE(std::string_view(text).contains('\n'));
	}
	REQUIRE(std::string_view(WarningMessage(slow)).contains("PEXT"));
}

TEST_CASE("CpuCheck: a verdict with nothing wrong has no warning", "[cpu]")
{
	REQUIRE(WarningMessage(Verdict{}) == nullptr);
	REQUIRE(WarningMessage(Assess(haswell())) == nullptr);
	REQUIRE(WarningMessage(Assess(ryzen(EAX_AMD_ZEN2_RYZEN))) != nullptr);
}

TEST_CASE("CpuCheck: the machine running the suite passes its own check", "[cpu]")
{
	// This binary is built with -mavx2 -mbmi2 and cannot have started otherwise, so a verdict of
	// "missing" here means ReadCpu() or Assess() misreads a real CPU.
	const CpuidSnapshot cpu = ReadCpu();
	REQUIRE(cpu.max_leaf >= 7);
	const Verdict verdict = Assess(cpu);
	REQUIRE_FALSE(verdict.avx2_missing);
	REQUIRE_FALSE(verdict.bmi2_missing);
}
