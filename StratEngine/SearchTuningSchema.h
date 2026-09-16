#pragma once
#include "SearchTuning.h"
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>

// Cold validation and parsing for SearchTuning, generated from SearchTuning.def. Nothing here runs
// during a search.
namespace SearchTuningSchema {

	struct TuningError {
		enum class Code : uint8_t { UnknownSetting, InvalidType, OutOfRange, Unavailable, InvalidCombination };

		Code code;
		std::string field;
		std::string message;
	};

	// Checks every field's domain and availability, then the cross-field constraints.
	std::optional<TuningError> Validate(const SearchTuning& tuning);

	// Reads a game_settings.json "search_tuning" object over in_out. Keys the catalogue does not bind
	// are ignored. Replaces in_out only when every bound key has the right type and the whole result
	// validates; an omitted key keeps in_out's value.
	std::optional<TuningError> ParseJson(const nlohmann::json& block, SearchTuning& in_out);

	// A Boolean feature the build compiled out may be set false but never true.
	std::optional<TuningError> CheckAvailable(const char* field, bool available, bool value);

} // namespace SearchTuningSchema
