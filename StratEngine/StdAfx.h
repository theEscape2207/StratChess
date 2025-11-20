#pragma once

#ifndef _WIN32_WINNT
# define _WIN32_WINNT 0x0600
#endif

#include "defines.h"

#pragma warning (push)
#pragma warning (disable :4505 4530)

#include <fstream>		// For udskrivning til fil
#include <iostream>		// cout

#include <span>

#include <vector>

#pragma warning (pop)

//Allow using spdlog all over
#include <spdlog/spdlog.h>

using BitSpan = std::span<BITBOARD>;
