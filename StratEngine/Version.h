// Version.h — the engine's version identity, as UCI 'id name' reports it.
#pragma once
#include <string_view>

// "X.Y.Z" in a STRAT_RELEASE build; otherwise "X.Y.Z+<short commit hash>", or "X.Y.Z+unknown"
// when Git could not name the commit. Defined in the build tree's generated/Version.cpp
// (cmake/Version.cpp.in), the only source a new commit recompiles.
std::string_view EngineVersion();
