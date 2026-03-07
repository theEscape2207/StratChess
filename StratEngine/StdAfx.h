#pragma once

#ifndef _WIN32_WINNT
# define _WIN32_WINNT 0x0600
#endif

#include "defines.h"

#pragma warning (push)
#pragma warning (disable :4505 4530)

// STL — keep sorted alphabetically when adding new entries
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <format>
#include <fstream>		// For udskrivning til fil
#include <functional>
#include <iostream>		// cout
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#pragma warning (pop)

//Allow using spdlog all over
#include <spdlog/spdlog.h>

using BitSpan = std::span<BITBOARD>;
