# Renders the engine's version TU with the commit hash read at BUILD time, not configure time.
#
# Ninja re-runs CMake only when CMakeLists.txt or a glob changes, so a configure-time hash would
# keep naming the commit a tree was configured at across any number of later commits -- a wrong
# identity, the failure this exists to remove. Watching .git/HEAD does not help: in a worktree
# .git is a file, and a commit moves the branch ref or packed-refs, not HEAD.
#
# The file is rewritten only when its content changes. The custom target declares it as a
# BYPRODUCT, so Ninja restats it and an unchanged hash recompiles nothing. No timestamp enters
# it: two builds of one commit stay byte-identical (Test-ReleaseReproducibility.ps1).
#
# Git missing or refusing (a source archive, an unsafe-directory refusal) gives "unknown".
# Found here rather than passed in, so a configure-time Git path never pins a stale answer.
#
# Invoked as:
#   cmake -D SOURCE_DIR=<repo> -D VERSION=<X.Y.Z> -D TEMPLATE=<Version.cpp.in>
#         -D OUTPUT=<generated/Version.cpp> -P GenerateVersion.cmake

foreach(strat_arg IN ITEMS SOURCE_DIR VERSION TEMPLATE OUTPUT)
    if(NOT DEFINED ${strat_arg})
        message(FATAL_ERROR "GenerateVersion.cmake: -D ${strat_arg}=... is required.")
    endif()
endforeach()

set(strat_hash "unknown")
find_program(strat_git git NO_CACHE)
if(strat_git)
    execute_process(
        COMMAND "${strat_git}" -C "${SOURCE_DIR}" rev-parse --short=8 HEAD
        RESULT_VARIABLE strat_rc
        OUTPUT_VARIABLE strat_out
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(strat_rc EQUAL 0 AND strat_out MATCHES "^[0-9a-f]+$")
        set(strat_hash "${strat_out}")
    endif()
endif()

set(STRAT_VERSION_STRING "${VERSION}+${strat_hash}")
configure_file("${TEMPLATE}" "${OUTPUT}.tmp" @ONLY)
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
