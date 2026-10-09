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

// Prefetches the cache line holding `address` into every cache level (prefetcht0, read, high
// locality). clang-cl accepts _mm_prefetch(p, _MM_HINT_T0) but compiles it to prefetcht2, so clang
// and GCC take the builtin, and only MSVC the intrinsic. A prefetch never faults.
#if defined(__clang__) || defined(__GNUC__)
inline void StratPrefetch(const void* address) noexcept { __builtin_prefetch(address); }
#else
#	include <xmmintrin.h>
inline void StratPrefetch(const void* address) noexcept
{
	_mm_prefetch(static_cast<const char*>(address), _MM_HINT_T0);
}
#endif
