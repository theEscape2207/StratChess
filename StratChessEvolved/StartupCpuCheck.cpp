// StartupCpuCheck.cpp -- refuses to run on a CPU the build cannot run on, and warns about slow PEXT.
//
// Compiled WITHOUT -mavx2 -mbmi2 (CMakeLists.txt, "Startup CPU check"). Two rules keep it that way:
//  - Run before any code compiled WITH them. main() is too late: static initializers run first, and
//    so does the std::cerr initializer's std::basic_ios::init, whose linked copy is ours, AVX2 and
//    all. The hook therefore goes in the CRT's C initializer table (.CRT$XIU), which the CRT runs
//    before every C++ initializer; a C++ init_seg(compiler) hook still ran after std::cerr's.
//    Elsewhere it is constructor priority 101, ahead of the default-priority constructors.
//  - Call nothing that the compiler emits as shared inline code: STL, and the <stdio.h> inline
//    wrappers (fprintf, for one). The linker may resolve such a COMDAT to an AVX2-compiled copy from
//    another object, as it did for basic_ios::init. fputs and _Exit are plain library functions.
// A dropped flag or a moved table entry leaves a green build, so check the linked exe: this entry
// must sit in the linker map's __xi_a..__xi_z table (the CRT runs it before __xc_a..__xc_z), and
// llvm-objdump of everything it reaches must show no VEX or BMI instruction.
#include "CpuCheck.h"
#include <cstdio>
#include <cstdlib>

namespace {
	void run_startup_cpu_check() noexcept
	{
		const CpuCheck::Verdict verdict = CpuCheck::Assess(CpuCheck::ReadCpu());

		// stderr: stdout is the UCI channel, and before 'uci' arrives there is no handshake to
		// put a diagnostic in. _Exit, not exit: no destructor or atexit handler has anything to do yet.
		if (const char* fatal = CpuCheck::FatalMessage(verdict)) {
			std::fputs("Fatal: ", stderr);
			std::fputs(fatal, stderr);
			std::fputs("\n", stderr);
			std::_Exit(1);
		}
		if (verdict.slow_pext) {
			std::fputs("Warning: ", stderr);
			std::fputs(CpuCheck::SlowPextMessage(), stderr);
			std::fputs("\n", stderr);
		}
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
