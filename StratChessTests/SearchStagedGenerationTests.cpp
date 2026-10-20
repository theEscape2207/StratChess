// SearchStagedGenerationTests.cpp — staged move generation in pvs(): captures first, quiets when the
// loop reaches them.
//
// Staging must search the same tree, so the main test runs every position with it on and off and
// requires identical iterations. The eligibility cases move one guard at a time off a passing baseline.

#include <catch2/catch_test_macros.hpp>
#include "SearchTestFixture.h"

namespace {
	// The eight Run-Bench positions, then the move kinds staging has to place: promotions and en
	// passant, castling, en passant alone, and a stalemate one move away. Verified against python-chess.
	// Last, the one position seen to diverge when depth-3 frames are staged too: the bench positions
	// do not, so without it this test cannot see the depth bound broken.
	constexpr std::array<const char*, 13> kPositions = {
	    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
	    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
	    "2r3k1/1p3pp1/p3p2p/8/2PR4/1P3P2/P4KPP/8 w - - 0 1",
	    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
	    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
	    "r1bqkb1r/pp3ppp/2n1pn2/2pp4/3P1B2/2PBPN2/PP3PPP/RN1QK2R w KQkq - 0 7",
	    "r1bq1rk1/pp2ppbp/2np1np1/8/2PNP3/2N1B3/PP2BPPP/R2QK2R w KQ - 0 9",
	    "2r3k1/pp3pp1/4p2p/3n4/3P4/P1NBP3/1P3PPP/2R3K1 w - - 0 1",
	    "r1r2r1r/1P4P1/8/3pP2k/1p6/2p5/N7/4K3 w - d6 0 1",
	    "r3k2r/pppq1ppp/2npbn2/2b1p3/2B1P3/2NPBN2/PPPQ1PPP/R3K2R w KQkq - 0 1",
	    "rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3",
	    "8/8/8/8/5k2/8/5p2/5K2 b - - 0 1", // Kf3 stalemates
	    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
	};

	constexpr int kDepth = 7;

	struct Iteration {
		int depth;
		int score;
		int64_t nodes;
		std::vector<Move> pv;
		bool operator==(const Iteration&) const = default;
	};

	struct Run {
		std::vector<Iteration> iterations;
		Move best_move;
		int best_score;
	};

	Run search(const char* fen, SearchTuning tuning, bool staged)
	{
		tuning.staged_generation_enabled = staged;
		AIPerplex ai(AIPerplexConfig{.default_depth = kDepth, .hash_mb = 1, .verbose_logging = false});
		REQUIRE_FALSE(ai.SetTuning(tuning).has_value());
		Run run;
		const SearchResult result =
		    ai.Search(Board(fen), SearchLimits::fixed_depth(kDepth),
		              [&](const IterationInfo& i) { run.iterations.push_back({i.depth, i.score, i.nodes, i.pv}); });
		run.best_move = result.best_move;
		run.best_score = result.best_score;
		return run;
	}

	void require_same_tree(const SearchTuning& tuning)
	{
		for (const char* fen : kPositions) {
			INFO("fen = " << fen);
			const Run off = search(fen, tuning, false);
			const Run on = search(fen, tuning, true);
			REQUIRE(off.iterations.size() == static_cast<size_t>(kDepth));
			CHECK(on.iterations == off.iterations);
			CHECK(on.best_move == off.best_move);
			CHECK(on.best_score == off.best_score);
		}
	}
} // namespace

TEST_CASE("Staged generation searches the same tree", "[search]")
{
	SECTION("default tuning")
	{
		REQUIRE(SearchTuning{}.capture_history_enabled); // capture history's scores are read at frame start
		require_same_tree(SearchTuning{});
	}
	SECTION("singular extensions from depth 2, the lowest that keeps depth 2 staged")
	{
		SearchTuning tuning;
		tuning.singular_extensions_enabled = true;
		tuning.singular_min_depth = 2;
		require_same_tree(tuning);
	}
	SECTION("singular extensions from depth 1")
	{
		SearchTuning tuning;
		tuning.singular_extensions_enabled = true;
		tuning.singular_min_depth = 1;
		require_same_tree(tuning);
	}
}

TEST_CASE("Staged generation eligibility", "[search]")
{
	AIPerlexTestFixture fix;
	fix.set_staged_generation(true);
	const Move none = Move::EmptyMove();

	CHECK(fix.staged_generation_eligible(1, false, none));
	CHECK(fix.staged_generation_eligible(2, false, none));
	CHECK_FALSE(fix.staged_generation_eligible(3, false, none));
	CHECK_FALSE(fix.staged_generation_eligible(1, true, none));
	CHECK_FALSE(fix.staged_generation_eligible(1, false, AnyLegalMove()));

	SECTION("a singular extension reaching depth 1 removes depth 2")
	{
		SearchTuning tuning = fix.ai->Tuning();
		tuning.singular_min_depth = 1;
		REQUIRE_FALSE(fix.ai->SetTuning(tuning).has_value());
		CHECK(fix.staged_generation_eligible(1, false, none));
		CHECK_FALSE(fix.staged_generation_eligible(2, false, none));
	}

	SECTION("the flag")
	{
		fix.set_staged_generation(false);
		CHECK_FALSE(fix.staged_generation_eligible(1, false, none));
	}
}
