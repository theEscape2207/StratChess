#include "StdAfx.h"
#include "SearchTuningSchema.h"
#include "SearchTelemetry.h"
#include <climits>
#include <limits>
#include <nlohmann/json.hpp>
#include <type_traits>

namespace SearchTuningSchema {
	namespace {

		using Code = TuningError::Code;

		constexpr bool exposed_over_uci(const char* uci_name) { return uci_name != nullptr; }

// Compile-time catalogue checks: every default lies in its domain, only a Boolean may be
// unavailable and it must then default off, and UCI encodes only check and spin options.
#define TUNING_FIELD(type, member, default_value, lo, hi, json, uci_name, available)                                   \
	static_assert(static_cast<type>(lo) <= SearchTuning{}.member && SearchTuning{}.member <= static_cast<type>(hi),    \
	              "SearchTuning.def: default of " #member " is outside its domain");                                   \
	static_assert((available) || (std::is_same_v<type, bool> && !SearchTuning{}.member),                               \
	              "SearchTuning.def: only a Boolean defaulting off may be unavailable: " #member);                     \
	static_assert(!exposed_over_uci(uci_name) || std::is_same_v<type, bool> || std::is_same_v<type, int>,              \
	              "SearchTuning.def: UCI exposes only bool and int fields: " #member);
#include "SearchTuning.def"
#undef TUNING_FIELD

		constexpr const char* kUciNames[] = {
#define TUNING_FIELD(type, member, default_value, lo, hi, json, uci_name, available) uci_name,
#include "SearchTuning.def"
#undef TUNING_FIELD
		};

		constexpr bool uci_names_unique()
		{
			for (size_t i = 0; i < std::size(kUciNames); ++i)
				for (size_t j = i + 1; j < std::size(kUciNames); ++j)
					if (kUciNames[i] && kUciNames[j] &&
					    std::string_view(kUciNames[i]) == std::string_view(kUciNames[j]))
						return false;
			return true;
		}
		static_assert(uci_names_unique(), "SearchTuning.def: duplicate UCI option name");

		std::string describe(bool value) { return value ? "true" : "false"; }
		template <typename T> std::string describe(T value) { return std::format("{}", value); }

		template <typename T> std::optional<TuningError> check_domain(const char* field, T value, T lo, T hi)
		{
			// Written so that a NaN fails both comparisons.
			if (lo <= value && value <= hi)
				return std::nullopt;
			return TuningError{Code::OutOfRange, field,
			                   std::format("{} is outside [{}, {}]", describe(value), describe(lo), describe(hi))};
		}

		std::optional<TuningError> read_json(const nlohmann::json& block, const char* key, bool& out)
		{
			const auto it = block.find(key);
			if (it == block.end())
				return std::nullopt;
			if (!it->is_boolean())
				return TuningError{Code::InvalidType, key, "expected true or false"};
			out = it->get<bool>();
			return std::nullopt;
		}

		std::optional<TuningError> read_json(const nlohmann::json& block, const char* key, double& out)
		{
			const auto it = block.find(key);
			if (it == block.end())
				return std::nullopt;
			if (!it->is_number())
				return TuningError{Code::InvalidType, key, "expected a number"};
			out = it->get<double>();
			return std::nullopt;
		}

		template <typename T>
		    requires std::is_integral_v<T> && std::is_signed_v<T>
		std::optional<TuningError> read_json(const nlohmann::json& block, const char* key, T& out)
		{
			const auto it = block.find(key);
			if (it == block.end())
				return std::nullopt;
			if (!it->is_number_integer())
				return TuningError{Code::InvalidType, key, "expected an integer"};

			const auto not_representable = [&] {
				return TuningError{Code::OutOfRange, key, std::format("{} does not fit the field's type", it->dump())};
			};
			if (it->is_number_unsigned()) {
				const auto value = it->get<uint64_t>();
				if (value > static_cast<uint64_t>(std::numeric_limits<T>::max()))
					return not_representable();
				out = static_cast<T>(value);
				return std::nullopt;
			}
			const auto value = it->get<int64_t>();
			if (value < std::numeric_limits<T>::lowest() || value > std::numeric_limits<T>::max())
				return not_representable();
			out = static_cast<T>(value);
			return std::nullopt;
		}

		// search_with_aspiration() doubles the delta once per retry and adds it to a seed score bounded by
		// Search_Init, so the widest window must stay representable.
		std::optional<TuningError> check_aspiration(const SearchTuning& tuning)
		{
			const int64_t limit = static_cast<int64_t>(INT_MAX) - GameValues::Search_Init;
			int64_t delta = tuning.aspiration_initial_delta;
			// The delta is at least one, so this loop runs at most 31 doublings before exceeding the limit.
			for (int retry = 0; delta <= limit && retry < tuning.aspiration_max_retries; ++retry)
				delta *= 2;
			if (delta <= limit)
				return std::nullopt;
			return TuningError{
			    Code::InvalidCombination, "aspiration_initial_delta",
			    std::format("aspiration_initial_delta {} doubled over aspiration_max_retries {} exceeds {}",
			                tuning.aspiration_initial_delta, tuning.aspiration_max_retries, limit)};
		}

	} // namespace

	std::optional<TuningError> CheckAvailable(const char* field, bool available, bool value)
	{
		if (available || !value)
			return std::nullopt;
		return TuningError{Code::Unavailable, field, "this build compiles the feature out"};
	}

	std::optional<TuningError> Validate(const SearchTuning& tuning)
	{
#define TUNING_FIELD(type, member, default_value, lo, hi, json, uci_name, available)                                   \
	if (auto error = check_domain<type>(#member, tuning.member, static_cast<type>(lo), static_cast<type>(hi)))         \
		return error;                                                                                                  \
	if (auto error = CheckAvailable(#member, available, tuning.member != static_cast<type>(0)))                        \
		return error;
#include "SearchTuning.def"
#undef TUNING_FIELD

		return check_aspiration(tuning);
	}

	std::optional<TuningError> ParseJson(const nlohmann::json& block, SearchTuning& in_out)
	{
		if (!block.is_object())
			return TuningError{Code::InvalidType, "search_tuning", "expected an object"};

		SearchTuning candidate = in_out;
#define TUNING_FIELD(type, member, default_value, lo, hi, json, uci_name, available)                                   \
	if constexpr (json) {                                                                                              \
		if (auto error = read_json(block, #member, candidate.member))                                                  \
			return error;                                                                                              \
	}
#include "SearchTuning.def"
#undef TUNING_FIELD

		if (auto error = Validate(candidate))
			return error;
		in_out = candidate;
		return std::nullopt;
	}

} // namespace SearchTuningSchema
