// SearchHistoryTests.cpp — the quiet history table's update rule: gravity-bounded bonus and malus,
// aging, and which moves a quiet cutoff penalizes.

#include <catch2/catch_test_macros.hpp>
#include "SearchTestFixture.h"
#include "Board.h"
#include "MoveFormatter.h"
#include "MoveGenerator.h"
#include "Sort.h"
#include "ThreadData.h"
#include <memory>

namespace {

	constexpr int32_t kMax = ThreadData::HISTORY_MAX;

	int32_t& entry(ThreadData& td, eColor side, const Move& move) { return td.history[side][move.from()][move.to()]; }

} // namespace

TEST_CASE("History - a bonus from zero is depth squared, a malus its negative", "[search][history]")
{
	auto td = std::make_unique<ThreadData>();
	const Board board("4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
	const Move quiet = MoveFormatter::FromUCI("a1a5", board);

	td->update_history(WHITE, quiet, 4);
	CHECK(entry(*td, WHITE, quiet) == 16);

	td->clear_history();
	td->penalize_history(WHITE, quiet, 4);
	CHECK(entry(*td, WHITE, quiet) == -16);
}

TEST_CASE("History - gravity keeps every entry within HISTORY_MAX", "[search][history]")
{
	auto td = std::make_unique<ThreadData>();
	const Board board("4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
	const Move quiet = MoveFormatter::FromUCI("a1a5", board);

	// Depth 200 exceeds the unclamped bonus's bound, so it also checks the bonus clamp.
	for (const int depth : {3, 20, 127, 200}) {
		td->clear_history();
		for (int i = 0; i < 2'000; ++i) {
			td->update_history(WHITE, quiet, depth);
			REQUIRE(entry(*td, WHITE, quiet) <= kMax);
		}
		for (int i = 0; i < 2'000; ++i) {
			td->penalize_history(WHITE, quiet, depth);
			REQUIRE(entry(*td, WHITE, quiet) >= -kMax);
		}
	}
}

TEST_CASE("History - aging decays both signs toward zero", "[search][history]")
{
	auto td = std::make_unique<ThreadData>();
	td->history[WHITE][0][1] = -1;
	td->history[WHITE][0][2] = 3;
	td->history[BLACK][0][3] = -7;

	td->age_history();

	CHECK(td->history[WHITE][0][1] == 0);
	CHECK(td->history[WHITE][0][2] == 1);
	CHECK(td->history[BLACK][0][3] == -3);
}

TEST_CASE("History - captures and promotions take neither bonus nor malus", "[search][history]")
{
	auto td = std::make_unique<ThreadData>();
	const Board board("4k3/1P6/8/8/8/8/r7/R3K3 w - - 0 1");
	const Move capture = MoveFormatter::FromUCI("a1a2", board);
	const Move promotion = MoveFormatter::FromUCI("b7b8q", board);
	REQUIRE(MoveHelper::IsCapture(capture));
	REQUIRE(MoveHelper::IsPromote(promotion));

	// Checked after each update: a bonus and a malus of one depth cancel exactly, so a combined check
	// would pass with both applied.
	for (const Move& m : {capture, promotion}) {
		td->update_history(WHITE, m, 6);
		CHECK(entry(*td, WHITE, m) == 0);
		td->penalize_history(WHITE, m, 6);
		CHECK(entry(*td, WHITE, m) == 0);
	}
}

TEST_CASE("History - a quiet cutoff penalizes exactly the searched quiets sorted before it", "[search][history]")
{
	auto td = std::make_unique<ThreadData>();
	// Ra1xa2 is a capture and b7b8 promotes; every other white move is quiet.
	const Board board("4k3/1P6/8/8/8/8/r7/R3K3 w - - 0 1");

	MoveList moveList;
	MoveGenerator::ComputeLegalMoves(board, moveList);
	const int n = static_cast<int>(moveList.size());
	std::array<std::pair<int, int>, MoveList::MAX_MOVES> scored_idx;
	MoveSorter::ScoreMoves(moveList, n, board, WHITE, Move::EmptyMove(), Move::EmptyMove(), Move::EmptyMove(),
	                       td->history, scored_idx);
	const auto sorted = [&](int i) { return moveList[scored_idx[static_cast<size_t>(i)].second]; };

	// Captures and promotions sort ahead of the quiets; find the first quiet index.
	int first_quiet = 0;
	while (first_quiet < n && !ThreadData::is_quiet(sorted(first_quiet)))
		++first_quiet;
	REQUIRE(first_quiet >= 2); // the capture and at least one promotion precede it
	const int cut_index = first_quiet + 4;
	REQUIRE(cut_index + 1 < n);
	const int skipped = first_quiet + 2; // a quiet whose child search never ran

	ThreadData::SearchedMoves searched;
	for (int i = 0; i <= cut_index; ++i)
		if (i != skipped)
			searched.set(static_cast<size_t>(i));
	searched.set(static_cast<size_t>(cut_index) + 1); // a bit past the cut must be ignored

	constexpr int depth = 5;
	td->penalize_searched_quiets(WHITE, moveList, scored_idx, searched, cut_index, depth);

	for (int i = 0; i < n; ++i) {
		const Move m = sorted(i);
		const bool penalized = i < cut_index && i != skipped && ThreadData::is_quiet(m);
		INFO("sorted index " << i << " (" << MoveFormatter::ToUCI(m) << ")");
		CHECK(entry(*td, WHITE, m) == (penalized ? -depth * depth : 0));
	}
}

TEST_CASE("History - a real search leaves penalized entries, all within bounds", "[search][history]")
{
	AIPerlexTestFixture fix("r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4");
	fix.get_move_at_threads(1, 6);

	const auto [lo, hi] = fix.history_range();
	CHECK(lo < 0);
	CHECK(lo >= -kMax);
	CHECK(hi <= kMax);
}
