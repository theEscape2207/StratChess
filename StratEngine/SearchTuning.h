#pragma once
#include <cstdint>

// Search tuning shared by the service configuration and the search implementation. Every field,
// its default, domain and bindings are declared in SearchTuning.def; validation and parsing live
// in SearchTuningSchema.h, so this header stays free of JSON.
struct SearchTuning {
	// Keep the catalogue grouped by feature; emit numeric members before flags to avoid padding.
#define TUNING_FIELD(type, member, default_value, ...) type member = default_value;
#define TUNING_FLAG(...)
#include "SearchTuning.def"
#undef TUNING_FIELD
#undef TUNING_FLAG

#define TUNING_FIELD(...)
#define TUNING_FLAG(member, default_value, ...) bool member = default_value;
#include "SearchTuning.def"
#undef TUNING_FIELD
#undef TUNING_FLAG

	friend bool operator==(const SearchTuning&, const SearchTuning&) = default;
};
