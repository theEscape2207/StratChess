#include "Common.h"
#include "StrengthLimit.h"
#include "defines.h"

namespace StrengthLimit {
	namespace {
		struct Anchor {
			int elo;
			double level;
		};

		// Provisional: kMaxElo is the uncapped engine's scouting estimate (#985) and 1320 is Stockfish 19's
		// UCI_Elo floor. The calibration slice replaces the anchors and both range ends and changes nothing
		// else. It reaches level L by setting UCI_Elo to ceil(inverse(L)), so the table must stay strictly
		// increasing in both columns.
		constexpr std::array kAnchors{Anchor{kMinElo, 0.0}, Anchor{kMaxElo, kMaxLevel}};

		constexpr bool strictly_increasing()
		{
			for (size_t i = 1; i < kAnchors.size(); ++i) {
				if (kAnchors[i].elo <= kAnchors[i - 1].elo || kAnchors[i].level <= kAnchors[i - 1].level)
					return false;
			}
			return true;
		}
		static_assert(strictly_increasing());
		static_assert(kAnchors.front().elo == kMinElo && kAnchors.back().elo == kMaxElo);
		static_assert(kAnchors.front().level == 0.0 && kAnchors.back().level == kMaxLevel);
	} // namespace

	double level_for_elo(int elo) noexcept
	{
		if (elo <= kAnchors.front().elo)
			return kAnchors.front().level;
		for (size_t i = 1; i < kAnchors.size(); ++i) {
			const Anchor& lo = kAnchors[i - 1];
			const Anchor& hi = kAnchors[i];
			if (elo <= hi.elo)
				return lo.level + (hi.level - lo.level) * (elo - lo.elo) / (hi.elo - lo.elo);
		}
		return kAnchors.back().level;
	}

	int pick_depth(double level) noexcept { return 1 + static_cast<int>(level); }

	// Stockfish's Skill::pick_best in our centipawns. Each candidate scores
	//   adjusted_i = score_i + int(weakness * (top - score_i) + delta * r_i) / 128
	// and the highest wins, the later rank on ties. The first term compresses gaps rather than
	// penalising them: a move's adjusted gap is (128 - weakness) / 128 of its real one, so a candidate
	// can win only when gap <= delta * (int(weakness) - 1) / (128 - weakness): 1487 cp at level 0 and
	// 176 cp at level 19 with delta at its one-pawn cap.
	//
	// Mate guard, a departure from Stockfish: the bound is on gaps, and mate scores sit close together,
	// so the formula could trade a mate for a slower one each move until a fifty-move or repetition draw.
	// A mate-scored rank 1 is played as found (a proven mate, or the slowest proven loss); otherwise
	// losing-mate ranks are dropped first, so they cannot pin delta at its cap.
	size_t pick(std::span<const int> scores, double level, uint64_t seed) noexcept
	{
		assert(!scores.empty());
		assert(std::is_sorted(scores.begin(), scores.end(), std::greater<>{}));
		const int top = scores.front();
		if (std::abs(top) >= GameValues::Mate_Threshold)
			return 0;
		size_t n = scores.size();
		while (n > 1 && scores[n - 1] <= -GameValues::Mate_Threshold)
			--n;
		if (n == 1)
			return 0;

		const double weakness = 120.0 - 2.0 * std::clamp(level, 0.0, kMaxLevel);
		const int delta = std::min(top - scores[n - 1], g_iPieceValues[PAWN >> 1]);
		// Uniform in [0, int(weakness)); Stockfish's modulo carries a negligible bias this does not.
		std::mt19937_64 rng(seed);
		std::uniform_int_distribution<int> draw(0, static_cast<int>(weakness) - 1);

		size_t chosen = 0;
		int best = std::numeric_limits<int>::min();
		for (size_t i = 0; i < n; ++i) {
			const double push = weakness * (top - scores[i]) + delta * draw(rng);
			const int adjusted = scores[i] + static_cast<int>(push) / 128;
			if (adjusted >= best) {
				best = adjusted;
				chosen = i;
			}
		}
		return chosen;
	}
} // namespace StrengthLimit
