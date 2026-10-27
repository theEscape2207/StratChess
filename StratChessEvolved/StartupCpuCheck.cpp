// StartupCpuCheck.cpp -- refuses to run on a CPU the build cannot run on, and warns about slow PEXT.
//
// Compiled WITHOUT -mavx2 -mbmi2 (CMakeLists.txt, "Startup CPU check"). Rules that keep it safe:
//  - Run before any code compiled WITH them. main() is too late: static initializers run first, and
//    so does the std::cerr initializer, whose std::basic_ios::init is the AVX2 COMDAT copy from this
//    repo's objects. A C++ initializer group is too late for the same reason. The hook is therefore
//    an entry in the CRT's C initializer table (.CRT$XIU), which the CRT runs before every C++
//    initializer; elsewhere it is constructor priority 101, ahead of the default-priority ones.
//  - Call nothing that the compiler emits as shared inline code: STL, <stdio.h>'s inline wrappers
//    (fprintf, for one), Common.h. The linker may resolve such a COMDAT to an AVX2-compiled copy
//    from another object. fputs and _Exit are plain library functions.
//  - Keep CpuCheck.cpp and this file on the per-source -mno-avx -mno-avx2 -mno-bmi -mno-bmi2 options.
// A dropped flag or a moved table entry leaves a green build, so check the linked exe: this entry
// must sit in the linker map's __xi_a..__xi_z table (the CRT runs it before __xc_a..__xc_z), and
// llvm-objdump of everything it reaches must show no VEX or BMI instruction.
#include "CpuCheck.h"
#include <cstdio>
#include <cstdlib>

namespace {
	// stderr: stdout is the UCI channel, and before 'uci' arrives there is no handshake to put a
	// diagnostic in.
	void report(const char* prefix, const char* message) noexcept
	{
		std::fputs(prefix, stderr);
		std::fputs(message, stderr);
		std::fputs("\n", stderr);
	}

	void run_startup_cpu_check() noexcept
	{
		const CpuCheck::Verdict verdict = CpuCheck::Assess(CpuCheck::ReadCpu());

		// _Exit, not exit: no destructor or atexit handler has anything to do yet.
		if (const char* fatal = CpuCheck::FatalMessage(verdict)) {
			report("Fatal: ", fatal);
			std::_Exit(1);
		}
		if (const char* warning = CpuCheck::WarningMessage(verdict))
			report("Warning: ", warning);
	}
} // namespace

#if defined(_MSC_VER)
#	pragma section(".CRT$XIU", read)
namespace {
	// The CRT calls each entry of the C initializer table and stops at the first nonzero result.
	int __cdecl early_startup_check() noexcept
	{
		run_startup_cpu_check();
		return 0;
	}

	// `used` keeps LTO from discarding a variable nothing references.
	[[gnu::used]]
	__declspec(allocate(".CRT$XIU")) int(__cdecl* const early_startup_check_entry)() = early_startup_check;
} // namespace
#else
namespace {
	[[gnu::constructor(101)]] void early_startup_check() noexcept { run_startup_cpu_check(); }
} // namespace
#endif
