// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "Sort.h"

#include "Board.h"
#include "MoveHelper.h"
#include "SearchTelemetry.h"
#include "See.h"

#include <charconv>
#include <cstdio>

namespace {
	// Profile builds only: STRAT_PROFILE_TIEBREAK_SEED=N (1..2^32-1) breaks move-ordering ties by a seeded
	// hash of the move instead of generation order — a neutral ordering perturbation that measures how
	// much a profile screen moves with no real ordering change. Read once, before main().
	uint32_t read_profile_tie_break_seed()
	{
		uint32_t seed = 0;
		if constexpr (kSearchProfileCompiled) {
			const auto text = StratGetEnv("STRAT_PROFILE_TIEBREAK_SEED");
			if (!text || text->empty())
				return 0;
			const auto [end, ec] = std::from_chars(text->data(), text->data() + text->size(), seed);
			if (ec != std::errc{} || end != text->data() + text->size()) {
				std::fprintf(stderr, "STRAT_PROFILE_TIEBREAK_SEED must be an unsigned 32-bit integer, got '%s'\n",
				             text->c_str());
				std::exit(EXIT_FAILURE);
			}
			if (seed != 0)
				std::printf("info string tiebreak seed %u\n", seed);
		}
		return seed;
	}

	const uint32_t kProfileTieBreakSeed = read_profile_tie_break_seed();

	// Bijective in the move's 16 bits for a fixed seed, so distinct moves never tie.
	uint32_t tie_break_key(const Move& mv, uint32_t seed)
	{
		uint32_t x = static_cast<uint32_t>(mv.from() | (mv.to() << 6) | (mv.flags() << 12)) * 0x9E3779B1u ^ seed;
		x ^= x >> 16;
		x *= 0x85EBCA6Bu;
		x ^= x >> 13;
		x *= 0xC2B2AE35u;
		x ^= x >> 16;
		return x;
	}

	// The one order every ScoreMoves entry point produces: score descending, then generation order.
	// std::sort is not stable, and equal scores are common — an in-check quiescence node with a cold
	// history table scores every quiet evasion 0 — so without the index the whole tied block is
	// permuted arbitrarily, and differently across stdlib versions. A profile build's tie-break seed
	// replaces generation order; the shipping build folds it to 0. A strict total order on distinct
	// moves, so the first entry is unique and sorting the rest after it reproduces the full sort.
	auto score_order(const MoveList& moveList)
	{
		return [&moveList, seed = kSearchProfileCompiled ? kProfileTieBreakSeed : 0u](const std::pair<int, int>& a,
		                                                                              const std::pair<int, int>& b) {
			if (a.first != b.first)
				return a.first > b.first;
			if (seed != 0)
				return tie_break_key(moveList[a.second], seed) < tie_break_key(moveList[b.second], seed);
			return a.second < b.second;
		};
	}
} // namespace

void MoveSorter::SortMovesByValue(MoveList& moveList, const Board& board)
{
	// The list really must be captures and promotions only — see the declaration.
	assert(std::ranges::all_of(moveList,
	                           [](const Move& m) { return MoveHelper::IsCapture(m) || MoveHelper::IsPromote(m); }));

	const size_t n = moveList.size();
	if (n < 2)
		return;

	// MVV-LVA: captured piece value minus (moving piece value / 16).
	std::array<int, MoveList::MAX_MOVES> values;
	for (size_t i = 0; i < n; ++i)
		values[i] = MoveHelper::Value(moveList[i], board.GetEffectiveMovPiece(moveList[i]),
		                              board.GetCapturedPiece(moveList[i]));

	// A stable insertion sort: equal values are common, and std::sort leaves their order to the standard
	// library, which differs between libstdc++ and MSVC STL. std::stable_sort may heap-allocate per node.
	for (size_t i = 1; i < n; ++i) {
		const Move move = moveList[i];
		const int value = values[i];
		size_t j = i;
		for (; j > 0 && values[j - 1] < value; --j) {
			moveList[j] = moveList[j - 1];
			values[j] = values[j - 1];
		}
		moveList[j] = move;
		values[j] = value;
	}
}

// The hash move is the top tier. There is deliberately no separate tier above it for the
// previous iteration's principal variation: such a hint exists only along the PV, so it can be
// offered at one node per ply per iteration, and at those nodes it names the move the
// transposition table already names — the entry at a PV node is that node's own store from the
// previous iteration, which is where the hint would come from too. Where the table has nothing
// to offer there, it is because the entry was overwritten, not because the hint knew better,
// and the fix belongs in the table.
//
// This applies to interior PV nodes. Ordering the ROOT's moves by the previous iteration's
// scores is a separate question with a different answer available to it, and nothing here
// forecloses it.
void MoveSorter::ScoreUnordered(const MoveList& moveList, int n, const Board& board, eColor side, const Move& hash_move,
                                const Move& killer0, const Move& killer1, const int32_t (&history)[2][64][64],
                                std::array<std::pair<int, int>, MoveList::MAX_MOVES>& out_scored_idx,
                                ContinuationRows cont)
{
	assert(n >= 0 && n <= static_cast<int>(MoveList::MAX_MOVES));

	for (int i = 0; i < n; ++i) {
		const Move& mv = moveList[i];
		int s = 0;

		if (mv == hash_move) {
			s = 1'900'000;
		} else if (MoveHelper::IsCapture(mv) || MoveHelper::IsPromote(mv)) {
			// SEE picks the tier, MVV-LVA scores within it. Two tripwires, both measured:
			//
			// Demoting the losing tier below the quiets is the tempting change and costs tens of
			// percent in nodes. Captures are never LMR-reduced wherever they sit, so postponing
			// one saves no depth while lowering the move number — and weakening the reduction —
			// of every quiet it steps over. Killers may jump it; they are LMR-exempt already.
			//
			// The !IsCapture() short-circuit is what keeps promotions out of SEE, not an
			// optimisation: see_ge scores a queen promotion onto a defended square as losing,
			// when the pawn was promoting anyway. MoveHelper::Value() ranks promotions by
			// promotion gain, so under-promotions stay below a queen promotion unaided.
			const int mvv_lva = MoveHelper::Value(mv, board.GetEffectiveMovPiece(mv), board.GetCapturedPiece(mv));

			s = (!MoveHelper::IsCapture(mv) || See::see_ge(board, mv, 0)) ? 1'000'000 + mvv_lva
			                                                              : kLosingCaptureTier + mvv_lva;
		} else if (mv == killer0) {
			s = 900'000;
		} else if (mv == killer1) {
			s = 800'000;
		} else {
			assert(static_cast<int>(side) >= 0 && static_cast<int>(side) < 2);
			s = history[static_cast<int>(side)][mv.from()][mv.to()];
			// Each entry is bounded by HISTORY_MAX, so the sum stays below kLosingCaptureTier; ThreadData
			// asserts that.
			if (!cont.empty()) {
				const int col = QuietContinuationColumn(board, mv);
				if (cont.one_ply != nullptr)
					s += cont.one_ply[col];
				if (cont.two_ply != nullptr)
					s += cont.two_ply[col];
			}
		}
		out_scored_idx[i] = {s, i};
	}
}

void MoveSorter::ScoreMoves(const MoveList& moveList, int n, const Board& board, eColor side, const Move& hash_move,
                            const Move& killer0, const Move& killer1, const int32_t (&history)[2][64][64],
                            std::array<std::pair<int, int>, MoveList::MAX_MOVES>& out_scored_idx, ContinuationRows cont)
{
	ScoreUnordered(moveList, n, board, side, hash_move, killer0, killer1, history, out_scored_idx, cont);
	OrderRemaining(moveList, out_scored_idx, 0, n);
}

void MoveSorter::ScoreMovesBestFirst(const MoveList& moveList, int n, const Board& board, eColor side,
                                     const Move& hash_move, const Move& killer0, const Move& killer1,
                                     const int32_t (&history)[2][64][64],
                                     std::array<std::pair<int, int>, MoveList::MAX_MOVES>& out_scored_idx,
                                     ContinuationRows cont)
{
	ScoreUnordered(moveList, n, board, side, hash_move, killer0, killer1, history, out_scored_idx, cont);
	if (n < 2)
		return;
	const auto before = score_order(moveList);
	int best = 0;
	for (int i = 1; i < n; ++i) {
		if (before(out_scored_idx[i], out_scored_idx[best]))
			best = i;
	}
	std::swap(out_scored_idx[0], out_scored_idx[best]);
}

void MoveSorter::OrderRemaining(const MoveList& moveList,
                                std::array<std::pair<int, int>, MoveList::MAX_MOVES>& scored_idx, int first, int n)
{
	assert(first >= 0 && n <= static_cast<int>(MoveList::MAX_MOVES));
	if (n - first < 2)
		return;
	std::sort(scored_idx.begin() + first, scored_idx.begin() + n, score_order(moveList));
}

// A quiet's moving piece is the one on its from-square.
int MoveSorter::QuietContinuationColumn(const Board& board, const Move& quiet) noexcept
{
	const int col = continuation_index(board.GetPiece(quiet.from()), quiet.to());
	assert(col >= 0 && col < kPieceSquares);
	return col;
}
