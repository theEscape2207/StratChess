// EndgameConversionTests.cpp — can the engine actually finish a won basic mate?
//
// Every other endgame test asks what a term scores. This one plays five
// K+B+N vs K starts out with the production search on both sides and asks for the
// mate, which is the only instrument that can see whether the conversion works at
// all. Bishop and knight mate only in a corner of the BISHOP's colour, and a
// search aimed at any other corner either runs the fifty-move clock out or hands
// the mating material over: near that boundary every continuation is correctly
// drawn, so nothing prefers keeping the pieces.
//
// Asserted in aggregate rather than per position, deliberately. The corner target
// gives the search the right destination but not the manoeuvre, so one start in
// five still runs the clock out, and WHICH one moves with any perturbation of the
// corner weight or the search depth. A per-position gate would therefore encode
// whichever position happens to convert today. Docs/TestDesign.md carries the
// perturbation measurements behind that choice.
//
// Slow by construction: five starts, up to a hundred fixed-depth searches each.
// Include order follows TacticalFullTests.cpp: the Catch2 headers come first
// because the engine headers below lean on the STL includes they drag in.
#include <catch2/catch_test_macros.hpp>
#include "TacticalTestHelpers.h" // make_tactical_engine — nothing tactical about it, but it is the one AIPerplex factory
#include "Board.h"
#include "GameState.h"

namespace {

	struct ConversionCase {
		const char* label;
		const char* fen;
		GameStates expected; // which side must deliver the mate
	};

	// The first two are tablebase-confirmed wins with the halfmove clock ZEROED. At
	// the clock they were recorded on (94) the Lichess tablebase calls them
	// `cursed-win` — won, but no longer inside the fifty-move rule — so there they
	// are not oracles at all: six plies remain and the shortest win is 46 and 44
	// plies away. Zeroed, the position is unchanged, the win is tablebase-confirmed,
	// and those DTZ figures are an exact optimal-play budget.
	//
	// The last three start the defending king in a corner of the WRONG colour, the
	// hardest case for a corner target: the king has to be walked the length of the
	// board before it can be mated. The third is
	// the second one's colour mirror, and the pair is the reason this test does not
	// gate per position — the two are the same position up to reflection, and the
	// search converts one or the other depending on tie-breaks.
	constexpr ConversionCase kConversionCases[] = {
	    {"tablebase #1 (Bg6 light, dtz 46)", "5k2/8/5KB1/8/8/1N6/8/8 b - - 0 1", GameStates::WHITE_WON},
	    {"tablebase #2 (Bg3 dark, dtz 44)", "2n5/8/8/8/8/5kb1/8/6K1 w - - 0 1", GameStates::BLACK_WON},
	    {"tablebase #2 mirrored (Bg6 light)", "6k1/8/5KB1/8/8/8/8/2N5 b - - 0 1", GameStates::WHITE_WON},
	    {"light bishop, king in a dark corner", "7k/8/5K2/8/2B5/5N2/8/8 w - - 0 1", GameStates::WHITE_WON},
	    {"dark bishop, king in a light corner", "k7/8/2K5/8/5B2/1N6/8/8 w - - 0 1", GameStates::WHITE_WON},
	};

	// Deep enough to convert once the king is cornered, cheap enough to keep the
	// suite at a few seconds. Docs/TestDesign.md records what a deeper search does
	// and does not buy here.
	constexpr unsigned kConversionDepth = 12;

	// King + bishop + knight: 10000 + 300 + 300 (g_iPieceValues). Anything less means
	// the winning side gave a piece away.
	constexpr int kWinnerMaterial = 10600;

	struct ConversionResult {
		int mated = 0;
		int materialLost = 0;
		int clockExpired = 0;
		int stalemated = 0;
		// Every other terminal verdict, the winning side being mated included. Kept
		// as its own counter so the aggregate gate below cannot absorb one: with
		// `mated >= 4` over five starts, an uncounted catastrophe would be invisible.
		int other = 0;
	};

} // namespace

TEST_CASE("Endgame - bishop and knight convert against a bare king", "[endgame_conversion][slow]")
{
	ConversionResult tally;

	for (const ConversionCase& tc : kConversionCases) {
		INFO(tc.label);

		// ONE engine plays both sides, so the defender is the same search and is not
		// adversarial — a mate says the conversion technique exists, not that it beats
		// best defence. The tablebase-derived starts are what compensate for that.
		Board board(tc.fen);
		auto ai = make_tactical_engine(kConversionDepth);
		const eColor winner = (tc.expected == GameStates::WHITE_WON) ? WHITE : BLACK;
		REQUIRE(board.GetMaterialScore(winner) == kWinnerMaterial);

		while (board.halfmove_clock() < HALFMOVE_CLOCK_LIMIT) {
			const SearchResult result = ai->Search(board, SearchLimits::fixed_depth(kConversionDepth));
			if (result.game_state != GameStates::STILL_PLAYING) {
				if (result.game_state == tc.expected)
					++tally.mated;
				else if (result.game_state == GameStates::DRAW_PAT)
					++tally.stalemated;
				else
					++tally.other;
				break;
			}
			REQUIRE(!result.best_move.is_null());
			REQUIRE(board.DoMove(result.best_move));

			// Checked every ply rather than at the end: once a piece is gone the
			// position is a dead draw and playing on measures nothing.
			if (board.GetMaterialScore(winner) < kWinnerMaterial) {
				++tally.materialLost;
				break;
			}
		}

		if (board.halfmove_clock() >= HALFMOVE_CLOCK_LIMIT)
			++tally.clockExpired;
	}

	CAPTURE(tally.mated, tally.materialLost, tally.clockExpired, tally.stalemated, tally.other);

	// Every start must land in exactly one bucket, or an outcome nobody thought of —
	// the winning side mated, a draw reported before the clock check — would pass the
	// gate by not being counted at all.
	REQUIRE(tally.other == 0);
	REQUIRE(tally.mated + tally.materialLost + tally.clockExpired + tally.stalemated ==
	        static_cast<int>(std::size(kConversionCases)));

	// The mating material must survive. A search drifting toward the fifty-move
	// boundary finds every continuation drawn and will part with a piece for nothing,
	// which is a conversion failure — a tiebreak preferring material would hide it
	// rather than fix it.
	REQUIRE(tally.materialLost == 0);

	// Stalemate is a different finding with a different fix from running the clock
	// out — the corner target rewards both cornering the bare king and closing king
	// distance, which is exactly how a winning side stalemates one.
	REQUIRE(tally.stalemated == 0);

	// The gate. Four of five is what the corner target delivers; the remaining start
	// runs the clock out because the search has the destination but not the
	// manoeuvre.
	REQUIRE(tally.mated >= 4);

	// Not a gate: the fifth start converting means the technique gap closed, which
	// is worth noticing rather than failing on.
	if (tally.mated < static_cast<int>(std::size(kConversionCases)))
		WARN("KBN conversion still incomplete: " << tally.mated << " of " << std::size(kConversionCases)
		                                         << " starts mated, " << tally.clockExpired
		                                         << " ran the fifty-move clock out");
}
