#pragma once
#include <cstdint>

// Search tuning shared by the service configuration and the search implementation. Every field,
// its default, domain and bindings are declared in SearchTuning.def; validation and parsing live
// in SearchTuningSchema.h, so this header stays free of JSON.
struct SearchTuning {
#define TUNING_FIELD(type, member, default_value, lo, hi, json, uci_name, available) type member = default_value;
#include "SearchTuning.def"
#undef TUNING_FIELD

	friend bool operator==(const SearchTuning&, const SearchTuning&) = default;
};
