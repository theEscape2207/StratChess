#pragma once
#include <cstdint>

// Whether a build that HAS singular extensions also starts with it on. Deliberately separate from
// compiling it in, because the two targets want opposite answers:
//   - the experimental engine defines both, so a harness measuring it gets the feature by default
//     and turns it off with UCI's SingularExtensions option;
//   - the test binary defines only the first, so every existing search test keeps exercising the
//     SHIPPED configuration; the singular tests turn it on for themselves.
#ifndef STRAT_SINGULAR_DEFAULT_ON
#	define STRAT_SINGULAR_DEFAULT_ON 0
#endif

// Search tuning shared by the service configuration and the search implementation. Every field,
// its default, domain and bindings are declared in SearchTuning.def; validation and parsing live
// in SearchTuningSchema.h, so this header stays free of JSON.
struct SearchTuning {
#define TUNING_FIELD(type, member, default_value, lo, hi, json, uci_name, available) type member = default_value;
#include "SearchTuning.def"
#undef TUNING_FIELD

	friend bool operator==(const SearchTuning&, const SearchTuning&) = default;
};
