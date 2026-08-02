cmake_minimum_required(VERSION 3.24)
project(StratChess CXX)

# Builds on Linux (GCC/Clang) and on Windows via clang-cl + Ninja, configured by
# the windows-clang-cl preset in CMakePresets.json.
#
# The .vcxproj/.sln build remains the day-to-day Visual Studio experience and is
# what still ships; retiring it is issue #81 Phase 3, and adopting the clang
# binary is issue #84.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # matches MSVC ConformanceMode=true

# Consumed by clangd, VS Code, Zed and coding agents. Costs nothing to emit.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependencies
#
# Pinned to the versions the Windows CI job already uses. Populated and consumed
# as include directories rather than via add_subdirectory: all three are
# header-only in this project, and Catch2 is used through its amalgamated
# distribution, so every existing #include line stays valid. Switching to real
# imported targets is issue #166.
# ---------------------------------------------------------------------------
include(FetchContent)

# SOURCE_SUBDIR points at a directory that does not exist in any of these repos.
# That makes FetchContent_MakeAvailable download and populate the sources but
# skip add_subdirectory(), because there is no CMakeLists.txt at that path -- so
# none of the dependencies' own CMake projects are configured, and no extra
# targets appear. This is the supported way to get populate-only behaviour:
# the bare FetchContent_Populate(name) form is deprecated as of CMake 3.30
# (policy CMP0169) and warns on the CMake that ubuntu-latest ships.
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog
    GIT_TAG        v1.16.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  do-not-configure)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  do-not-configure)

FetchContent_Declare(catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2
    GIT_TAG        v3.13.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  do-not-configure)

FetchContent_MakeAvailable(spdlog nlohmann_json catch2)

# ---------------------------------------------------------------------------
# Engine sources
#
# Globbed rather than listed: a hand-maintained list here would be a third copy
# to keep in sync with two .vcxproj files, and a .cpp missing from a project is
# a silent failure -- it passes by not running. CONFIGURE_DEPENDS re-runs the
# glob when the directory contents change.
#
# Archived/ holds retired algorithms kept for reference and must never be built,
# matching both .vcxproj files.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/StratEngine/*.cpp)
list(FILTER ENGINE_SOURCES EXCLUDE REGEX "/Archived/")

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/StratChessTests/*.cpp)

# ---------------------------------------------------------------------------
# Shared compile settings
#
# -mavx2 -mbmi2 matches AdvancedVectorExtensions2; Magic.h's _pext_u64 needs BMI2.
# -Werror matches the Level4 + /WX gate MSBuild has always had.
# ---------------------------------------------------------------------------
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)   # std::jthread — Lazy SMP helpers in AIPerplex::GetMove

function(strat_configure_target tgt)
    # Spelled identically by every driver here.
    target_compile_options(${tgt} PRIVATE -mavx2 -mbmi2)

    # Magic.h builds its 64 x 4096 sliding-attack tables at compile time, which
    # costs on the order of 10-26 million constexpr operations. Clang's default
    # budget (1,048,576 steps) is far below that and GCC's (33,554,432 ops) is
    # close enough to be unsafe, so both are raised explicitly rather than left
    # to the toolchain default. MSVC needs the same treatment and already has it:
    # the .vcxproj files pass /constexpr:steps100000000.
    #
    # Force-including Compat.h is how the SAL no-op macros and STRAT_FORCEINLINE
    # reach the 17 headers that never include StdAfx.h themselves, without editing
    # any of them. Deliberately not target_precompile_headers: CMake's generated
    # PCH makes GCC process defines.h twice per TU (it does not carry #pragma once
    # state across the PCH boundary), which is ~60 redefinition errors.
    #
    # clang-cl runs the same frontend as a Unix clang build but parses MSVC driver
    # syntax, and each GNU spelling fails differently there -- two of them
    # silently, which is why they are routed explicitly rather than left to chance:
    #   -Wall              is taken as MSVC /Wall, which clang maps to -Weverything,
    #                      burying the build in C++98-compat and padding noise
    #   -fconstexpr-steps= is accepted by the driver and dropped before it reaches
    #                      the frontend, so Magic.h blows the default budget
    #   -include           has no clang-cl equivalent; the header path is taken as
    #                      a second input file and every TU fails on /Fo
    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(${tgt} PRIVATE
            /clang:-Wall /clang:-Wextra /WX
            /clang:-fconstexpr-steps=100000000
            /FI${CMAKE_SOURCE_DIR}/StratEngine/Compat.h)
    else()
        target_compile_options(${tgt} PRIVATE
            -Wall -Wextra -Werror
            -include ${CMAKE_SOURCE_DIR}/StratEngine/Compat.h)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${tgt} PRIVATE -fconstexpr-steps=100000000)
        elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${tgt} PRIVATE -fconstexpr-ops-limit=100000000)
        endif()
    endif()

    target_include_directories(${tgt} PRIVATE
        ${CMAKE_SOURCE_DIR}/StratEngine
        ${spdlog_SOURCE_DIR}/include
        ${nlohmann_json_SOURCE_DIR}/include)
    target_link_libraries(${tgt} PRIVATE Threads::Threads)
endfunction()

# ---------------------------------------------------------------------------
# Targets
#
# The engine is compiled into both executables, mirroring the .vcxproj layout.
# Extracting it as a static library is issue #83: it changes link topology and
# owes its own nps measurement, so Phase 0 deliberately leaves topology alone.
#
# Release links the engine with LTO, matching the WholeProgramOptimization the
# .vcxproj Release configurations have always set -- without it a CMake-built
# binary is not comparable to the MSBuild one. The test binary is deliberately
# left out: it is checked for correctness, not speed, so LTO would only cost
# build time.
# ---------------------------------------------------------------------------
add_executable(StratChessEvolved
    ${ENGINE_SOURCES}
    ${CMAKE_SOURCE_DIR}/StratChessEvolved/StratChessEvolved.cpp)
strat_configure_target(StratChessEvolved)

include(CheckIPOSupported)
check_ipo_supported(RESULT STRAT_IPO_SUPPORTED OUTPUT STRAT_IPO_ERROR)
if(STRAT_IPO_SUPPORTED)
    set_property(TARGET StratChessEvolved
                 PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
else()
    message(WARNING "LTO unavailable; Release will not be optimised across "
                    "translation units: ${STRAT_IPO_ERROR}")
endif()

add_executable(StratChessTests
    ${ENGINE_SOURCES}
    ${TEST_SOURCES}
    ${catch2_SOURCE_DIR}/extras/catch_amalgamated.cpp)
strat_configure_target(StratChessTests)
target_compile_definitions(StratChessTests PRIVATE STRAT_ENABLE_TEST_ACCESS)
target_include_directories(StratChessTests PRIVATE ${catch2_SOURCE_DIR}/extras)
