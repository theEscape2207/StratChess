# Fails the build unless the generated clang-cl C++ compile rules still carry the MSVC
# /showIncludes dependency path.
#
# The CMakeLists override that selects that path drives internals CMake does not document:
# variable names, rule generation and defaults can all change in a toolchain upgrade, and a
# silent return to GCC-style depfiles is invisible -- it reintroduces objects that a cache
# hit leaves with no dependency record, so a later header edit never rebuilds them. Asserting
# the generated rule rather than the CMake variables is what makes that detectable, and
# living in the build graph is what makes `ninja` and `cmake --build` see it too.
#
# Invoked as:
#   cmake -D RULES_FILE=<build>/CMakeFiles/rules.ninja
#         -D RULE_TARGETS=<comma-separated target names>
#         -P CheckNinjaDepsRules.cmake

if(NOT RULES_FILE OR NOT EXISTS "${RULES_FILE}")
    message(FATAL_ERROR "Dependency rule check: '${RULES_FILE}' does not exist.")
endif()

file(READ "${RULES_FILE}" strat_rules_text)
string(REPLACE ";" "\\;" strat_rules_text "${strat_rules_text}")
string(REPLACE "\n" ";" strat_rules_lines "${strat_rules_text}")
string(REPLACE "," ";" strat_targets "${RULE_TARGETS}")

# rules.ninja is a flat list of blocks: a `rule <name>` line followed by its indented
# bindings. Collect the bindings of every C++ compile rule belonging to our targets;
# CMake names them CXX_COMPILER__<target>[_unscanned]_<config>.
set(strat_rule_name "")
set(strat_seen "")
foreach(line IN LISTS strat_rules_lines)
    if(line MATCHES "^rule (.+)$")
        # Held in its own variable: the inner if() clobbers CMAKE_MATCH_1 on its first miss.
        set(strat_this_rule "${CMAKE_MATCH_1}")
        set(strat_rule_name "")
        foreach(tgt IN LISTS strat_targets)
            if(strat_this_rule MATCHES "^CXX_COMPILER__${tgt}_")
                set(strat_rule_name "${strat_this_rule}")
                list(APPEND strat_seen "${tgt}")
                set(strat_block_${strat_rule_name} "")
            endif()
        endforeach()
    elseif(strat_rule_name AND line MATCHES "^  ")
        string(APPEND strat_block_${strat_rule_name} "${line}\n")
    elseif(line MATCHES "^[^ ]")
        set(strat_rule_name "")
    endif()
endforeach()

set(strat_failures "")

foreach(tgt IN LISTS strat_targets)
    if(NOT tgt IN_LIST strat_seen)
        list(APPEND strat_failures "no CXX compile rule found for target '${tgt}'")
    endif()
endforeach()

get_cmake_property(strat_vars VARIABLES)
foreach(var IN LISTS strat_vars)
    if(NOT var MATCHES "^strat_block_(.+)$")
        continue()
    endif()
    set(rule "${CMAKE_MATCH_1}")
    set(block "${${var}}")
    if(NOT block MATCHES "(^|\n)  deps = msvc(\n|$)")
        list(APPEND strat_failures "${rule}: expected 'deps = msvc'")
    endif()
    if(block MATCHES "(^|\n)  depfile = ")
        list(APPEND strat_failures "${rule}: has a 'depfile =' binding, which the msvc path must not use")
    endif()
    if(NOT block MATCHES "(^|\n)  command = [^\n]*/showIncludes")
        list(APPEND strat_failures "${rule}: '/showIncludes' missing from the compile command")
    endif()
endforeach()

if(strat_failures)
    string(REPLACE ";" "\n  - " strat_report "${strat_failures}")
    message(FATAL_ERROR
        "The clang-cl C++ compile rules no longer use the /showIncludes dependency path:\n"
        "  - ${strat_report}\n"
        "Under a compiler cache this means objects can be replayed with no dependency record, "
        "so a later header edit will not rebuild them. Fix the depfile override in CMakeLists.txt "
        "-- or, if the compiler cache now records dependencies for GCC-style depfiles, remove the "
        "override and this check together.")
endif()
