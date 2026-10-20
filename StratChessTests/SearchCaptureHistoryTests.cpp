// SearchCaptureHistoryTests.cpp — capture history: which cutoffs raise and lower an entry, how a fed
// entry orders captures inside their SEE tier and never across tiers, the kill switch, and quiescence
// staying blind to it.

#include <catch2/catch_test_macros.hpp>
#include "SearchTestFixture.h"
#include "Board.h"
#include "MoveFormatter.h"
#include "MoveGenerator.h"
#include "Sort.h"
#include "ThreadData.h"
#include <algorithm>
#include <memory>

namespace {

	constexpr int16_t kMax = ThreadData::HISTORY_MAX;

	// White to move, not in check. Good captures dxe4 (PxQ), dxc4 (PxR) and Nxc4 (NxR); Qxd5 is
	// losing, the pawn defended by e6. No two share a capture-history entry. Verified against
	// python-chess.
	constexpr const char* kCaptures = "k7/8/4p3/3p3Q/2r1q3/3P4/1N6/K7 w - - 0 1";

	struct Ordered {
		MoveList list;
		ScoredMoves scored_idx{};
		int n = 0;
		Move at(int i) const { return list[scored_idx[static_cast<size_t>(i)].second]; }
		int score_of(const Move& m) const { return scored_idx[static_cast<size_t>(index_of(m))].first; }
		int index_of(const Move& m) const
		{
			for (int i = 0; i < n; ++i)
				if (at(i) == m)
					return i;
			FAIL("move not in list");
			return -1;
		}
	};

	Ordered order(const ThreadData& td, bool use_capture_history, const Move& hash_move = Move::EmptyMove())
	{
		Ordered o;
		MoveGenerator::ComputeLegalMoves(td.board, o.list);
		o.n = static_cast<int>(o.list.size());
		MoveSorter::ScoreMoves(o.list, o.n, td.scoring_context(0, {}, use_capture_history), hash_move, o.scored_idx);
		return o;
	}

	std::unique_ptr<ThreadData> thread_data(const char* fen)
	{
		auto td = std::make_unique<ThreadData>();
		td->board = Board(fen);
		return td;
	}

} // namespace

TEST_CASE("Capture history - a capture or promotion that cuts gains depth squared", "[search][history]")
{
	auto td = thread_data(kCaptures);
	const Ordered o = order(*td, true);
	const Move cutter = MoveFormatter::FromUCI("b2c4", td->board);

	td->update_capture_history(o.list, o.scored_idx, {}, o.index_of(cutter), 5);

	CHECK(td->capture_history_entry(cutter) == 25);
	// Shares b2c4's square and victim, not its piece, so a coarser key would raise it too.
	CHECK(td->capture_history_entry(MoveFormatter::FromUCI("d3c4", td->board)) == 0);
}

TEST_CASE("Capture history - a cutoff penalizes exactly the searched captures sorted before it", "[search][history]")
{
	auto td = thread_data(kCaptures);
	const Ordered o = order(*td, true);
	constexpr int depth = 4;

	// The four captures sort first: three good, then the losing one below the (empty) killers.
	int first_quiet = 0;
	while (first_quiet < o.n && !ThreadData::is_quiet(o.at(first_quiet)))
		++first_quiet;
	REQUIRE(first_quiet == 4);
	constexpr int skipped = 1; // a capture whose child search never ran

	const auto check_entries = [&](int cut_index) {
		for (int i = 0; i < first_quiet; ++i) {
			const bool penalized = i < cut_index && i != skipped;
			const int expected = i == cut_index ? depth * depth : penalized ? -depth * depth : 0;
			INFO("cut " << cut_index << ", sorted index " << i << " (" << MoveFormatter::ToUCI(o.at(i)) << ")");
			CHECK(td->capture_history_entry(o.at(i)) == expected);
		}
	};
	const auto searched_through = [&](int cut_index) {
		ThreadData::SearchedMoves searched;
		for (int i = 0; i <= cut_index + 1; ++i) // a bit past the cut must be ignored
			if (i != skipped)
				searched.set(static_cast<size_t>(i));
		return searched;
	};

	SECTION("a capture cuts")
	{
		td->update_capture_history(o.list, o.scored_idx, searched_through(2), 2, depth);
		check_entries(2);
	}
	SECTION("a quiet cuts")
	{
		const int cut_index = first_quiet + 2;
		td->update_capture_history(o.list, o.scored_idx, searched_through(cut_index), cut_index, depth);
		check_entries(cut_index);
		CHECK(td->capture_history_entry(o.at(cut_index)) == 0);
	}
}

TEST_CASE("Capture history - a failed hash capture takes the malus, a cutting one the bonus", "[search][history]")
{
	auto td = thread_data(kCaptures);
	const Move hash = MoveFormatter::FromUCI("d3c4", td->board); // sorts below dxe4 without the hash
	const Ordered o = order(*td, true, hash);
	REQUIRE(o.at(0) == hash);
	ThreadData::SearchedMoves searched;
	searched.set(0);
	searched.set(1);

	td->update_capture_history(o.list, o.scored_idx, searched, 1, 3);
	CHECK(td->capture_history_entry(hash) == -9);

	td->clear_history();
	td->update_capture_history(o.list, o.scored_idx, searched, 0, 3);
	CHECK(td->capture_history_entry(hash) == 9);
}

TEST_CASE("Capture history - a fed entry reorders inside a tier, never across tiers", "[search][history]")
{
	auto td = thread_data(kCaptures);
	const Move pxq = MoveFormatter::FromUCI("d3e4", td->board);
	const Move nxr = MoveFormatter::FromUCI("b2c4", td->board);
	const Move losing = MoveFormatter::FromUCI("h5d5", td->board);

	CHECK(order(*td, true).at(0) == pxq);

	td->capture_history_entry(nxr) = kMax;
	CHECK(order(*td, true).at(0) == nxr);
	CHECK(order(*td, false).at(0) == pxq);

	// The extremes: every good capture at -bound still outranks killer0, the losing one at +bound
	// still ranks below killer1.
	for (const Move m : {pxq, nxr, MoveFormatter::FromUCI("d3c4", td->board)})
		td->capture_history_entry(m) = static_cast<int16_t>(-kMax);
	td->capture_history_entry(losing) = kMax;
	const Ordered o = order(*td, true);
	for (const Move m : {pxq, nxr, MoveFormatter::FromUCI("d3c4", td->board)})
		CHECK(o.score_of(m) >= MoveSorter::kGoodCaptureTier);
	CHECK(o.score_of(losing) >= MoveSorter::kLosingCaptureTier);
	CHECK(o.score_of(losing) < MoveSorter::kKiller1Score);
}

TEST_CASE("Capture history - off, a search leaves the table untouched", "[search][history]")
{
	constexpr const char* fen = "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4";
	AIPerlexTestFixture off(fen);
	off.set_capture_history(false);
	off.get_move_at_threads(1, 6);
	CHECK(off.capture_history_range() == std::pair<int16_t, int16_t>{0, 0});

	// A fresh fixture, because a repeat search on a warm transposition table reaches almost no cutoffs.
	AIPerlexTestFixture on(fen);
	on.set_capture_history(true);
	on.get_move_at_threads(1, 6);
	const auto [lo, hi] = on.capture_history_range();
	CHECK(lo < 0);
	CHECK(hi > 0);
	CHECK(lo >= -kMax);
	CHECK(hi <= kMax);
}

TEST_CASE("Capture history - in-check quiescence ignores a fed table", "[search][history][qsearch]")
{
	// Re2 checks; Nxe2 outranks Kxe2 by MVV-LVA, so a table read would let a fed Kxe2 overtake it.
	AIPerlexTestFixture fix("4k3/8/8/8/8/2N5/4r3/Q3K3 w - - 0 1");
	REQUIRE(fix.board_.InCheck());
	fix.set_capture_history(true);

	const std::vector<std::string> empty = fix.quiescence_order();
	REQUIRE(empty.front() == "c3e2");

	fix.capture_history_entry("e1e2") = kMax;
	CHECK(fix.quiescence_order() == empty);
}
