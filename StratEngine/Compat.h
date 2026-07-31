#pragma once

// Constructs MSVC provides that GCC and Clang do not. This is the only place in
// the codebase where a compiler is named conditionally; everything else stays
// compiler-neutral and relies on these definitions.

#if defined(_MSC_VER)

#  include <sal.h>
#  define STRAT_FORCEINLINE __forceinline

// localtime_s takes (tm*, time_t*) and reports success via a 0 return.
#  define STRAT_LOCALTIME(tm_out, time_in) (localtime_s((tm_out), (time_in)) == 0)

#else

   // SAL source-annotation macros expand to nothing off MSVC. The annotations are
   // kept in the sources because they document parameter direction and MSVC's
   // analyser still consumes them.
#  define _In_
#  define _Inout_
#  define _Out_

#  define STRAT_FORCEINLINE inline __attribute__((always_inline))

#  include <ctime>
// POSIX localtime_r takes its arguments in the opposite order to localtime_s
// (time_t* first, tm* second) and reports success via a non-null return.
#  define STRAT_LOCALTIME(tm_out, time_in) (localtime_r((time_in), (tm_out)) != nullptr)

#endif
