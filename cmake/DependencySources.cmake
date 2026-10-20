# Shared dependency sources: one checkout per dependency and tag, used by every build tree on
# the machine, while each tree compiles its own copy into its own _deps (#860).
#
# strat_declare_dependency(<name> GIT_REPOSITORY <url> GIT_TAG <tag> [FetchContent args...])
#
# With STRAT_DEPS_SOURCE_CACHE set, <name> comes from <cache>/_deps/<name>-<tag>: cloned there on
# first use, then handed to FetchContent as FETCHCONTENT_SOURCE_DIR_<NAME>, which skips population
# entirely. Empty, it is a plain FetchContent_Declare into the tree's own _deps.
#
# - Keyed by tag, so a pin bump selects a new directory and no checkout is ever updated in place.
#   Old tags are inert; delete them by hand.
# - The _deps segment is load-bearing: the vendored-code filters in .clang-tidy, .clang-tidy-deep
#   and Get-Coverage.ps1 match on it.
# - A directory exists only after a complete clone: the clone goes to <dir>.tmp and is renamed in
#   place. file(LOCK) serializes concurrent configures, and only the lock holder writes the .tmp,
#   so a leftover one is from a killed configure and is removed before cloning again.
#
# The variable's first value comes from the environment, which is how build.ps1 and CI hand it to
# every configure, including the ones Ninja re-runs by itself. After that the cached value wins.

if(NOT DEFINED STRAT_DEPS_SOURCE_CACHE AND DEFINED ENV{STRAT_DEPS_SOURCE_CACHE})
    file(TO_CMAKE_PATH "$ENV{STRAT_DEPS_SOURCE_CACHE}" strat_deps_source_cache_default)
else()
    set(strat_deps_source_cache_default "")
endif()
set(STRAT_DEPS_SOURCE_CACHE "${strat_deps_source_cache_default}" CACHE PATH
    "Shared dependency checkouts, one per <name>-<tag>. Empty: fetch into this tree's _deps.")

function(strat_declare_dependency name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "GIT_REPOSITORY;GIT_TAG" "")
    if(NOT arg_GIT_REPOSITORY OR NOT arg_GIT_TAG)
        message(FATAL_ERROR "strat_declare_dependency(${name}) needs GIT_REPOSITORY and GIT_TAG.")
    endif()

    if(STRAT_DEPS_SOURCE_CACHE)
        set(root "${STRAT_DEPS_SOURCE_CACHE}/_deps")
        set(dir "${root}/${name}-${arg_GIT_TAG}")
        if(NOT IS_DIRECTORY "${dir}")
            file(MAKE_DIRECTORY "${root}")
            file(LOCK "${dir}.lock" GUARD FUNCTION TIMEOUT 600)
            if(NOT IS_DIRECTORY "${dir}")
                strat_clone_dependency("${name}" "${arg_GIT_REPOSITORY}" "${arg_GIT_TAG}" "${dir}")
            endif()
        endif()
        string(TOUPPER "${name}" upper)
        set(FETCHCONTENT_SOURCE_DIR_${upper} "${dir}" PARENT_SCOPE)
    endif()

    FetchContent_Declare(${name} ${ARGN})
endfunction()

# Called only under the lock.
function(strat_clone_dependency name repository tag dir)
    find_package(Git REQUIRED QUIET)
    set(tmp "${dir}.tmp")
    file(REMOVE_RECURSE "${tmp}")

    message(STATUS "Dependency sources: cloning ${name} ${tag} into ${dir}")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -c advice.detachedHead=false
                clone --quiet --depth 1 --branch "${tag}" "${repository}" "${tmp}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE out)
    if(NOT rc EQUAL 0)
        file(REMOVE_RECURSE "${tmp}")
        message(FATAL_ERROR "Cloning ${name} ${tag} from ${repository} failed (${rc}):\n${out}")
    endif()

    # No NO_REPLACE: on Linux it fails for any directory ("Operation not permitted"). A plain
    # rename already refuses a non-empty destination on Windows and Linux, and the lock and
    # re-check leave none.
    file(RENAME "${tmp}" "${dir}" RESULT renamed)
    if(NOT renamed EQUAL 0)
        file(REMOVE_RECURSE "${tmp}")
        message(FATAL_ERROR "Publishing ${dir} failed: ${renamed}")
    endif()
endfunction()
