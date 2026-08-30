// QuiescenceTests.cpp — Catch2 tests for quiescence(): the bound delta pruning is allowed to
// use, the legal evasions a node in check has to consider, the unit its TT depth field is
// expressed in, and the order it searches its moves in.

#include "SearchTestFixture.h"
#include <catch2/catch_test_macros.hpp>
#include "MoveFormatter.h"
#include "MoveHelper.h"
#include "TranspositionTable.h"
#include "defines.h"
#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// Delta pruning bound
// ============================================================================
// Delta pruning needs an OPTIMISTIC bound on what a move can win. MoveHelper::Value
// is MVV-LVA — an ordering heuristic that subtracts a sixteenth of the moving piece —
// so it understates the gain and is not usable as a bound. For a king (10 000) that
// subtraction is 625, which turns a won pawn into -525 and discards the capture.
// Officer and king captures only became reachable in quiescence with #306, which is
// what exposed this.

TEST_CASE("Qsearch - delta pruning keeps a king capture that wins a pawn", "[search][qsearch]")
{
	// Kxd2 wins an undefended pawn. It is the only capture available, and the rooks
	// keep the position clear of insufficient-material handling so the gain shows up.
	AIPerlexTestFixture fix("7k/8/8/8/r7/8/3p4/3KR3 w - - 0 1");
	REQUIRE_FALSE(fix.board_.InCheck());

	const int stand_pat = fix.evaluate();
	const int score = fix.quiesce_node(-GameValues::Search_Init, GameValues::Search_Init,
	                                   AIPerlexTestFixture::QSEARCH_BUDGET, /*ply=*/0);

	// Standing pat is always available, so the node can never score below it. Scoring
	// exactly it means the only capture was pruned before it was ever searched.
	INFO("stand_pat = " << stand_pat << ", qsearch = " << score);
	CHECK(score > stand_pat);
}

TEST_CASE("MoveHelper - DeltaGain bounds the material a move can win", "[search][qsearch]")
{
	Board board("7k/8/8/8/r7/8/3p4/3KR3 w - - 0 1");
	const Move kingTakesPawn = MoveFormatter::FromUCI("d1d2", board);
	// The bound is the pawn itself, not the pawn minus a sixteenth of the king.
	CHECK(MoveHelper::DeltaGain(kingTakesPawn, board.GetEffectiveMovPiece(kingTakesPawn),
	                            board.GetCapturedPiece(kingTakesPawn)) == 100);

	// A promotion is worth the piece it becomes less the pawn it consumes, and a
	// capture-promotion adds the captured piece on top.
	Board promo("r3k3/1P6/8/8/8/8/8/4K3 w - - 0 1");
	const Move queenPromo = MoveFormatter::FromUCI("b7b8q", promo);
	CHECK(MoveHelper::DeltaGain(queenPromo, promo.GetEffectiveMovPiece(queenPromo),
	                            promo.GetCapturedPiece(queenPromo)) == 800);

	// 200, not the ">= 800" the old delta-pruning comment claimed for all promotions.
	const Move knightPromo = MoveFormatter::FromUCI("b7b8n", promo);
	CHECK(MoveHelper::DeltaGain(knightPromo, promo.GetEffectiveMovPiece(knightPromo),
	                            promo.GetCapturedPiece(knightPromo)) == 200);

	const Move queenPromoCapture = MoveFormatter::FromUCI("b7a8q", promo);
	CHECK(MoveHelper::DeltaGain(queenPromoCapture, promo.GetEffectiveMovPiece(queenPromoCapture),
	                            promo.GetCapturedPiece(queenPromoCapture)) == 800 + 500);

	// A quiet move wins nothing.
	const Move quiet = MoveFormatter::FromUCI("e1e2", promo);
	CHECK(MoveHelper::DeltaGain(quiet, promo.GetEffectiveMovPiece(quiet), promo.GetCapturedPiece(quiet)) == 0);
}

// ============================================================================
// Legal quiescence while in check
// ============================================================================
// A side to move in check may not decline to move, so quiescence cannot settle
// such a node by standing pat, and a capture-only move list cannot answer it:
// blocks and king walks are evasions too. Each case below is a position where
// the capture-only, stand-pat-first version returns a different answer.

TEST_CASE("Qsearch - in check, stand-pat cannot cut off and mate is seen", "[search][qsearch]")
{
	// White is two knights up and in check from Ra1; f2/g2/h2 are its own pawns and
	// f1/h1 are covered along the rank, so it is mate. Neither knight can reach the
	// first rank. Standing pat here returns a winning score for a lost position.
	AIPerlexTestFixture fix("7k/8/8/NN6/8/8/5PPP/r5K1 w - - 0 1");
	REQUIRE(fix.board_.InCheck());
	REQUIRE(fix.count_legal_moves() == 0);

	constexpr int beta = 100;
	// The precondition that makes this test meaningful: stand-pat would have cut off.
	REQUIRE(fix.evaluate() >= beta);

	const int ply = 3;
	CHECK(fix.quiesce_node(-GameValues::Search_Init, beta, AIPerlexTestFixture::QSEARCH_BUDGET, ply) ==
	      -GameValues::Mate + ply);
}

// The assertion that carries these three is the TT's stored best move: it names the
// evasion the node actually searched. "Not mate" alone is not enough — the capture-only
// version returns stand-pat here, which is also not mate, so such a test passes on the
// very behaviour it is meant to reject.

TEST_CASE("Qsearch - in check, a quiet king evasion is found", "[search][qsearch]")
{
	// Ra1 checks along the rank; every escape is a quiet king step off it, and no
	// capture exists. A capture-only generator sees an empty list here.
	AIPerlexTestFixture fix("7k/8/8/8/8/8/8/r5K1 w - - 0 1");
	REQUIRE(fix.board_.InCheck());
	REQUIRE(fix.count_legal_moves() > 0);

	const int ply = 2;
	const int score =
	    fix.quiesce_node(-GameValues::Search_Init, GameValues::Search_Init, AIPerlexTestFixture::QSEARCH_BUDGET, ply);
	CHECK(score > -GameValues::Mate_Threshold);

	const auto entry = fix.probe_tt(ply);
	REQUIRE(entry.has_value());
	REQUIRE_FALSE(entry->best_move.is_null());
	// Only the king can move, so whichever escape was chosen must start on g1.
	CHECK(entry->best_move.from() == g1);
}

TEST_CASE("Qsearch - in check, a quiet blocking evasion is found and refuted", "[search][qsearch]")
{
	// Ra1 checks along the rank, the king is walled in by its own pawns, and the only
	// legal reply is the quiet interposition Rd7-d1 — a move no capture-only generator
	// produces. The block does not save the game: Rxd1 renews the check with nothing left
	// to interpose. Both halves of that line are needed to see it, so this case fails
	// under either change alone — a capture-only evasion list never finds Rd1, and a
	// pawn-only capture list never finds the officer recapture that mates.
	AIPerlexTestFixture fix("7k/3R4/8/8/8/8/5PPP/r5K1 w - - 0 1");
	REQUIRE(fix.board_.InCheck());
	REQUIRE(fix.count_legal_moves() == 1);

	const int ply = 2;
	const int score =
	    fix.quiesce_node(-GameValues::Search_Init, GameValues::Search_Init, AIPerlexTestFixture::QSEARCH_BUDGET, ply);
	CHECK(score == -GameValues::Mate + ply + 2);

	const auto entry = fix.probe_tt(ply);
	REQUIRE(entry.has_value());
	REQUIRE_FALSE(entry->best_move.is_null());
	CHECK(MoveFormatter::ToUCI(entry->best_move) == "d7d1");
}

TEST_CASE("Qsearch - in check, a capturing evasion is still found", "[search][qsearch]")
{
	// Ne2 checks the boxed-in king and a knight check cannot be blocked, so Re7xe2 is
	// the only legal reply. This is the one evasion shape the old generator could see;
	// it must survive the switch to a full evasion list.
	AIPerlexTestFixture fix("7k/4R3/8/8/8/8/4nPPP/5RKR w - - 0 1");
	REQUIRE(fix.board_.InCheck());
	REQUIRE(fix.count_legal_moves() == 1);

	const int ply = 2;
	const int score =
	    fix.quiesce_node(-GameValues::Search_Init, GameValues::Search_Init, AIPerlexTestFixture::QSEARCH_BUDGET, ply);
	CHECK(score > -GameValues::Mate_Threshold);

	const auto entry = fix.probe_tt(ply);
	REQUIRE(entry.has_value());
	REQUIRE_FALSE(entry->best_move.is_null());
	CHECK(MoveFormatter::ToUCI(entry->best_move) == "e7e2");
}

TEST_CASE("Qsearch - an in-check line that repeats the root scores as a draw", "[search][qsearch][repetition]")
{
	// The termination case the in-check path introduced. White's king is checked along the
	// rank, steps off it, and the rook re-checks on the next rank — a quiet evasion answered
	// by a quiet check, with no capture anywhere. Four plies later the position is the root
	// again, and nothing about the material has changed, so this can go on forever.
	//
	// Removing either the FEN-setup seeding of the repetition history or quiescence's
	// check_draws call makes this return -572 — the static evaluation of a position that is
	// still in check, which is exactly the failure mode being guarded against.
	//
	// It does NOT pin the third part, widening the in-search bound to admit the root: with
	// that reverted the same draw is found one cycle deeper, between two in-search
	// positions, and the score is still 0. That half is covered by the tactical suite
	// instead, where without it the engine shuffles a knight rather than winning a queen
	// (WAC-008, 35/36).
	AIPerlexTestFixture fix("7k/8/8/8/8/8/8/r6K w - - 0 1");
	REQUIRE(fix.board_.InCheck());

	const int score =
	    fix.quiesce_after({"h1h2", "a1a2", "h2h1", "a2a1"}, -GameValues::Search_Init, GameValues::Search_Init);
	CHECK(score == GameValues::Draw);
}

TEST_CASE("Qsearch - out of check, the stand-pat cutoff is unchanged", "[search][qsearch]")
{
	// The untouched path: quiet position, evaluation above beta, so the node stands pat
	// and returns beta without generating anything. The queen sits on b1, not a1: from a1
	// it would check the black king on h8, and a FEN whose side-not-to-move is in check is
	// rejected outright by Board::setup_from_fen_impl, leaving an empty board behind.
	AIPerlexTestFixture fix("7k/8/8/8/8/8/8/1Q4K1 w - - 0 1");
	REQUIRE_FALSE(fix.board_.InCheck());

	constexpr int beta = 100;
	REQUIRE(fix.evaluate() >= beta);

	CHECK(fix.quiesce_node(-GameValues::Search_Init, beta, AIPerlexTestFixture::QSEARCH_BUDGET, /*ply=*/0) == beta);
}

// ============================================================================
// Quiescence TT depth: remaining budget, not plies consumed
// ============================================================================
// The transposition table's depth field means "search still to come" on the main
// path, and quiescence now writes the same unit. Both tests enter through pvs() so
// that the budget is the one the search hands out: with plies counted as consumed
// instead, the first would see 0 stored at a fresh node, and the second would reuse
// an entry produced with almost no search left.

TEST_CASE("Qsearch - a fresh node stores its remaining budget as the entry depth", "[search][qsearch][tt]")
{
	// A quiet position, so the node stands pat and stores without recursing: the entry's
	// depth is then exactly the budget pvs() handed the node.
	AIPerlexTestFixture fix("7k/8/8/8/8/8/8/1Q4K1 w - - 0 1");

	fix.quiesce_via_pvs(-GameValues::Search_Init, GameValues::Search_Init, /*ply=*/0);

	const auto entry = fix.probe_tt(/*ply=*/0);
	REQUIRE(entry.has_value());
	CHECK(entry->phase == SearchPhase::QUIESCENCE);
	CHECK(entry->depth == AIPerlexTestFixture::QSEARCH_BUDGET);
}

TEST_CASE("Qsearch - an entry with less remaining budget does not satisfy a fresh node", "[search][qsearch][tt]")
{
	// A value no evaluation of this position could produce, so returning it proves the entry
	// was reused rather than the node re-searched.
	constexpr int16_t planted = 12'345;
	const std::string quiet_position = "7k/8/8/8/8/8/8/1Q4K1 w - - 0 1";

	SECTION("insufficient entry is ignored")
	{
		AIPerlexTestFixture fix(quiet_position);
		fix.store_qsearch_entry(planted, /*qsearch_budget=*/1, /*ply=*/0);

		const int score = fix.quiesce_via_pvs(-GameValues::Search_Init, GameValues::Search_Init, /*ply=*/0);

		CHECK(score != planted);
	}

	SECTION("sufficient entry is reused")
	{
		AIPerlexTestFixture fix(quiet_position);
		fix.store_qsearch_entry(planted, AIPerlexTestFixture::QSEARCH_BUDGET, /*ply=*/0);

		const int score = fix.quiesce_via_pvs(-GameValues::Search_Init, GameValues::Search_Init, /*ply=*/0);

		CHECK(score == planted);
	}

	SECTION("an entry from an exhausted in-check chain is ignored")
	{
		// In check the budget is bypassed, so a chain of evasions drives it negative and the
		// entries it stores carry negative depths. This pins the contract for that range
		// rather than reproducing a past defect: under plies-consumed those entries carried
		// the *largest* depths in the table, so the situation this rules out could not arise
		// in the same shape, and reverting the unit does not fail this section.
		AIPerlexTestFixture fix(quiet_position);
		fix.store_qsearch_entry(planted, /*qsearch_budget=*/-5, /*ply=*/0);

		const int score = fix.quiesce_via_pvs(-GameValues::Search_Init, GameValues::Search_Init, /*ply=*/0);

		CHECK(score != planted);
	}
}

TEST_CASE("Qsearch - the root of a check chain records its full budget", "[search][qsearch][tt]")
{
	// A lone rook checking a cornered king: every evasion is quiet, so the chain runs on the
	// in-check path that ignores the budget. The chain's own entries carry negative budgets;
	// this pins the root, which is the only one of them reachable by key from here.
	AIPerlexTestFixture fix("7k/8/8/8/8/8/8/r6K w - - 0 1");
	REQUIRE(fix.board_.InCheck());

	fix.quiesce_via_pvs(-GameValues::Search_Init, GameValues::Search_Init, /*ply=*/0);

	const auto entry = fix.probe_tt(/*ply=*/0);
	REQUIRE(entry.has_value());
	CHECK(entry->phase == SearchPhase::QUIESCENCE);
	// The root of the chain still holds its full budget; the entries below it are the negative
	// ones, and they are unreachable from here without walking the tree. What this pins is that
	// the root is not itself recorded as exhausted.
	CHECK(entry->depth == AIPerlexTestFixture::QSEARCH_BUDGET);
}

// --- Quiescence in-check ordering (#320) -------------------------------------------------
//
// Position: black to move, in check from Re1 down the open e-file. The evasions are
//   Qa5xe1          — a capture of the attacker
//   Qa5-e5          — an interposition, quiet
//   Ke8-d8/f8/d7/f7 — king evasions, quiet
//
// Under the capture-only MVV-LVA sort these tests replaced, a quiet move scored
// -PieceHelper::Value(mover) / 16. The king is worth 10000 (defines.h) against the queen's
// 900, so every king evasion scored -625 to the queen's -56 and sorted below it — the move
// most likely to be best searched last.
static constexpr const char* kRookChecksAlongOpenFile = "4k3/8/8/q7/8/8/8/4R1K1 b - - 0 1";

TEST_CASE("Search - in check, a king evasion with history outranks a quiet interposition", "[search][qsearch]")
{
	AIPerlexTestFixture fix(kRookChecksAlongOpenFile);
	REQUIRE(fix.board_.InCheck());

	// What a real beta cutoff on the king walk would have left in the history table.
	fix.seed_history("e8f7", /*depth=*/4);

	const std::vector<std::string> order = fix.quiescence_order();
	const auto king_walk = std::find(order.begin(), order.end(), "e8f7");
	const auto interposition = std::find(order.begin(), order.end(), "a5e5");

	REQUIRE(king_walk != order.end());
	REQUIRE(interposition != order.end());

	// The whole point of #320: history decides between two quiet evasions, so the king walk
	// is no longer sunk below the queen purely for being the heavier piece.
	CHECK(king_walk < interposition);
}

TEST_CASE("Search - in check, capturing the attacker is still ordered first", "[search][qsearch]")
{
	AIPerlexTestFixture fix(kRookChecksAlongOpenFile);
	REQUIRE(fix.board_.InCheck());

	// Seeded hard enough that a scorer consulting history for captures too would be caught.
	fix.seed_history("e8f7", /*depth=*/64);

	const std::vector<std::string> order = fix.quiescence_order();
	REQUIRE_FALSE(order.empty());

	// Winning captures outrank every quiet regardless of history — ScoreMoves scores them in
	// a tier above it, and history is only consulted on the quiet branch.
	CHECK(order.front() == "a5e1");
}

// A contact check: the checking piece stands next to the king, so the king can take it. A knight
// checker never can — a knight does not attack the squares adjacent to it — so this class is
// rook, bishop, queen and pawn only.
//
// Kxe2 wins a whole rook and is plainly best. It is also the case that exposes what the LVA proxy
// does to a king capture: MoveHelper::Value scores a capture as victim - mover/16, and the king is
// worth 10000, so Kxe2 scores 500 - 625 = -125 and lands in ScoreMoves' *losing* capture tier,
// below every quiet evasion (history is capped non-negative).
TEST_CASE("Search - in check, capturing a contact checker outranks fleeing", "[search][qsearch]")
{
	AIPerlexTestFixture fix("4k3/8/8/8/8/8/4r3/4K3 w - - 0 1");
	REQUIRE(fix.board_.InCheck());

	const std::vector<std::string> order = fix.quiescence_order();
	const auto capture = std::find(order.begin(), order.end(), "e1e2");
	REQUIRE(capture != order.end());

	// Taking the rook must be searched before walking away from it.
	CHECK(capture == order.begin());
}

// Move generation is pseudo-legal: GenerateOfficerMoves masks the king's destinations against own
// pieces only, so a king capture of a DEFENDED piece reaches the sorter and is rejected later by
// DoMove. Ordering it first would put a move that cannot be played at the head of every such node.
//
// Ke1 is in check from Re2, which the d3 pawn defends, so Kxe2 is illegal. Rxe2 is legal and is the
// move to search first. Capped, Kxe2 scores 500 - 56 = 444 against Rxe2's 500 - 31 = 469; uncapped
// it would score 500 and displace it. Together with the contact-check test above this pins the cap
// from both sides: that test fails if the king's LVA weight is left at 10000, this one fails if it
// is dropped to nothing.
TEST_CASE("Search - in check, a legal capture outranks an illegal king capture", "[search][qsearch]")
{
	AIPerlexTestFixture fix("4k3/8/8/8/8/3p4/R3r3/4K3 w - - 0 1");
	REQUIRE(fix.board_.InCheck());

	const std::vector<std::string> order = fix.quiescence_order();

	// The illegal king capture really is in the list — otherwise this test proves nothing.
	REQUIRE(std::find(order.begin(), order.end(), "e1e2") != order.end());

	CHECK(order.front() == "a2e2");
}

TEST_CASE("Search - in check, SEE pruning cannot prune the only evasion", "[search][qsearch][see]")
{
	// White is in check from Ng3 and Rxg3 is the ONLY legal move. It is also a losing capture --
	// hxg3 recaptures, so SEE is 300 - 500 = -200 -- which is exactly the shape the out-of-check
	// prune discards. Pruning it here would leave no survivor, and an empty survivor set is what
	// quiescence reads as checkmate: the node would store a fabricated mate score EXACT into the
	// table. The `!in_check` term at the pruning site is the only thing preventing that, which is
	// why it is not behind see_pruning_enabled.
	AIPerlexTestFixture fix("1k6/8/8/8/3b3p/6n1/r7/6RK w - - 0 1");
	REQUIRE(fix.board_.InCheck());
	REQUIRE(fix.count_legal_moves() == 1);
	fix.set_see_pruning(true);

	const int score = fix.quiesce_node(-GameValues::Mate, GameValues::Mate, AIPerlexTestFixture::QSEARCH_BUDGET, 0);

	CHECK(score > -GameValues::Mate_Threshold);
}

TEST_CASE("Search - see_pruning_enabled actually reaches the quiescence loop", "[search][qsearch][see]")
{
	// Guards against the flag becoming dead code: a position whose only capture plainly loses
	// material must search fewer quiescence edges with pruning on. Rxd5 takes a defended knight
	// and exd5 recaptures, so SEE is 300 - 500 = -200. The defender has to be the e6 pawn: from
	// e7 it attacks d6 and f6, and the knight would be hanging rather than defended.
	const char* fen = "4k3/8/4p3/3n4/8/8/8/3RK3 w - - 0 1";

	AIPerlexTestFixture off(fen);
	off.set_see_pruning(false);
	off.quiesce_node(-GameValues::Mate, GameValues::Mate, AIPerlexTestFixture::QSEARCH_BUDGET, 0);

	AIPerlexTestFixture on(fen);
	on.set_see_pruning(true);
	on.quiesce_node(-GameValues::Mate, GameValues::Mate, AIPerlexTestFixture::QSEARCH_BUDGET, 0);

	CHECK(on.qnodes() < off.qnodes());
}

TEST_CASE("Search - out of check, quiescence still orders captures by MVV-LVA", "[search][qsearch]")
{
	// White to move, not in check. Two captures of the d5 pawn are available: exd5 takes with
	// a pawn, Qxd5 with the queen. MVV-LVA prefers the cheaper attacker for the same victim.
	AIPerlexTestFixture fix("4k3/8/8/3p4/4P3/8/8/3QK3 w - - 0 1");
	REQUIRE_FALSE(fix.board_.InCheck());

	const std::vector<std::string> order = fix.quiescence_order();
	REQUIRE(order.size() >= 2);
	CHECK(order.front() == "e4d5");
}
