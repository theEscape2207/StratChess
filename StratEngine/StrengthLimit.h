// StrengthLimit.h — the UCI_LimitStrength / UCI_Elo policy: Elo -> level, level -> pick depth, and the
// weakened root move pick.
//
// Pure functions, wired in by UciHandler rather than the search service: the search runs unchanged at
// the MultiPV and depth limit the handler gives it, and the pick reads the ranked SearchResult. So the
// search-equivalence gate rests on no new search branch, and game/tactical modes, which never limit
// strength, carry nothing.
//
// The pick takes a seed rather than a generator. UciHandler draws one seed per search on the
// controlling thread and captures it by value in the completion handler, so no mutable state
// crosses threads and a test fixes the pick by fixing the seed.
#pragma once

#include <cstdint>
#include <span>

namespace StrengthLimit {
	// The advertised UCI_Elo range; the default is kMinElo. Provisional until calibrated (see the table
	// in StrengthLimit.cpp).
	inline constexpr int kMinElo = 1320;
	inline constexpr int kMaxElo = 2600;
	inline constexpr double kMaxLevel = 19.0;

	// The ranked set the pick chooses from while limiting: MultiPV is raised to at least this.
	inline constexpr unsigned kMinMultiPV = 4;

	// Piecewise-linear over the anchor table, clamped to [0, kMaxLevel]; non-decreasing in elo.
	[[nodiscard]] double level_for_elo(int elo) noexcept;

	// 1 + int(level): the depth the pick is made at. The fractional level enters only the pick.
	[[nodiscard]] int pick_depth(double level) noexcept;

	// The index of the move to play in a ranked set's scores (rank 1 first, best first, non-empty).
	[[nodiscard]] size_t pick(std::span<const int> scores, double level, uint64_t seed) noexcept;
} // namespace StrengthLimit
