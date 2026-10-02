// Short KBN finishes through production search; general confinement is outside this contract.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include "TacticalTestHelpers.h"
#include "MoveFormatter.h"
#include "MoveGenerator.h"

namespace {

	struct MateCase {
		const char* label;
		const char* position; // FEN through the en-passant field; clock supplied by the test
		unsigned plies;
	};

	// Bg7#; and Bg7+ Kg8 Nf6#. Horizontal and colour/rank mirrors cover both sides
	// and bishop square colours. The complete short mate trees are verified offline.
	constexpr MateCase kMateCases[] = {
	    {"white M1, light bishop", "7k/5K2/5N1B/8/8/8/8/8 w - -", 1},
	    {"white M1, dark bishop", "k7/2K5/B1N5/8/8/8/8/8 w - -", 1},
	    {"black M1, dark bishop", "8/8/8/8/8/5n1b/5k2/7K b - -", 1},
	    {"black M1, light bishop", "8/8/8/8/8/b1n5/2k5/K7 b - -", 1},
	    {"white M2, light bishop", "7k/8/6KB/8/6N1/8/8/8 w - -", 3},
	    {"white M2, dark bishop", "k7/8/BK6/8/1N6/8/8/8 w - -", 3},
	    {"black M2, dark bishop", "8/8/8/6n1/8/6kb/8/7K b - -", 3},
	    {"black M2, light bishop", "8/8/8/1n6/8/bk6/8/K7 b - -", 3},
	};

	MoveList legal_moves(Board& board)
	{
		MoveList candidates;
		MoveGenerator::ComputeLegalMoves(board, candidates);
		MoveList legal;
		for (const Move& move : candidates) {
			if (board.IsLegalMove(move))
				legal.push(move);
		}
		return legal;
	}

	void require_mate(Board board, eColor winner, unsigned plies_left, unsigned depth)
	{
		INFO("position: " << board.ExtractFEN());
		const MoveList legal = legal_moves(board);
		if (legal.empty()) {
			REQUIRE(board.GetCurrentColor() != winner);
			REQUIRE(board.InCheck());
			return;
		}
		REQUIRE(plies_left > 0);
		REQUIRE(board.halfmove_clock() < HALFMOVE_CLOCK_LIMIT);

		if (board.GetCurrentColor() == winner) {
			// Fresh state makes every decision independent of previous fixtures and branches.
			auto ai = make_tactical_engine(depth);
			const SearchResult result = ai->Search(board, SearchLimits::fixed_depth(depth));
			REQUIRE(result.game_state == GameStates::STILL_PLAYING);
			REQUIRE(!result.best_move.is_null());
			INFO("chosen move: " << MoveFormatter::ToUCI(result.best_move));
			REQUIRE(std::find(legal.begin(), legal.end(), result.best_move) != legal.end());
			REQUIRE(board.DoMove(result.best_move));
			require_mate(board, winner, plies_left - 1, depth);
			return;
		}

		// Search cannot choose a cooperative defender: every legal reply must still mate.
		for (const Move& reply : legal) {
			INFO("defensive reply: " << MoveFormatter::ToUCI(reply));
			REQUIRE(board.DoMove(reply));
			require_mate(board, winner, plies_left - 1, depth);
			board.UndoMove(reply);
		}
	}

} // namespace

TEST_CASE("Endgame - bishop and knight finish short forced mates", "[endgame_conversion][search]")
{
	const auto& tc = GENERATE(Catch::Generators::from_range(kMateCases));
	const unsigned depth = GENERATE(4u, 6u, 8u);
	const int clock = GENERATE(0, 94);
	CAPTURE(tc.label, depth, clock);
	Board board;
	REQUIRE(board.SetupFromFEN(std::string(tc.position) + " " + std::to_string(clock) + " 1"));
	require_mate(board, board.GetCurrentColor(), tc.plies, depth);
}
