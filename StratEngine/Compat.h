#pragma once

// Constructs whose spelling differs between MSVC and GCC/Clang. This is the only
// place in the codebase where a compiler is named conditionally; everything else
// stays compiler-neutral and relies on these definitions.

#include <cstdlib>
#include <optional>
#include <string>

#if defined(_MSC_VER)

#	define STRAT_FORCEINLINE __forceinline

#	define STRAT_NOINLINE __declspec(noinline)

// localtime_s takes (tm*, time_t*) and reports success via a 0 return.
#	define STRAT_LOCALTIME(tm_out, time_in) (localtime_s((tm_out), (time_in)) == 0)

// The CRT deprecates getenv in favour of _dupenv_s, and warnings are errors.
inline std::optional<std::string> StratGetEnv(const char* name)
{
	char* buffer = nullptr;
	size_t length = 0;
	if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr)
		return std::nullopt;
	std::string value(buffer);
	std::free(buffer);
	return value;
}

#else

inline std::optional<std::string> StratGetEnv(const char* name)
{
	const char* value = std::getenv(name);
	return value ? std::optional<std::string>(value) : std::nullopt;
}

#	define STRAT_FORCEINLINE inline __attribute__((always_inline))

#	define STRAT_NOINLINE __attribute__((noinline))

#	include <ctime>
// POSIX localtime_r takes its arguments in the opposite order to localtime_s
// (time_t* first, tm* second) and reports success via a non-null return.
#	define STRAT_LOCALTIME(tm_out, time_in) (localtime_r((time_in), (tm_out)) != nullptr)

#endif
