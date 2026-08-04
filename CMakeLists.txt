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

# ---------------------------------------------------------------------------
# Sanitizers
#
# Empty by default, and nothing below reaches a target while it is empty -- an
# ordinary build produces the same command lines and the same binary as if this
# section did not exist.
#
# Linux only, and refused rather than ignored elsewhere. The GNU -fsanitize=
# spelling does not survive the MSVC driver, and clang-cl is the compiler that
# accepts flags and drops them before the frontend sees them (#84), so a Windows
# build that quietly declined to instrument itself would look exactly like one
# that worked.
# ---------------------------------------------------------------------------
set(STRAT_SANITIZE "" CACHE STRING
    "Sanitizers to build with, passed through to -fsanitize= (e.g. address,undefined). Empty disables.")

# Sanitizers report and continue by default, which exits 0 -- a CI job would go
# green with findings on screen. OFF turns any finding into a failure. ON is for
# surveying an uninstrumented codebase for the first time, where stopping at the
# first report means one rebuild per finding.
option(STRAT_SANITIZE_RECOVER
       "Let sanitizers report and continue rather than abort (survey mode; CI must leave this OFF)" OFF)

if(STRAT_SANITIZE AND (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC"
                       OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    message(FATAL_ERROR
            "STRAT_SANITIZE is not supported on MSVC or clang-cl. Build on Linux -- "
            "the sanitize-linux job in .github/workflows/build-and-test.yml is the "
            "reference invocation.")
endif()

# libstdc++ debug mode: checked iterators and container preconditions, which is
# what MSVC gets from _ITERATOR_DEBUG_LEVEL=2 and _GLIBCXX_ASSERTIONS (bounds
# only) does not cover.
#
# It changes the ABI of the standard containers, so every translation unit in the
# link must agree. That holds here because all three dependencies are
# populate-only FetchContent sources compiled into our own targets with these
# flags -- do not switch them to prebuilt imported targets without revisiting it
# (#166).
option(STRAT_STDLIB_DEBUG
       "Build against libstdc++ debug mode (_GLIBCXX_DEBUG). GCC/libstdc++ only." OFF)

if(STRAT_STDLIB_DEBUG AND (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC"
                           OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    message(FATAL_ERROR
            "STRAT_STDLIB_DEBUG is libstdc++-only and does nothing against the MSVC "
            "standard library, which enables the equivalent checks through "
            "_ITERATOR_DEBUG_LEVEL in Debug builds already.")
endif()

function(strat_configure_target tgt)
    # Magic.h builds its 64 x 4096 sliding-attack tables at compile time, which
    # costs on the order of 10-26 million constexpr operations -- past every
    # compiler's default budget, so each one has to be raised explicitly.
    #
    # Force-including Compat.h is how the SAL no-op macros and STRAT_FORCEINLINE
    # reach the 17 headers that never include StdAfx.h themselves, without editing
    # any of them. Deliberately not target_precompile_headers: CMake's generated
    # PCH makes GCC process defines.h twice per TU (it does not carry #pragma once
    # state across the PCH boundary), which is ~60 redefinition errors.
    #
    # Three drivers, three spellings, and the wrong one is rarely a hard error:
    #
    #   clang-cl runs the same frontend as a Unix clang build but parses MSVC
    #   driver syntax. -Wall is taken as MSVC /Wall, which clang maps to
    #   -Weverything; -fconstexpr-steps= is accepted by the driver and dropped
    #   before it reaches the frontend; -include has no equivalent at all and its
    #   argument becomes a second input file, failing every TU on /Fo.
    #
    #   MSVC understands none of the GNU spellings and reports each as a D9002
    #   warning before carrying on, so -mavx2/-mbmi2 would go missing with the
    #   build still succeeding -- the reason the nps gate, not just a clean
    #   compile, is what qualifies a change to this function.
    set(compat_header ${CMAKE_SOURCE_DIR}/StratEngine/Compat.h)

    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        # /arch:AVX2 also enables BMI2, which is what Magic.h's _pext_u64 needs.
        target_compile_options(${tgt} PRIVATE
            /W4 /WX /utf-8 /permissive-
            /arch:AVX2
            /constexpr:steps100000000
            /FI${compat_header})
    elseif(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(${tgt} PRIVATE
            -mavx2 -mbmi2
            /clang:-Wall /clang:-Wextra /WX /utf-8
            /clang:-fconstexpr-steps=100000000
            /FI${compat_header})
    else()
        target_compile_options(${tgt} PRIVATE
            -mavx2 -mbmi2
            -Wall -Wextra -Werror
            -include ${compat_header})
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${tgt} PRIVATE -fconstexpr-steps=100000000)
        elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${tgt} PRIVATE -fconstexpr-ops-limit=100000000)
        endif()
    endif()

    if(STRAT_STDLIB_DEBUG)
        target_compile_definitions(${tgt} PRIVATE _GLIBCXX_DEBUG _GLIBCXX_DEBUG_PEDANTIC)
    endif()

    # Outside the chain above because the MSVC and clang-cl branches are already
    # excluded by the FATAL_ERROR guard, so this only ever runs for GCC or a Unix
    # clang. The same flags go on the compile and the link line: -fsanitize=
    # selects the runtime libraries the link needs, not just the instrumentation.
    if(STRAT_SANITIZE)
        set(san_flags -fsanitize=${STRAT_SANITIZE} -fno-omit-frame-pointer)
        if(NOT STRAT_SANITIZE_RECOVER)
            list(APPEND san_flags -fno-sanitize-recover=all)
        endif()
        target_compile_options(${tgt} PRIVATE ${san_flags})
        target_link_options(${tgt} PRIVATE ${san_flags})

        # ASan sees a container's heap block, not the elements inside it, so an
        # out-of-range std::vector::operator[] stays within an allocation it knows
        # about and is never reported. libstdc++'s own bounds assertions are what
        # catch that class of error.
        target_compile_definitions(${tgt} PRIVATE _GLIBCXX_ASSERTIONS)
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
