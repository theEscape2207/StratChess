// UCIEvalTests.cpp — Catch2 [uci] tests for cmd_eval: the static evaluation and its per-term breakdown.
// Shared infrastructure is in UCITestFixture.h; session/administrative commands are in UCITests.cpp.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "UCITestFixture.h"
#include "Board.h"
#include "Eval.h"

#include <algorithm>
#include <sstream>
#include <string>

// ---------------------------------------------------------------------------
// cmd_eval — static evaluation introspection
// ---------------------------------------------------------------------------

// Parses the integer centipawn value out of a "<label><N> cp" line, e.g.
// label="static eval: " on "static eval: 34 cp (White to move; ...)".
// A real parse of the emitted number, not a substring check — this is what
// makes the honesty-invariant test below actually load-bearing.
//
// The label is matched at the start of a line, not anywhere in the output.
// The breakdown prints a "sum (white pov)" line above "white pov:", and
// the two are distinguished today only by the trailing colon: an unanchored
// find() would start silently reading the wrong line the moment anyone added
// one — turning the sign-convention assertions below into confident nonsense
// rather than a failure.
static int extract_cp_score(const std::string& output, const std::string& label)
{
	const std::string anchored = "\n" + output;
	const auto label_pos = anchored.find("\n" + label);
	REQUIRE(label_pos != std::string::npos);

	const auto value_start = label_pos + 1 + label.size();
	const auto value_end = anchored.find(" cp", value_start);
	REQUIRE(value_end != std::string::npos);
	return std::stoi(anchored.substr(value_start, value_end - value_start));
}

TEST_CASE("cmd_eval: before position matches the starting position evaluator score", "[uci]")
{
	const std::string starting_fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
	UciHandlerTestFixture fix;

	const std::string out = fix.capture([&] { fix.eval(); });

	const Evaluator eval;
	const Board starting_board(starting_fen);
	REQUIRE(extract_cp_score(out, "static eval:") == eval.Evaluate(starting_board));
}

TEST_CASE("cmd_eval: printed score matches Evaluator::Evaluate() directly (honesty invariant)", "[uci]")
{
	// The property that makes this tool trustworthy for Texel tuning
	// and for a byte-identity check across an eval refactor: 'eval' must
	// never compute its own parallel score, only report the same Evaluate()
	// the search calls.
	const std::string fen = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"; // Kiwipete

	UciHandlerTestFixture fix;
	fix.position("position fen " + fen);

	const std::string out = fix.capture([&] { fix.eval(); });
	const int printed = extract_cp_score(out, "static eval:");

	const Evaluator eval;
	Board board(fen);
	const int expected = eval.Evaluate(board);

	REQUIRE(printed == expected);
}

TEST_CASE("cmd_eval: output contains neither bestmove nor info", "[uci]")
{
	// 'eval' is not a search response — a GUI must not mistake it for one.
	UciHandlerTestFixture fix;
	fix.position("position fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

	const std::string out = fix.capture([&] { fix.eval(); });

	REQUIRE(out.find("bestmove") == std::string::npos);
	REQUIRE(out.find("info") == std::string::npos);
}

TEST_CASE("cmd_eval: white-pov line matches the stated sign convention", "[uci]")
{
	// Both positions have a large, unambiguous material imbalance (a whole
	// rook) so a sign bug cannot hide behind a near-zero score.
	UciHandlerTestFixture fix;

	SECTION("White to move — white pov equals the side-to-move score")
	{
		fix.position("position fen 4k3/8/8/8/8/8/8/R3K3 w - - 0 1"); // White up a rook

		const std::string out = fix.capture([&] { fix.eval(); });

		const int side_to_move_score = extract_cp_score(out, "static eval:");
		const int white_pov = extract_cp_score(out, "white pov:");

		REQUIRE(side_to_move_score != 0);
		REQUIRE(white_pov == side_to_move_score);
	}

	SECTION("Black to move — white pov is the negation of the side-to-move score")
	{
		fix.position("position fen r3k3/8/8/8/8/8/8/4K3 b - - 0 1"); // Black up a rook

		const std::string out = fix.capture([&] { fix.eval(); });

		const int side_to_move_score = extract_cp_score(out, "static eval:");
		const int white_pov = extract_cp_score(out, "white pov:");

		REQUIRE(side_to_move_score != 0);
		REQUIRE(white_pov == -side_to_move_score);
	}
}

// ---------------------------------------------------------------------------
// cmd_eval — per-term breakdown
// ---------------------------------------------------------------------------
//
// These assert on the *printed* table rather than on EvalBreakdown directly.
// A breakdown that is right internally and mis-rendered is still a debugging
// tool that lies, and Texel tuning will be reading the output, not the
// struct. The struct-level check that the rows really are the same terms
// Evaluator::Evaluate() sums lives in EvalTermTests.cpp ([eval]).

struct EvalTermRow {
	int white;
	int black;
	int net;
};

// Pulls one "<term> | <white> | <black> | <net>" row out of the printed table.
// Matches on the first whitespace-delimited token so column padding is not
// baked into the test — a change to the column widths must not silently turn
// these assertions into no-ops.
static EvalTermRow extract_term_row(const std::string& output, const std::string& term)
{
	EvalTermRow row{};
	bool found = false;

	for (const std::string& line : split_lines(output)) {
		std::istringstream head(line);
		std::string first;
		if (!(head >> first) || first != term)
			continue;

		const auto bar = line.find('|');
		REQUIRE(bar != std::string::npos);
		std::string values = line.substr(bar + 1);
		std::replace(values.begin(), values.end(), '|', ' ');

		std::istringstream parsed(values);
		REQUIRE(static_cast<bool>(parsed >> row.white >> row.black >> row.net));
		found = true;
		break;
	}

	REQUIRE(found);
	return row;
}

// The "endgame" row, which prints a net figure only — the scale acts on the
// white-minus-black score, so its per-color columns are dashes and the row
// parser above cannot read it. Lines without a column separator are skipped so
// the "endgame scale:" line below the table cannot be mistaken for the row.
static int extract_endgame_net(const std::string& output)
{
	for (const std::string& line : split_lines(output)) {
		std::istringstream head(line);
		std::string first;
		if (!(head >> first) || first != "endgame" || line.find('|') == std::string::npos)
			continue;

		const auto last_bar = line.rfind('|');
		return std::stoi(line.substr(last_bar + 1));
	}

	FAIL("the printed breakdown has no endgame row");
	return 0;
}

// The "sum (white pov)" line: the label followed by the right-aligned figure.
static int extract_sum_white_pov(const std::string& output)
{
	static const std::string kLabel = "sum (white pov)";
	const auto label_pos = output.find(kLabel);
	REQUIRE(label_pos != std::string::npos);

	const auto line_end = output.find('\n', label_pos);
	const auto value_start = label_pos + kLabel.size();
	const std::string value = (line_end != std::string::npos) ? output.substr(value_start, line_end - value_start)
	                                                          : output.substr(value_start);

	return std::stoi(value);
}

// The endgame row is absent here on purpose: it is net-only and is added to the
// sum through extract_endgame_net above.
TEST_CASE("cmd_eval: printed breakdown nets are white-minus-black and sum to the evaluator's score", "[uci]")
{
	// The phase 2 honesty invariant (D9). Three separate claims, each of which
	// has failed in some engine at some point: each row's net is consistent
	// with its own two columns; the net column sums to the printed total; and
	// that total is the score the search would actually see, computed here
	// from a fresh Board through a separate Evaluator instance.
	const char* fen =
	    GENERATE("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", // Kiwipete, middlegame
	             "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",             // startpos, symmetric
	             "8/8/8/3r4/4k3/8/8/3QK3 w - - 0 1",                                     // endgame, White to move
	             "r3k3/8/8/8/8/8/8/4K3 b - - 0 1",  // Black to move, Black up a rook
	             "8/8/8/4k3/8/8/8/3QK3 w - - 0 1",  // pawnless K+Q vs K — mop-up active
	             "8/8/8/3k4/8/8/3N4/3K4 w - - 0 1", // K+N vs K — scaled to a draw
	             // A pawn-supported White knight on d5 that no Black pawn can
	             // challenge: the outposts row is zero in every case above, which
	             // would leave the sum invariant unable to see it.
	             "4k3/p6p/8/3N4/2P5/8/8/4K3 w - - 0 1");
	CAPTURE(fen);

	UciHandlerTestFixture fix;
	fix.position(std::string("position fen ") + fen);

	const std::string out = fix.capture([&] { fix.eval(); });

	int net_sum = 0;
	for (const EvalTermEntry& entry : EVAL_TERMS) {
		CAPTURE(entry.name);
		const EvalTermRow row = extract_term_row(out, entry.name);
		REQUIRE(row.net == row.white - row.black);
		net_sum += row.net;
	}
	net_sum += extract_endgame_net(out);

	REQUIRE(net_sum == extract_sum_white_pov(out));
	REQUIRE(net_sum == extract_cp_score(out, "white pov:"));

	const Evaluator eval;
	Board board(fen);
	const int side_to_move_score = eval.Evaluate(board);
	const int expected_white_pov = (board.GetCurrentColor() == WHITE) ? side_to_move_score : -side_to_move_score;

	REQUIRE(net_sum == expected_white_pov);
}

TEST_CASE("cmd_eval: breakdown reports the game phase the evaluator computed", "[uci]")
{
	// Phase is printed because it is not derivable from the rows, yet it sets
	// where between the mg and eg endpoints every tapered term landed — so a
	// reader debugging the pst row needs it to interpret that row at all
	// (in place of a middlegame/endgame stage name).
	UciHandlerTestFixture fix;

	SECTION("full starting material is the maximum phase")
	{
		fix.position("position startpos");

		REQUIRE(fix.capture([&] { fix.eval(); }).find("phase: 24/24") != std::string::npos);
	}

	SECTION("bare kings plus a queen is deep in the endgame")
	{
		fix.position("position fen 4k3/8/8/8/8/8/8/3QK3 w - - 0 1");

		REQUIRE(fix.capture([&] { fix.eval(); }).find("phase: 4/24") != std::string::npos);
	}
}

TEST_CASE("cmd_eval: a term that is active for exactly one side shows it in the per-color split", "[uci]")
{
	// The per-color columns are the reason for the table's shape (D10): a net
	// of -30 does not say whether the pawn term is penalising White or
	// rewarding Black. This pins that the columns carry that information
	// rather than both being derived from the net.
	//
	// Pawnless K+Q vs K, White winning decisively: eval_mopup is gated on a
	// 400 cp material lead, so it is active for White and inert for Black.
	//
	// The losing king is cornered on a8 rather than centralised, so both of
	// eval_mopup's components contribute. With the king on e5 its
	// center-manhattan-distance is 0 and the entire MOPUP_CMD_WEIGHT term
	// drops out — the assertion would then rest solely on the king-distance
	// component, and would still pass with MOPUP_CMD_WEIGHT zeroed.
	UciHandlerTestFixture fix;
	fix.position("position fen k7/8/8/8/8/8/8/3QK3 w - - 0 1");

	const EvalTermRow mopup = extract_term_row(fix.capture([&] { fix.eval(); }), "mopup");

	REQUIRE(mopup.white > 0);
	REQUIRE(mopup.black == 0);
}
