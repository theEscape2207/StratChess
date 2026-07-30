#pragma once

// Constructs MSVC provides that GCC and Clang do not. This is the only place in
// the codebase where a compiler is named conditionally; everything else stays
// compiler-neutral and relies on these definitions.

#if defined(_MSC_VER)

#  include <sal.h>
#  define STRAT_FORCEINLINE __forceinline

#else

   // SAL source-annotation macros expand to nothing off MSVC. The annotations are
   // kept in the sources because they document parameter direction and MSVC's
   // analyser still consumes them.
#  define _In_
#  define _Inout_
#  define _Out_

#  define STRAT_FORCEINLINE inline __attribute__((always_inline))

#endif
