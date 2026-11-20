// SearchMultiPVTests.cpp — multi-PV root ranking (#94): SetMultiPV(N) makes one search return its
// N best root moves, each with an exact score and its own line, from one completed depth.
//
// The invariant tests run with singular extensions on and off, because pvs()'s one exclusion test
// serves both the singular verification and the multi-PV root.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "PVIntegrity.h"
#include "SearchTestFixture.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>

namespace {
	constexpr const char* kKiwipete = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";
	constexpr std::array<const char*, 4> kPositions = {
	    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
	    kKiwipete,
	    "2r3k1/1p3pp1/p3p2p/8/2PR4/1P3P2/P4KPP/8 w - - 0 1",
	    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
	};
	// White's Qd1xd5 wins a queen outright: every full-move-set search's root move.
	constexpr const char* kFreeQueen = "4k3/8/8/3q4/8/8/8/3QK3 w - - 0 1";

	// Deep enough for singular extensions to fire once arm_singular() lowers their depth gate.
	constexpr int kDepth = 6;

	void arm_singular(const AIPerlexTestFixture& fix, bool enabled)
	{
		fix.set_singular_enabled(enabled);
		fix.set_singular_min_depth(4);
	}

	// The result invariant: best_move then alternatives are distinct legal root moves, scores never
	// rise, every line starts with its move and replays legally, and the set has the expected size.
	void check_ranked_set(const Board& root, const SearchResult& result, size_t expected_alternatives)
	{
		REQUIRE_FALSE(result.best_move.is_null());
		REQUIRE_FALSE(result.pv.empty());
		CHECK(result.pv.front() == result.best_move);
		REQUIRE(result.alternatives.size() == expected_alternatives);

		std::vector<Move> seen{result.best_move};
		int previous_score = result.best_score;
		for (const RootLine& line : result.alternatives) {
			CHECK(line.score <= previous_score);
			previous_score = line.score;
			REQUIRE_FALSE(line.pv.empty());
			CHECK(line.pv.front() == line.move);
			CHECK(pv_replays_legally(root, line.pv));
			CHECK(std::find(seen.begin(), seen.end(), line.move) == seen.end());
			seen.push_back(line.move);
		}
	}

	size_t expected_alternatives(const AIPerlexTestFixture& fix, unsigned n)
	{
		return std::min(static_cast<size_t>(n), static_cast<size_t>(fix.count_legal_moves())) - 1;
	}

	// The first move the generator emits that is legal: what the emergency path would play.
	Move first_legal_move(Board board)
	{
		MoveList moves;
		MoveGenerator::ComputeLegalMoves(board, moves);
		for (const Move& move : moves)
			if (board.DoMove(move)) {
				board.UndoMove(move);
				return move;
			}
		return Move::EmptyMove();
	}

	struct Emitted {
		int depth;
		int64_t nodes;
		std::vector<Move> pv;
		std::vector<RootLine> alternatives;
	};

	SearchResult search(const AIPerlexTestFixture& fix, const SearchLimits& limits,
	                    std::vector<Emitted>* emitted = nullptr)
	{
		IterationObserver observer;
		if (emitted != nullptr)
			observer = [emitted](const IterationInfo& info) {
				emitted->push_back({info.depth, info.nodes, info.pv, info.alternatives});
			};
		return fix.ai->Search(fix.board_, limits, std::move(observer));
	}
} // namespace

TEST_CASE("MultiPV: SetMultiPV clamps to [1, MAX_MULTIPV]", "[multipv]")
{
	AIPerplex ai(AIPerplexConfig{.hash_mb = 1});
	CHECK(ai.MultiPV() == 1);
	ai.SetMultiPV(0);
	CHECK(ai.MultiPV() == 1);
	ai.SetMultiPV(4);
	CHECK(ai.MultiPV() == 4);
	ai.SetMultiPV(100'000);
	CHECK(ai.MultiPV() == AIPerplex::MAX_MULTIPV);
	ai.StartNewGame();
	CHECK(ai.MultiPV() == AIPerplex::MAX_MULTIPV);
}

TEST_CASE("MultiPV: the result is a distinct, sorted, legal set from one depth", "[multipv]")
{
	const bool singular = GENERATE(true, false);
	const unsigned n = GENERATE(2u, 4u);
	for (const char* fen : kPositions) {
		INFO(fen << " N=" << n << " singular=" << singular);
		AIPerlexTestFixture fix(fen);
		arm_singular(fix, singular);
		fix.ai->SetMultiPV(n);
		const SearchResult result = search(fix, SearchLimits::fixed_depth(kDepth));
		CHECK(result.depth_completed == kDepth);
		check_ranked_set(fix.board_, result, expected_alternatives(fix, n));
		CHECK(fix.root_excluded_count() == 0);
		if (singular)
			CHECK(fix.singular_verifications() > 0); // the singular half of the exclusion test ran
	}
}

TEST_CASE("MultiPV: N = 1 has no alternatives", "[multipv]")
{
	AIPerlexTestFixture fix(kKiwipete);
	const SearchResult result = search(fix, SearchLimits::fixed_depth(4));
	CHECK(result.alternatives.empty());
}

TEST_CASE("MultiPV: N above the legal move count ranks every legal move", "[multipv]")
{
	// The cornered king has three moves. Verified against python-chess.
	AIPerlexTestFixture three("7k/8/8/8/8/8/8/K7 w - - 0 1");
	REQUIRE(three.count_legal_moves() == 3);
	three.ai->SetMultiPV(5);
	const SearchResult result = search(three, SearchLimits::fixed_depth(4));
	check_ranked_set(three.board_, result, 2);
}

TEST_CASE("MultiPV: a terminal root returns no move and no alternatives", "[multipv]")
{
	const char* fen = GENERATE("7k/6Q1/6K1/8/8/8/8/8 b - - 0 1",  // checkmated
	                           "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1"); // stalemated
	AIPerlexTestFixture fix(fen);
	REQUIRE(fix.count_legal_moves() == 0);
	fix.ai->SetMultiPV(4);
	const SearchResult result = search(fix, SearchLimits::fixed_depth(4));
	CHECK(result.best_move.is_null());
	CHECK(result.alternatives.empty());
	CHECK(fix.root_excluded_count() == 0);
}

TEST_CASE("MultiPV: a mate in one ranks first", "[multipv]")
{
	// Ra8# — the knight cannot interpose on the eighth rank. Verified against python-chess.
	AIPerlexTestFixture fix("6k1/5ppp/8/8/2n5/8/5PPP/R5K1 w - - 0 1");
	fix.ai->SetMultiPV(AIPerplex::MAX_MULTIPV);
	const SearchResult result = search(fix, SearchLimits::fixed_depth(4));
	check_ranked_set(fix.board_, result, expected_alternatives(fix, AIPerplex::MAX_MULTIPV));
	CHECK(MoveFormatter::ToUCI(result.best_move) == "a1a8");
	CHECK(result.best_score >= GameValues::Mate_Threshold);
	for (const RootLine& line : result.alternatives)
		CHECK(line.score < GameValues::Mate_Threshold);
}

TEST_CASE("MultiPV: moves that allow a mate in one rank last", "[multipv]")
{
	// A rook leaving the first rank along the a-file allows ...Re1#, as does Re1 itself (...Rxe1#);
	// Ra8 is met by ...Rxa8.
	// Verified against python-chess.
	AIPerlexTestFixture fix("4r1k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1");
	fix.ai->SetMultiPV(AIPerplex::MAX_MULTIPV);
	const SearchResult result = search(fix, SearchLimits::fixed_depth(4));
	check_ranked_set(fix.board_, result, expected_alternatives(fix, AIPerplex::MAX_MULTIPV));

	const std::vector<std::string> mated = {"a1a2", "a1a3", "a1a4", "a1a5", "a1a6", "a1a7", "a1e1"};
	const size_t total = result.alternatives.size();
	REQUIRE(total >= mated.size());
	for (size_t i = 0; i < total; ++i) {
		const RootLine& line = result.alternatives[i];
		const bool is_mated = std::find(mated.begin(), mated.end(), MoveFormatter::ToUCI(line.move)) != mated.end();
		INFO(MoveFormatter::ToUCI(line.move) << " score " << line.score);
		CHECK(is_mated == (i >= total - mated.size()));
		CHECK(is_mated == (line.score <= -GameValues::Mate_Threshold));
	}
}

TEST_CASE("MultiPV: depth-1 scores are exact, not bounds", "[multipv]")
{
	// No white move gives check or lets Black capture, so each child's quiescence is its stand-pat
	// and every root move's true depth-1 score is -Evaluate(child). Verified against python-chess.
	AIPerlexTestFixture fix("1n2k3/p6p/8/8/8/8/P6P/1N2K3 w - - 0 1");
	const int legal = fix.count_legal_moves();
	fix.ai->SetMultiPV(static_cast<unsigned>(legal));

	std::map<std::string, int> oracle;
	Board board = fix.board_;
	MoveList moves;
	MoveGenerator::ComputeLegalMoves(board, moves);
	for (const Move& move : moves) {
		if (!board.DoMove(move))
			continue;
		REQUIRE_FALSE(board.InCheck());
		MoveList replies;
		MoveGenerator::ComputeCaptures(board, replies);
		REQUIRE(replies.empty());
		oracle[MoveFormatter::ToUCI(move)] = -fix.evaluate(board);
		board.UndoMove(move);
	}
	REQUIRE(static_cast<int>(oracle.size()) == legal);

	const SearchResult result = search(fix, SearchLimits::fixed_depth(1));
	check_ranked_set(fix.board_, result, static_cast<size_t>(legal - 1));
	CHECK(result.best_score == oracle.at(MoveFormatter::ToUCI(result.best_move)));
	for (const RootLine& line : result.alternatives) {
		INFO(MoveFormatter::ToUCI(line.move));
		CHECK(line.score == oracle.at(MoveFormatter::ToUCI(line.move)));
	}
	const int best = std::max_element(oracle.begin(), oracle.end(), [](const auto& a, const auto& b) {
		                 return a.second < b.second;
	                 })->second;
	CHECK(result.best_score == best);
}

TEST_CASE("MultiPV: later ranks store nothing at the root key", "[multipv]")
{
	// Without the guard the last rank stores last, naming a move other than the free capture.
	const unsigned threads = GENERATE(1u, 2u);
	const unsigned n = threads == 1 ? 4u : 3u;
	INFO("threads=" << threads << " N=" << n);
	AIPerlexTestFixture fix(kFreeQueen);
	fix.ai->SetThreads(threads);
	fix.ai->SetMultiPV(n);
	const SearchResult result = search(fix, SearchLimits::fixed_depth(kDepth));
	check_ranked_set(fix.board_, result, n - 1);
	REQUIRE(MoveFormatter::ToUCI(result.best_move) == "d1d5");
	const auto entry = fix.probe_tt(0);
	REQUIRE(entry.has_value());
	CHECK(entry->best_move == result.best_move);
}

TEST_CASE("MultiPV: the observer sees every emitted depth's alternatives", "[multipv]")
{
	AIPerlexTestFixture fix(kKiwipete);
	fix.ai->SetMultiPV(3);
	std::vector<Emitted> emitted;
	const SearchResult result = search(fix, SearchLimits::fixed_depth(kDepth), &emitted);
	REQUIRE(emitted.size() == static_cast<size_t>(kDepth));
	for (const Emitted& info : emitted) {
		INFO("depth " << info.depth);
		CHECK(info.alternatives.size() == 2);
		REQUIRE_FALSE(info.pv.empty());
	}
	CHECK(emitted.back().pv == result.pv);
	REQUIRE(emitted.back().alternatives.size() == result.alternatives.size());
	for (size_t i = 0; i < result.alternatives.size(); ++i) {
		CHECK(emitted.back().alternatives[i].move == result.alternatives[i].move);
		CHECK(emitted.back().alternatives[i].score == result.alternatives[i].score);
		CHECK(emitted.back().alternatives[i].pv == result.alternatives[i].pv);
	}
}

TEST_CASE("MultiPV: a stop inside a set keeps the previous depth's complete set", "[multipv]")
{
	constexpr unsigned kN = 4;
	constexpr int kMaxDepth = 9;
	std::vector<Emitted> full;
	{
		AIPerlexTestFixture fix(kKiwipete, kMaxDepth);
		fix.ai->SetMultiPV(kN);
		(void)search(fix, SearchLimits::fixed_depth(kMaxDepth), &full);
	}
	REQUIRE(full.size() == static_cast<size_t>(kMaxDepth));
	// The last depth costs several poll intervals, so a limit halfway into it lands inside the set.
	const Emitted& kept = full[full.size() - 2];
	const int64_t limit = (kept.nodes + full.back().nodes) / 2;
	REQUIRE(full.back().nodes - kept.nodes > 20'000);

	AIPerlexTestFixture fix(kKiwipete, kMaxDepth + 1);
	fix.ai->SetMultiPV(kN);
	const SearchResult result = search(fix, SearchLimits::fixed_nodes(limit));
	CHECK(result.depth_completed == kept.depth);
	CHECK(result.pv == kept.pv);
	REQUIRE(result.alternatives.size() == kept.alternatives.size());
	for (size_t i = 0; i < kept.alternatives.size(); ++i) {
		CHECK(result.alternatives[i].move == kept.alternatives[i].move);
		CHECK(result.alternatives[i].score == kept.alternatives[i].score);
	}
	CHECK(fix.root_excluded_count() == 0);
}

TEST_CASE("MultiPV: a stop inside depth 1's set falls back to its rank 1", "[multipv]")
{
	// Every root move ranked, and Black has only a king, so quiescence stays small: depth 1's rank 1
	// takes about 100 node entries and the set about 2,500, so the first poll, at entry 1024, stops a
	// one-node search between them. Rank 1's move is not the first generated legal move, so the
	// fallback is told apart from the emergency path's first legal move.
	constexpr const char* kFen = "4k3/8/8/8/8/8/8/RN1QKBNR w KQ - 0 1";
	AIPerlexTestFixture fix(kFen, 8);
	fix.ai->SetMultiPV(AIPerplex::MAX_MULTIPV);
	REQUIRE(AIPerlexTestFixture::tuning(*fix.ai).min_nodes_threshold == SearchTuning{}.min_nodes_threshold);

	Move rank1;
	{
		AIPerlexTestFixture unlimited(kFen);
		rank1 = search(unlimited, SearchLimits::fixed_depth(1)).best_move;
	}
	REQUIRE(rank1 != first_legal_move(fix.board_));

	const SearchResult result = search(fix, SearchLimits::fixed_nodes(1));
	CHECK(result.depth_completed == 1);
	CHECK(result.best_move == rank1);
	CHECK(result.alternatives.empty());
	REQUIRE_FALSE(result.pv.empty());
	CHECK(result.pv.front() == rank1);
	CHECK(fix.root_excluded_count() == 0);
}

// Characterisation for #94: the node cost of N = 4 over N = 1 on the Run-Bench positions at a fixed
// depth, and how often sorting changed rank 1. Deterministic, so no quiet window. Reports, no threshold.
TEST_CASE("MultiPV: N = 4 node multiplier on the bench positions", "[.multipv-cost]")
{
	constexpr std::array<const char*, 8> kBench = {
	    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
	    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
	    "2r3k1/1p3pp1/p3p2p/8/2PR4/1P3P2/P4KPP/8 w - - 0 1",
	    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
	    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
	    "r1bqkb1r/pp3ppp/2n1pn2/2pp4/3P1B2/2PBPN2/PP3PPP/RN1QK2R w KQkq - 0 7",
	    "r1bq1rk1/pp2ppbp/2np1np1/8/2PNP3/2N1B3/PP2BPPP/R2QK2R w KQ - 0 9",
	    "2r3k1/pp3pp1/4p2p/3n4/3P4/P1NBP3/1P3PPP/2R3K1 w - - 0 1",
	};
	constexpr int kCostDepth = 12;
	int64_t nodes1 = 0;
	int64_t nodes4 = 0;
	int64_t sets = 0;
	int64_t resorted = 0;
	for (const char* fen : kBench) {
		for (const unsigned n : {1u, 4u}) {
			AIPerlexTestFixture fix(fen);
			(void)fix.ai->SetHash(64);
			fix.ai->SetMultiPV(n);
			std::vector<Emitted> emitted;
			const SearchResult result = search(fix, SearchLimits::fixed_depth(kCostDepth), &emitted);
			const int64_t nodes = result.nodes_searched + result.qnodes_searched;
			std::printf("%-70s N=%u nodes=%lld\n", fen, n, static_cast<long long>(nodes));
			if (n == 1) {
				nodes1 += nodes;
			} else {
				nodes4 += nodes;
				sets += static_cast<int64_t>(emitted.size());
				resorted += fix.multipv_rank1_resorted();
			}
		}
	}
	std::printf("depth %d: N=4 / N=1 node multiplier %.2f (%lld / %lld)\n", kCostDepth,
	            static_cast<double>(nodes4) / static_cast<double>(nodes1), static_cast<long long>(nodes4),
	            static_cast<long long>(nodes1));
	std::printf("sorting changed rank 1 in %lld of %lld completed sets (%.1f%%)\n", static_cast<long long>(resorted),
	            static_cast<long long>(sets), 100.0 * static_cast<double>(resorted) / static_cast<double>(sets));
	CHECK(nodes1 > 0);
}
