#pragma once

#include "Compat.h"

#if defined(_WIN32)
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0600
#  endif
#endif

#include "defines.h"

#if defined(_MSC_VER)
#  pragma warning (push)
#  pragma warning (disable :4505 4530)
#endif

// STL — keep sorted alphabetically when adding new entries
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <format>
#include <fstream>		// For udskrivning til fil
#include <immintrin.h>	// _pext_u64 for Magic.h — requires /arch:AVX2 (BMI2), see Magic.h
#include <functional>
#include <iostream>		// cout
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>		// std::jthread — Lazy SMP helper threads (AIPerplex::GetMove)
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#  pragma warning (pop)
#endif

//Allow using spdlog all over
#include <spdlog/spdlog.h>

using BitSpan = std::span<BITBOARD>;
