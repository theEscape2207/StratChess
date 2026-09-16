#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "AIPerplex.h"
#include "SearchTuningSchema.h"
#include "defines.h"

#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using SearchTuningSchema::TuningError;
using Code = TuningError::Code;
using json = nlohmann::json;

namespace {

	// The hand-written SearchTuning the catalogue replaced. Search reads members directly, so the
	// generated struct must keep their types, order and layout.
	struct BaselineTuning {
		int64_t min_nodes_threshold;
		double min_completion_ratio;
		double min_pv_ratio;
		int score_draw_threshold;
		int delta_pruning_margin;
		int aspiration_initial_delta;
		int aspiration_max_retries;
		bool aspiration_enabled;
		int lmr_min_depth;
		int lmr_min_move_index;
		bool lmr_enabled;
		bool null_move_enabled;
		int null_move_reduction;
		int null_move_min_depth;
		bool see_pruning_enabled;
		bool singular_extensions_enabled;
		int singular_min_depth;
		int singular_tt_depth_margin;
		int singular_margin_factor;
		bool reverse_futility_enabled;
		int reverse_futility_max_depth;
		int reverse_futility_margin;
		bool reverse_futility_tt_refine_enabled;
		bool frontier_futility_enabled;
		int frontier_futility_margin;
		bool late_move_pruning_enabled;
	};

#define SAME_MEMBER(member)                                                                                            \
	static_assert(std::is_same_v<decltype(SearchTuning::member), decltype(BaselineTuning::member)>);                   \
	static_assert(offsetof(SearchTuning, member) == offsetof(BaselineTuning, member));
	SAME_MEMBER(min_nodes_threshold)
	SAME_MEMBER(min_completion_ratio)
	SAME_MEMBER(min_pv_ratio)
	SAME_MEMBER(score_draw_threshold)
	SAME_MEMBER(delta_pruning_margin)
	SAME_MEMBER(aspiration_initial_delta)
	SAME_MEMBER(aspiration_max_retries)
	SAME_MEMBER(aspiration_enabled)
	SAME_MEMBER(lmr_min_depth)
	SAME_MEMBER(lmr_min_move_index)
	SAME_MEMBER(lmr_enabled)
	SAME_MEMBER(null_move_enabled)
	SAME_MEMBER(null_move_reduction)
	SAME_MEMBER(null_move_min_depth)
	SAME_MEMBER(see_pruning_enabled)
	SAME_MEMBER(singular_extensions_enabled)
	SAME_MEMBER(singular_min_depth)
	SAME_MEMBER(singular_tt_depth_margin)
	SAME_MEMBER(singular_margin_factor)
	SAME_MEMBER(reverse_futility_enabled)
	SAME_MEMBER(reverse_futility_max_depth)
	SAME_MEMBER(reverse_futility_margin)
	SAME_MEMBER(reverse_futility_tt_refine_enabled)
	SAME_MEMBER(frontier_futility_enabled)
	SAME_MEMBER(frontier_futility_margin)
	SAME_MEMBER(late_move_pruning_enabled)
#undef SAME_MEMBER
	static_assert(sizeof(SearchTuning) == sizeof(BaselineTuning));
	static_assert(alignof(SearchTuning) == alignof(BaselineTuning));

	// Parses one key over the defaults and returns the error, if any.
	std::optional<TuningError> parse_one(const char* key, const json& value)
	{
		SearchTuning tuning;
		return SearchTuningSchema::ParseJson(json{{key, value}}, tuning);
	}

	bool accepts(const char* key, const json& value) { return !parse_one(key, value).has_value(); }

	Code rejection(const char* key, const json& value)
	{
		const auto error = parse_one(key, value);
		REQUIRE(error.has_value());
		CHECK(error->field == key);
		return error->code;
	}

} // namespace

TEST_CASE("SearchTuning defaults are the shipped values", "[tuning]")
{
	const SearchTuning tuning;
	CHECK(tuning.min_nodes_threshold == 1000);
	CHECK(tuning.min_completion_ratio == 0.10);
	CHECK(tuning.min_pv_ratio == 0.33);
	CHECK(tuning.score_draw_threshold == 20);
	CHECK(tuning.delta_pruning_margin == 200);
	CHECK(tuning.aspiration_initial_delta == 50);
	CHECK(tuning.aspiration_max_retries == 4);
	CHECK(tuning.aspiration_enabled);
	CHECK(tuning.lmr_min_depth == 3);
	CHECK(tuning.lmr_min_move_index == 3);
	CHECK(tuning.lmr_enabled);
	CHECK(tuning.null_move_enabled);
	CHECK(tuning.null_move_reduction == 3);
	CHECK(tuning.null_move_min_depth == 3);
	CHECK(tuning.see_pruning_enabled);
	// The test target compiles singular extensions in but leaves them off by default.
	CHECK_FALSE(tuning.singular_extensions_enabled);
	CHECK(tuning.singular_min_depth == 8);
	CHECK(tuning.singular_tt_depth_margin == 3);
	CHECK(tuning.singular_margin_factor == 2);
	CHECK(tuning.reverse_futility_enabled);
	CHECK(tuning.reverse_futility_max_depth == 3);
	CHECK(tuning.reverse_futility_margin == 100);
	CHECK(tuning.reverse_futility_tt_refine_enabled);
	CHECK(tuning.frontier_futility_enabled);
	CHECK(tuning.frontier_futility_margin == 200);
	CHECK(tuning.late_move_pruning_enabled);

	CHECK_FALSE(SearchTuningSchema::Validate(tuning).has_value());
}

TEST_CASE("SearchTuning JSON requires each field's own type", "[tuning]")
{
	CHECK(rejection("lmr_enabled", 1) == Code::InvalidType);
	CHECK(rejection("lmr_enabled", "true") == Code::InvalidType);
	CHECK(rejection("lmr_min_depth", 3.5) == Code::InvalidType);
	CHECK(rejection("lmr_min_depth", "3") == Code::InvalidType);
	CHECK(rejection("lmr_min_depth", true) == Code::InvalidType);
	CHECK(rejection("min_pv_ratio", "0.5") == Code::InvalidType);
	CHECK(accepts("min_pv_ratio", 1)); // an integer is a valid number for a ratio

	CHECK(rejection("score_draw_threshold", int64_t{INT_MAX} + 1) == Code::OutOfRange);
	CHECK(rejection("score_draw_threshold", int64_t{INT_MIN} - 1) == Code::OutOfRange);
	CHECK(rejection("min_nodes_threshold", std::numeric_limits<uint64_t>::max()) == Code::OutOfRange);
	CHECK(accepts("min_nodes_threshold", std::numeric_limits<int64_t>::max()));

	SearchTuning tuning;
	const auto error = SearchTuningSchema::ParseJson(json::array(), tuning);
	REQUIRE(error.has_value());
	CHECK(error->code == Code::InvalidType);
}

TEST_CASE("SearchTuning JSON ignores keys the catalogue does not bind", "[tuning]")
{
	SearchTuning tuning;
	REQUIRE_FALSE(SearchTuningSchema::ParseJson(json{{"no_such_key", 5}, {"singular_min_depth", 0}}, tuning));
	CHECK(tuning == SearchTuning{});
}

TEST_CASE("SearchTuning JSON rejection is atomic", "[tuning]")
{
	SearchTuning tuning;
	tuning.lmr_min_depth = 6;
	const SearchTuning before = tuning;

	const auto error = SearchTuningSchema::ParseJson(
	    json{{"lmr_enabled", false}, {"null_move_reduction", 4}, {"frontier_futility_margin", -1}}, tuning);

	REQUIRE(error.has_value());
	CHECK(error->field == "frontier_futility_margin");
	CHECK(tuning == before);
}

TEST_CASE("SearchTuning bounded fields accept their domain edges and reject beyond", "[tuning]")
{
	CHECK(accepts("delta_pruning_margin", 0));
	CHECK(accepts("delta_pruning_margin", 10000));
	CHECK(rejection("delta_pruning_margin", -1) == Code::OutOfRange);
	CHECK(rejection("delta_pruning_margin", 10001) == Code::OutOfRange);

	CHECK(accepts("min_pv_ratio", 0.0));
	CHECK(accepts("min_pv_ratio", 1.0));
	CHECK(rejection("min_pv_ratio", -0.01) == Code::OutOfRange);
	CHECK(rejection("min_pv_ratio", 1.01) == Code::OutOfRange);

	for (const char* depth_key : {"lmr_min_depth", "null_move_reduction", "null_move_min_depth"}) {
		INFO(depth_key);
		CHECK(accepts(depth_key, 1));
		CHECK(accepts(depth_key, INT_MAX));
		CHECK(rejection(depth_key, 0) == Code::OutOfRange);
	}

	CHECK(accepts("reverse_futility_max_depth", 1));
	CHECK(accepts("reverse_futility_max_depth", 4));
	CHECK(accepts("reverse_futility_max_depth", 8));
	CHECK(accepts("reverse_futility_max_depth", MAX_PLY));
	CHECK(rejection("reverse_futility_max_depth", 0) == Code::OutOfRange);
	CHECK(rejection("reverse_futility_max_depth", MAX_PLY + 1) == Code::OutOfRange);

	for (const char* margin_key : {"reverse_futility_margin", "frontier_futility_margin"}) {
		INFO(margin_key);
		CHECK(accepts(margin_key, 0));
		CHECK(accepts(margin_key, 1000));
		CHECK(rejection(margin_key, -1) == Code::OutOfRange);
		CHECK(rejection(margin_key, 1001) == Code::OutOfRange);
	}
}

TEST_CASE("SearchTuning comparison-only thresholds take any representable value", "[tuning]")
{
	CHECK(accepts("min_nodes_threshold", -1));
	CHECK(accepts("min_completion_ratio", -1.0));
	CHECK(accepts("min_completion_ratio", 2.5));
	CHECK(accepts("score_draw_threshold", -5));
	CHECK(accepts("score_draw_threshold", 20000));
	CHECK(accepts("lmr_min_move_index", -1));
	CHECK(accepts("lmr_min_move_index", INT_MAX));
}

TEST_CASE("SearchTuning validates unbound fields and non-finite ratios directly", "[tuning]")
{
	const auto code_of = [](SearchTuning tuning) {
		const auto error = SearchTuningSchema::Validate(tuning);
		REQUIRE(error.has_value());
		return error->code;
	};

	SearchTuning tuning;
	tuning.singular_min_depth = 0;
	CHECK(code_of(tuning) == Code::OutOfRange);

	tuning = {};
	tuning.singular_tt_depth_margin = -1;
	CHECK(code_of(tuning) == Code::OutOfRange);

	tuning = {};
	tuning.singular_margin_factor = 1001;
	CHECK(code_of(tuning) == Code::OutOfRange);

	tuning = {};
	tuning.singular_margin_factor = 1000;
	CHECK_FALSE(SearchTuningSchema::Validate(tuning).has_value());

	for (const double bad : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
		tuning = {};
		tuning.min_pv_ratio = bad;
		CHECK(code_of(tuning) == Code::OutOfRange);
		tuning = {};
		tuning.min_completion_ratio = bad;
		CHECK(code_of(tuning) == Code::OutOfRange);
	}
}

TEST_CASE("SearchTuning bounds the aspiration window jointly", "[tuning]")
{
	const auto aspiration = [](int64_t delta, int64_t retries) {
		SearchTuning tuning;
		return SearchTuningSchema::ParseJson(
		    json{{"aspiration_initial_delta", delta}, {"aspiration_max_retries", retries}}, tuning);
	};
	const int64_t limit = int64_t{INT_MAX} - GameValues::Search_Init;

	CHECK_FALSE(aspiration(limit, 0).has_value());
	CHECK_FALSE(aspiration(1, 30).has_value()); // 2^30 fits under the limit
	CHECK_FALSE(aspiration(limit / 2, 1).has_value());

	for (const auto& [delta, retries] :
	     {std::pair<int64_t, int64_t>{limit + 1, 0}, {limit, 1}, {1, 31}, {1, INT_MAX}}) {
		INFO(delta << " doubled " << retries << " times");
		const auto error = aspiration(delta, retries);
		REQUIRE(error.has_value());
		CHECK(error->code == Code::InvalidCombination);
	}

	CHECK(rejection("aspiration_initial_delta", 0) == Code::OutOfRange);
	CHECK(rejection("aspiration_max_retries", -1) == Code::OutOfRange);
}

TEST_CASE("SearchTuning keeps a compiled-out feature off", "[tuning]")
{
	using SearchTuningSchema::CheckAvailable;
	CHECK_FALSE(CheckAvailable("feature", false, false).has_value());
	CHECK_FALSE(CheckAvailable("feature", true, true).has_value());
	const auto error = CheckAvailable("feature", false, true);
	REQUIRE(error.has_value());
	CHECK(error->code == Code::Unavailable);
	CHECK(error->field == "feature");

	// The test target compiles singular extensions in, so enabling them is valid here.
	CHECK(accepts("singular_extensions_enabled", true));
}

TEST_CASE("AIPerplex rejects invalid tuning at construction", "[tuning][service_api]")
{
	SearchTuning tuning;
	tuning.min_pv_ratio = 2.0;
	REQUIRE_THROWS_AS(AIPerplex(AIPerplexConfig{.hash_mb = 1, .tuning = tuning}), std::invalid_argument);
	REQUIRE_THROWS_WITH(AIPerplex(AIPerplexConfig{.hash_mb = 1, .tuning = tuning}),
	                    Catch::Matchers::ContainsSubstring("min_pv_ratio"));
}

namespace {

	// Applies one UCI option over the defaults and returns the error, if any.
	std::optional<TuningError> parse_uci(std::string_view name, std::string_view value, SearchTuning& tuning)
	{
		tuning = SearchTuning{};
		return SearchTuningSchema::ParseUci(name, value, tuning);
	}

	Code uci_rejection(std::string_view name, std::string_view value)
	{
		SearchTuning tuning;
		const auto error = parse_uci(name, value, tuning);
		REQUIRE(error.has_value());
		CHECK(tuning == SearchTuning{});
		return error->code;
	}

} // namespace

TEST_CASE("SearchTuning UCI options set their own member", "[tuning][uci]")
{
	SearchTuning tuning;
	SearchTuning expected;

	REQUIRE_FALSE(parse_uci("ReverseFutility", "false", tuning));
	expected.reverse_futility_enabled = false;
	CHECK(tuning == expected);

	expected = SearchTuning{};
	REQUIRE_FALSE(parse_uci("ReverseFutilityMaxDepth", "8", tuning));
	expected.reverse_futility_max_depth = 8;
	CHECK(tuning == expected);

	expected = SearchTuning{};
	REQUIRE_FALSE(parse_uci("ReverseFutilityMargin", "150", tuning));
	expected.reverse_futility_margin = 150;
	CHECK(tuning == expected);

	expected = SearchTuning{};
	REQUIRE_FALSE(parse_uci("ReverseFutilityTtRefine", "false", tuning));
	expected.reverse_futility_tt_refine_enabled = false;
	CHECK(tuning == expected);

	expected = SearchTuning{};
	REQUIRE_FALSE(parse_uci("FrontierFutility", "false", tuning));
	expected.frontier_futility_enabled = false;
	CHECK(tuning == expected);

	expected = SearchTuning{};
	REQUIRE_FALSE(parse_uci("FrontierFutilityMargin", "250", tuning));
	expected.frontier_futility_margin = 250;
	CHECK(tuning == expected);

	expected = SearchTuning{};
	REQUIRE_FALSE(parse_uci("LateMovePruning", "false", tuning));
	expected.late_move_pruning_enabled = false;
	CHECK(tuning == expected);

	// The test target compiles singular extensions in, defaulting off.
	expected = SearchTuning{};
	REQUIRE_FALSE(parse_uci("SingularExtensions", "true", tuning));
	expected.singular_extensions_enabled = true;
	CHECK(tuning == expected);
}

TEST_CASE("SearchTuning UCI values are lowercase Booleans and unsigned decimals", "[tuning][uci]")
{
	SearchTuning tuning;
	REQUIRE_FALSE(parse_uci("ReverseFutility", " \tfalse ", tuning));
	CHECK_FALSE(tuning.reverse_futility_enabled);

	CHECK(uci_rejection("ReverseFutility", "False") == Code::InvalidType);
	CHECK(uci_rejection("ReverseFutility", "0") == Code::InvalidType);
	CHECK(uci_rejection("ReverseFutility", "") == Code::InvalidType);
	CHECK(uci_rejection("ReverseFutility", "false x") == Code::InvalidType);

	CHECK(uci_rejection("ReverseFutilityMargin", "") == Code::InvalidType);
	CHECK(uci_rejection("ReverseFutilityMargin", "-1") == Code::InvalidType);
	CHECK(uci_rejection("ReverseFutilityMargin", "+5") == Code::InvalidType);
	CHECK(uci_rejection("ReverseFutilityMargin", "12x") == Code::InvalidType);
	CHECK(uci_rejection("ReverseFutilityMargin", "1.5") == Code::InvalidType);
	CHECK(uci_rejection("ReverseFutilityMargin", "1 2") == Code::InvalidType);
	CHECK(uci_rejection("ReverseFutilityMargin", "99999999999999999999") == Code::OutOfRange);
	CHECK(uci_rejection("ReverseFutilityMargin", "1001") == Code::OutOfRange);
}

TEST_CASE("SearchTuning UCI depth band spans the engine's ply capacity", "[tuning][uci]")
{
	SearchTuning tuning;
	for (const int depth : {4, 8, MAX_PLY}) {
		REQUIRE_FALSE(parse_uci("ReverseFutilityMaxDepth", std::to_string(depth), tuning));
		CHECK(tuning.reverse_futility_max_depth == depth);
	}
	CHECK(uci_rejection("ReverseFutilityMaxDepth", "0") == Code::OutOfRange);
	CHECK(uci_rejection("ReverseFutilityMaxDepth", std::to_string(MAX_PLY + 1)) == Code::OutOfRange);
}

TEST_CASE("SearchTuning UCI ignores names it does not expose", "[tuning][uci]")
{
	CHECK(uci_rejection("reversefutility", "false") == Code::UnknownSetting);
	CHECK(uci_rejection("reverse_futility_enabled", "false") == Code::UnknownSetting);
	CHECK(uci_rejection("DeltaPruningMargin", "100") == Code::UnknownSetting);
	CHECK(uci_rejection("Hash", "16") == Code::UnknownSetting);
	CHECK(uci_rejection("", "") == Code::UnknownSetting);
}

TEST_CASE("SearchTuning UCI option lines", "[tuning][uci]")
{
	// The test target compiles singular extensions in, so it advertises all eight.
	const std::vector<std::string> expected{
	    "option name SingularExtensions type check default false",
	    "option name ReverseFutility type check default true",
	    "option name ReverseFutilityMaxDepth type spin default 3 min 1 max 256",
	    "option name ReverseFutilityMargin type spin default 100 min 0 max 1000",
	    "option name ReverseFutilityTtRefine type check default true",
	    "option name FrontierFutility type check default true",
	    "option name FrontierFutilityMargin type spin default 200 min 0 max 1000",
	    "option name LateMovePruning type check default true",
	};
	CHECK(SearchTuningSchema::UciOptionLines() == expected);
}
