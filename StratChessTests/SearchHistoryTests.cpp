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

TEST_CASE("Continuation history - gravity keeps int16_t entries within HISTORY_MAX", "[search][history]")
{
	auto td = std::make_unique<ThreadData>();
	td->board = Board("4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
	const Move quiet = MoveFormatter::FromUCI("a1a5", td->board);
	td->cont_key[1] = static_cast<uint16_t>(continuation_index(BLACK_KING, e8));
	td->cont_key[0] = static_cast<uint16_t>(continuation_index(WHITE_ROOK, a5));
	const ContinuationRows rows = td->continuation_rows(1, 2);
	REQUIRE(rows.one_ply != nullptr);
	REQUIRE(rows.two_ply != nullptr);
	const int col = continuation_index(WHITE_ROOK, a5);

	for (const int depth : {3, 127, 200}) {
		td->clear_continuation_history();
		for (int i = 0; i < 2'000; ++i) {
			td->update_history(WHITE, quiet, depth, rows);
			REQUIRE(rows.one_ply[col] <= kMax);
			REQUIRE(rows.two_ply[col] <= kMax);
		}
		CHECK(rows.one_ply[col] > kMax / 2);
		for (int i = 0; i < 2'000; ++i) {
			td->penalize_history(WHITE, quiet, depth, rows);
			REQUIRE(rows.one_ply[col] >= -kMax);
			REQUIRE(rows.two_ply[col] >= -kMax);
		}
		CHECK(rows.one_ply[col] < 0);
	}
}

TEST_CASE("Continuation history - a large entry outranks a larger history entry alone", "[search][history]")
{
	auto td = std::make_unique<ThreadData>();
	const Board board("4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
	const Move by_history = MoveFormatter::FromUCI("a1a5", board);
	const Move by_continuation = MoveFormatter::FromUCI("e1d2", board);
	td->history[WHITE][by_history.from()][by_history.to()] = 1'000;
	td->history[WHITE][by_continuation.from()][by_continuation.to()] = 0;
	td->cont_key[1] = static_cast<uint16_t>(continuation_index(BLACK_KING, e8));
	const ContinuationRows rows = td->continuation_rows(1, 1);
	REQUIRE(rows.one_ply != nullptr);
	rows.one_ply[continuation_index(WHITE_KING, d2)] = 2'000;

	MoveList moveList;
	MoveGenerator::ComputeLegalMoves(board, moveList);
	const int n = static_cast<int>(moveList.size());
	std::array<std::pair<int, int>, MoveList::MAX_MOVES> scored_idx;
	const auto first = [&] { return moveList[scored_idx[0].second]; };

	MoveSorter::ScoreMoves(moveList, n, board, WHITE, Move::EmptyMove(), Move::EmptyMove(), Move::EmptyMove(),
	                       td->history, scored_idx);
	CHECK(first() == by_history);

	MoveSorter::ScoreMoves(moveList, n, board, WHITE, Move::EmptyMove(), Move::EmptyMove(), Move::EmptyMove(),
	                       td->history, scored_idx, rows.one_ply, rows.two_ply);
	CHECK(first() == by_continuation);
}

TEST_CASE("Continuation history - a null move leaves its child no one-ply row", "[search][history]")
{
	// White is a queen and rook up, so the null move's reply cannot reach beta and it cuts.
	AIPerlexTestFixture fix("4k3/pppp4/8/8/8/8/PPPP4/RQ2K3 w - - 0 1");
	fix.arm_clock();
	constexpr int ply = 2;
	fix.cont_key(ply) = static_cast<uint16_t>(continuation_index(BLACK_PAWN, a6));
	fix.cont_key(ply + 1) = static_cast<uint16_t>(continuation_index(WHITE_KING, d1));

	// Depth 5 is above the reverse futility band, so the null move is what cuts.
	fix.search_node(5, ply, -1'001, -1'000, /*is_pv_node=*/false);

	// Only the null move stores an entry with no move.
	const auto entry = fix.probe_tt(ply);
	REQUIRE(entry.has_value());
	CHECK(entry->best_move == Move::EmptyMove());
	CHECK(fix.cont_key(ply + 1) == ThreadData::kNoContinuation);
}

TEST_CASE("Continuation history - a search at zero plies leaves the table untouched", "[search][history]")
{
	constexpr const char* fen = "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4";
	AIPerlexTestFixture off(fen);
	off.set_continuation_history_plies(0);
	off.get_move_at_threads(1, 6);
	CHECK(off.continuation_range() == std::pair<int16_t, int16_t>{0, 0});

	// The same search with the table on does write it, all within bounds. A fresh fixture, because a
	// repeat search on a warm transposition table reaches almost no cutoffs.
	AIPerlexTestFixture on(fen);
	on.set_continuation_history_plies(2);
	on.get_move_at_threads(1, 6);
	const auto [lo, hi] = on.continuation_range();
	CHECK(lo < 0);
	CHECK(hi > 0);
	CHECK(lo >= -kMax);
	CHECK(hi <= kMax);
}
