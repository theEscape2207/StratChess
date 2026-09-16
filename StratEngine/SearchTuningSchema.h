#pragma once
#include "SearchTuning.h"
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

	// Applies one UCI "setoption" to in_out. Names are case-sensitive; a value is lowercase true/false
	// or an unsigned decimal integer, surrounding whitespace allowed. A name the catalogue does not
	// expose, or exposes for a feature this build compiles out, is UnknownSetting. Replaces in_out
	// only when the value parses and the whole result validates.
	std::optional<TuningError> ParseUci(std::string_view name, std::string_view value, SearchTuning& in_out);

	// The complete "option name ..." line of every exposed, available field, in catalogue order.
	std::vector<std::string> UciOptionLines();

	// A Boolean feature the build compiled out may be set false but never true.
	std::optional<TuningError> CheckAvailable(const char* field, bool available, bool value);

} // namespace SearchTuningSchema
