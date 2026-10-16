// EvalPawnCacheTests.cpp — the pawn-structure cache (#837) never changes a score.
//
// Evaluate(board, cache) must equal Evaluate(board) for every position and every prior cache
// content. The corpus walk is the broad check; the directed cases pin the state transitions it
// reaches only by luck: blockade on and off under one tag, one king moving, phase 0, a missing
// king, and a pawn-tag replacement that must not let an old cover survive.

#include "EvalTestFixture.h"
#include "MoveGenerator.h"
#include "PawnCache.h"

#include <fstream>
#include <nlohmann/json.hpp>

namespace {

	// Every node of the depth-limited tree, root included, through one shared cache.
	void WalkAndCompare(Board& board, int depth, const Evaluator& eval, PawnCache& cache, int& checked)
	{
		const int cached = eval.Evaluate(board, cache);
		const int uncached = eval.Evaluate(board);
		++checked;
		if (cached != uncached)
			FAIL_CHECK(board.ExtractFEN() << ": cached " << cached << " != uncached " << uncached);
		if (depth == 0)
			return;

		MoveList moves;
		if (board.InCheck())
			MoveGenerator::ComputeEvasions(board, moves);
		else
			MoveGenerator::ComputeLegalMoves(board, moves);
		for (const auto& move : moves) {
			if (!board.DoMove(move))
				continue;
			WalkAndCompare(board, depth - 1, eval, cache, checked);
			board.UndoMove(move);
		}
	}

	std::vector<std::string> PerftFens()
	{
		std::ifstream file(STRAT_TESTS_DATA_DIR "/perft_test_cases.json");
		REQUIRE(file.is_open());
		const auto data = nlohmann::json::parse(file);
		std::vector<std::string> fens;
		for (const auto& tc : data.at("perft_test_cases"))
			fens.push_back(tc.at("fen").get<std::string>());
		return fens;
	}

	// The walk above over every corpus root, at both sizes. 1 entry overwrites on every structure
	// change, so the same walk also exercises replacement on nearly every node.
	void CompareOverCorpus(int depth, int minPositions)
	{
		const std::size_t entries = GENERATE(std::size_t{4096}, std::size_t{1});
		CAPTURE(entries);
		const Evaluator eval;
		PawnCache cache(entries);
		int checked = 0;
		for (const std::string& fen : PerftFens()) {
			Board board(fen);
			WalkAndCompare(board, depth, eval, cache, checked);
		}
		REQUIRE(checked > minPositions);
	}

	PawnCache::Entry& SlotOf(PawnCache& cache, const Board& board)
	{
		const auto boards = board.GetBitBoards();
		return cache.Slot(boards[WHITE_PAWN], boards[BLACK_PAWN]);
	}

} // namespace

TEST_CASE("PawnCache - cached equals uncached over the perft corpus at depth 3", "[eval][pawncache][slow]")
{
	CompareOverCorpus(3, 600000);
}

TEST_CASE("PawnCache - cached equals uncached over the perft corpus at depth 2", "[eval][pawncache]")
{
	// The fast-tier slice of the case above.
	CompareOverCorpus(2, 10000);
}

TEST_CASE("PawnCache - a blockader moving onto and off the stop square is followed under one tag", "[eval][pawncache]")
{
	// Same pawns in both: White's d5 passer, blockaded by the knight on d6 or not. The pawn
	// tag matches, so only the replay can tell the two apart.
	const Board blocked("4k3/p6p/3n4/3P4/8/8/7P/R3K3 w - - 0 1");
	const Board free("4k3/p6p/n7/3P4/8/8/7P/R3K3 w - - 0 1");
	REQUIRE(EvaluatorTestFixture::Pawns(blocked, WHITE) != EvaluatorTestFixture::Pawns(free, WHITE));

	// The colour mirror pins Black's stop-square direction, which cached == uncached cannot:
	// both paths share ApplyBlockade.
	const Board blockedBlack("r3k3/7p/8/8/3p4/3N4/P6P/4K3 b - - 0 1");
	const Board freeBlack("r3k3/7p/8/8/3p4/N7/P6P/4K3 b - - 0 1");
	REQUIRE(EvaluatorTestFixture::Pawns(blockedBlack, BLACK) == EvaluatorTestFixture::Pawns(blocked, WHITE));
	REQUIRE(EvaluatorTestFixture::Pawns(freeBlack, BLACK) == EvaluatorTestFixture::Pawns(free, WHITE));

	const Evaluator eval;
	PawnCache cache;
	for (const Board* board : {&free, &blocked, &free, &blocked, &freeBlack, &blockedBlack, &freeBlack}) {
		CAPTURE(board->ExtractFEN());
		REQUIRE(eval.Evaluate(*board, cache) == eval.Evaluate(*board));
	}
}

TEST_CASE("PawnCache - one king moving rewrites only that colour's cover", "[eval][pawncache]")
{
	const Board before("3q2k1/5ppp/8/8/8/8/5PPP/3Q2K1 w - - 0 1");
	const Board after("3q2k1/5ppp/8/8/8/8/5PPP/3Q1K2 w - - 0 1");
	// Kg1 and Kf1 anchor on different files, so the covers differ and a stale reuse would show.
	REQUIRE(EvaluatorTestFixture::KingShelter(before, WHITE) != EvaluatorTestFixture::KingShelter(after, WHITE));

	const Evaluator eval;
	PawnCache cache;
	REQUIRE(eval.Evaluate(before, cache) == eval.Evaluate(before));
	PawnCache::Entry& slot = SlotOf(cache, before);
	REQUIRE(slot.king_sq[WHITE] == g1);
	REQUIRE(slot.king_sq[BLACK] == g8);
	const KingPawnCover black = slot.Cover(BLACK);

	REQUIRE(eval.Evaluate(after, cache) == eval.Evaluate(after));
	REQUIRE(&SlotOf(cache, after) == &slot);
	REQUIRE(slot.king_sq[WHITE] == f1);
	REQUIRE(slot.king_sq[BLACK] == g8);
	REQUIRE(slot.Cover(BLACK).shelter.mg == black.shelter.mg);
	REQUIRE(slot.Cover(BLACK).storm.mg == black.storm.mg);
	REQUIRE(slot.Cover(BLACK).files.mg == black.files.mg);

	REQUIRE(eval.Evaluate(before, cache) == eval.Evaluate(before));

	// Same pawns and kings at another non-zero phase: the stored cover is reused as is.
	const Board fewerPieces("6k1/5ppp/8/8/8/8/5PPP/3Q2K1 w - - 0 1");
	REQUIRE(EvaluatorTestFixture::Phase(fewerPieces) != EvaluatorTestFixture::Phase(before));
	REQUIRE(eval.Evaluate(fewerPieces, cache) == eval.Evaluate(fewerPieces));
	REQUIRE(slot.king_sq[WHITE] == g1);
}

TEST_CASE("PawnCache - phase 0 serves the pawn part and leaves the covers empty", "[eval][pawncache]")
{
	const Board board("4k3/pp6/8/8/8/8/PP6/4K3 w - - 0 1");
	REQUIRE(EvaluatorTestFixture::Phase(board) == 0);

	const Evaluator eval;
	PawnCache cache;
	REQUIRE(eval.Evaluate(board, cache) == eval.Evaluate(board));
	const PawnCache::Entry& slot = SlotOf(cache, board);
	REQUIRE(slot.Matches(board.GetBitBoards()[WHITE_PAWN], board.GetBitBoards()[BLACK_PAWN]));
	REQUIRE(slot.king_sq[WHITE] == NO_SQUARE);
	REQUIRE(slot.king_sq[BLACK] == NO_SQUARE);
	REQUIRE(eval.Evaluate(board, cache) == eval.Evaluate(board));
}

TEST_CASE("PawnCache - a kingless board skips the cover and matches the direct terms", "[eval][pawncache]")
{
	// The FEN parser rejects a kingless position, so the default-constructed board is the one
	// that reaches the guard. Evaluate() settles it as drawn before any term runs, hence the
	// raw score. It is also the all-zero tag a fresh cache must not match.
	const Board empty;
	PawnCache cache(1);
	REQUIRE(EvaluatorTestFixture::RawWhitePov(empty, cache) == EvaluatorTestFixture::RawWhitePov(empty));
	REQUIRE(EvaluatorTestFixture::RawWhitePov(empty, cache) == EvaluatorTestFixture::RawWhitePov(empty));
	REQUIRE(SlotOf(cache, empty).Matches(0, 0));
	REQUIRE(SlotOf(cache, empty).king_sq[WHITE] == NO_SQUARE);
	REQUIRE(SlotOf(cache, empty).king_sq[BLACK] == NO_SQUARE);
}

TEST_CASE("PawnCache - a pawn-tag replacement empties the old structure's covers", "[eval][pawncache]")
{
	// One entry, kings fixed on g1/g8. P fills the cover; Q replaces the pawn part at phase 0,
	// where the cover fill is skipped; Q at non-zero phase must then compute, not serve P's.
	const Board p("3q2k1/5ppp/8/8/8/8/5PPP/3Q2K1 w - - 0 1");
	const Board qPhaseZero("6k1/5ppp/8/8/8/8/PPP5/6K1 w - - 0 1");
	const Board q("3q2k1/5ppp/8/8/8/8/PPP5/3Q2K1 w - - 0 1");
	REQUIRE(EvaluatorTestFixture::Phase(qPhaseZero) == 0);
	REQUIRE(EvaluatorTestFixture::KingShelter(p, WHITE) != EvaluatorTestFixture::KingShelter(q, WHITE));

	const Evaluator eval;
	PawnCache cache(1);
	REQUIRE(eval.Evaluate(p, cache) == eval.Evaluate(p));
	REQUIRE(eval.Evaluate(qPhaseZero, cache) == eval.Evaluate(qPhaseZero));
	REQUIRE(eval.Evaluate(q, cache) == eval.Evaluate(q));
}

TEST_CASE("PawnCache - a cold pawnless position is a miss, not the empty slot", "[eval][pawncache]")
{
	const Board board("4k3/8/8/8/8/8/8/R3K2r w - - 0 1");
	const Evaluator eval;
	PawnCache cache(1);
	REQUIRE_FALSE(SlotOf(cache, board).Matches(0, 0));
	REQUIRE(eval.Evaluate(board, cache) == eval.Evaluate(board));
	REQUIRE(SlotOf(cache, board).Matches(0, 0));
	REQUIRE(eval.Evaluate(board, cache) == eval.Evaluate(board));
}
