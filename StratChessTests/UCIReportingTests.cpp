// UCIReportingTests.cpp — Catch2 [uci] tests for the commands that run search
// and report on it: cmd_eval, cmd_perft (+ 'go perft' via run()) and cmd_go.
// Session/administrative commands (parse_go, cmd_position, cmd_setoption,
// dispatch, command log) are in UCITests.cpp.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "UCITestFixture.h"
#include "Board.h"
#include "Eval.h"
#include "MoveFormatter.h"
#include "MoveGenerator.h"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// cmd_eval — static evaluation introspection (issue #129 phase 1)
// ---------------------------------------------------------------------------

// Parses the integer centipawn value out of a "<label><N> cp" line, e.g.
// label="static eval: " on "static eval: 34 cp (White to move; ...)".
// A real parse of the emitted number, not a substring check — this is what
// makes the honesty-invariant test below actually load-bearing.
//
// The label is matched at the start of a line, not anywhere in the output.
// Phase 2's breakdown prints a "sum (white pov)" line above "white pov:", and
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

TEST_CASE("cmd_eval: works before any position command, does not crash", "[uci]")
{
	UciHandlerTestFixture fix;
	CoutRedirect redirect;
	REQUIRE_NOTHROW(fix.eval());

	const std::string out = redirect.str();
	REQUIRE(out.find("static eval:") != std::string::npos);
	REQUIRE(out.find("white pov:") != std::string::npos);
}

TEST_CASE("cmd_eval: printed score matches EvalManager::Evaluate() directly (honesty invariant)", "[uci]")
{
	// The property that makes this tool trustworthy for #117 (Texel tuning)
	// and #127 (EvalContext restructure's byte-identity check): 'eval' must
	// never compute its own parallel score, only report the same Evaluate()
	// the search calls.
	const std::string fen = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"; // Kiwipete

	UciHandlerTestFixture fix;
	fix.position("position fen " + fen);

	CoutRedirect redirect;
	fix.eval();
	const int printed = extract_cp_score(redirect.str(), "static eval:");

	auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);
	Board board(fen);
	const int expected = eval->Evaluate(board);

	REQUIRE(printed == expected);
}

TEST_CASE("cmd_eval: output contains neither bestmove nor info", "[uci]")
{
	// 'eval' is not a search response — a GUI must not mistake it for one.
	UciHandlerTestFixture fix;
	fix.position("position fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

	CoutRedirect redirect;
	fix.eval();
	const std::string out = redirect.str();

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

		CoutRedirect redirect;
		fix.eval();
		const std::string out = redirect.str();

		const int side_to_move_score = extract_cp_score(out, "static eval:");
		const int white_pov = extract_cp_score(out, "white pov:");

		REQUIRE(side_to_move_score != 0);
		REQUIRE(white_pov == side_to_move_score);
	}

	SECTION("Black to move — white pov is the negation of the side-to-move score")
	{
		fix.position("position fen r3k3/8/8/8/8/8/8/4K3 b - - 0 1"); // Black up a rook

		CoutRedirect redirect;
		fix.eval();
		const std::string out = redirect.str();

		const int side_to_move_score = extract_cp_score(out, "static eval:");
		const int white_pov = extract_cp_score(out, "white pov:");

		REQUIRE(side_to_move_score != 0);
		REQUIRE(white_pov == -side_to_move_score);
	}
}

// ---------------------------------------------------------------------------
// cmd_eval — per-term breakdown (issue #129 phase 2)
// ---------------------------------------------------------------------------
//
// These assert on the *printed* table rather than on EvalBreakdown directly.
// A breakdown that is right internally and mis-rendered is still a debugging
// tool that lies, and #117 (Texel tuning) will be reading the output, not the
// struct. The struct-level check that the rows really are the same terms
// EvalComplex::Evaluate() sums lives in EvalTests.cpp ([eval]).

struct EvalTermRow {
	int white;
	int black;
	int net;
};

static std::vector<std::string> split_lines(const std::string& text)
{
	std::vector<std::string> lines;
	std::istringstream stream(text);
	std::string line;
	while (std::getline(stream, line))
		lines.push_back(line);
	return lines;
}

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

// Every term the breakdown prints, in table order. Material is a row like any
// other: it is the largest single contribution and a sign error there would be
// the easiest one to miss.
// Every row the breakdown prints. The list must stay complete: the sum check
// below compares these nets against the printed total, so a missing row makes
// the invariant vacuous rather than failing loudly -- it passed for a while
// with `bishops` and `castling` absent only because both were zero in the
// positions tested here.
//
// The endgame row is absent here on purpose: it is net-only and is added to the
// sum through extract_endgame_net above.
static const char* const kBreakdownTerms[] = {"material", "pawns",   "rooks",    "pst",
                                              "mopup",    "bishops", "castling", "mobility"};

TEST_CASE("cmd_eval: printed breakdown nets are white-minus-black and sum to the evaluator's score", "[uci]")
{
	// The phase 2 honesty invariant (D9). Three separate claims, each of which
	// has failed in some engine at some point: each row's net is consistent
	// with its own two columns; the net column sums to the printed total; and
	// that total is the score the search would actually see, computed here
	// from a fresh Board through a separate EvalManager instance.
	const char* fen =
	    GENERATE("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", // Kiwipete, middlegame
	             "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",             // startpos, symmetric
	             "8/8/8/3r4/4k3/8/8/3QK3 w - - 0 1",                                     // endgame, White to move
	             "r3k3/8/8/8/8/8/8/4K3 b - - 0 1",   // Black to move, Black up a rook
	             "8/8/8/4k3/8/8/8/3QK3 w - - 0 1",   // pawnless K+Q vs K — mop-up active
	             "8/8/8/3k4/8/8/3N4/3K4 w - - 0 1"); // K+N vs K — scaled to a draw
	CAPTURE(fen);

	UciHandlerTestFixture fix;
	fix.position(std::string("position fen ") + fen);

	CoutRedirect redirect;
	fix.eval();
	const std::string out = redirect.str();

	int net_sum = 0;
	for (const char* term : kBreakdownTerms) {
		CAPTURE(term);
		const EvalTermRow row = extract_term_row(out, term);
		REQUIRE(row.net == row.white - row.black);
		net_sum += row.net;
	}
	net_sum += extract_endgame_net(out);

	REQUIRE(net_sum == extract_sum_white_pov(out));
	REQUIRE(net_sum == extract_cp_score(out, "white pov:"));

	auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);
	Board board(fen);
	const int side_to_move_score = eval->Evaluate(board);
	const int expected_white_pov = (board.GetCurrentColor() == WHITE) ? side_to_move_score : -side_to_move_score;

	REQUIRE(net_sum == expected_white_pov);
}

TEST_CASE("cmd_eval: breakdown reports the game phase the evaluator computed", "[uci]")
{
	// Phase is printed because it is not derivable from the rows, yet it sets
	// where between the mg and eg endpoints every tapered term landed — so a
	// reader debugging the pst row needs it to interpret that row at all
	// (issue #99, replacing the old middlegame/endgame stage name).
	UciHandlerTestFixture fix;

	SECTION("full starting material is the maximum phase")
	{
		fix.position("position startpos");

		CoutRedirect redirect;
		fix.eval();
		REQUIRE(redirect.str().find("phase: 24/24") != std::string::npos);
	}

	SECTION("bare kings plus a queen is deep in the endgame")
	{
		fix.position("position fen 4k3/8/8/8/8/8/8/3QK3 w - - 0 1");

		CoutRedirect redirect;
		fix.eval();
		REQUIRE(redirect.str().find("phase: 4/24") != std::string::npos);
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

	CoutRedirect redirect;
	fix.eval();
	const EvalTermRow mopup = extract_term_row(redirect.str(), "mopup");

	REQUIRE(mopup.white > 0);
	REQUIRE(mopup.black == 0);
}

// ---------------------------------------------------------------------------
// cmd_perft — "perft <depth>" / "go perft <depth>"
//
// The divide lines are a wire format, not diagnostics: external harnesses parse
// them with ^\s*([a-h][1-8][a-h][1-8][rnbqRNBQ]?)\s*[:\s]\s*(\d+)$ (#196), so
// these tests assert against that regex rather than against a substring.
// kDivideLine/parse_divide/divide_total live in UCITestFixture.h, since
// UCITests.cpp uses divide_total as a board-state oracle too.
// ---------------------------------------------------------------------------

TEST_CASE("cmd_perft: startpos depth 1 emits 20 harness-parseable divide lines", "[uci][perft]")
{
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	std::string output;
	{
		CoutRedirect redirect;
		fix.perft("perft 1");
		output = redirect.str();
	}

	const auto divides = parse_divide(output);
	REQUIRE(divides.size() == 20);
	for (const auto& entry : divides) {
		REQUIRE(entry.first.size() == 4);
		REQUIRE(entry.second == 1);
	}
	REQUIRE(divide_total(output) == 20);
}

TEST_CASE("cmd_perft: 'go perft' is not parsed as a search", "[uci][perft]")
{
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	std::string output;
	{
		CoutRedirect redirect;
		fix.perft("go perft 2");
		output = redirect.str();
	}

	REQUIRE(divide_total(output) == 400);
	REQUIRE(output.find("bestmove") == std::string::npos);
}

TEST_CASE("cmd_perft: honours the position set by cmd_position", "[uci][perft]")
{
	UciHandlerTestFixture fix;
	fix.position("position fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

	std::string output;
	{
		CoutRedirect redirect;
		fix.perft("perft 3");
		output = redirect.str();
	}

	REQUIRE(parse_divide(output).size() == 48);
	REQUIRE(divide_total(output) == 97862);
}

TEST_CASE("cmd_perft: malformed depth is ignored, not guessed at", "[uci][perft]")
{
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	for (const auto* line : {"perft", "perft abc", "perft -1", "perft 11", "go perft"}) {
		std::string output;
		{
			CoutRedirect redirect;
			fix.perft(line);
			output = redirect.str();
		}
		INFO("input: " << line);
		REQUIRE(parse_divide(output).empty());
	}
}

TEST_CASE("cmd_perft: leaves the board unchanged", "[uci][perft]")
{
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	{
		CoutRedirect redirect;
		fix.perft("perft 3");
	}

	REQUIRE(fix.board().GetCurrentColor() == WHITE);

	std::string output;
	{
		CoutRedirect redirect;
		fix.perft("perft 1");
		output = redirect.str();
	}
	REQUIRE(divide_total(output) == 20);
}

// run() is where "go perft" could be swallowed by the "go" branch, and the
// tests above bypass it by calling cmd_perft directly. This one drives the real
// command loop over stdin, which is what an external harness does.
class CinRedirect {
  public:
	explicit CinRedirect(std::string input) : buffer_(std::move(input)), old_(std::cin.rdbuf(buffer_.rdbuf())) {}
	~CinRedirect()
	{
		try {
			std::cin.rdbuf(old_);
		} catch (...) { // NOLINT(bugprone-empty-catch) - restoring cin in a destructor
		}
	}

	CinRedirect(const CinRedirect&) = delete;
	CinRedirect& operator=(const CinRedirect&) = delete;

  private:
	std::istringstream buffer_;
	std::streambuf* old_;
};

TEST_CASE("run(): dispatches 'go perft' to perft, not to the search", "[uci][perft]")
{
	UciHandler handler;

	std::string output;
	{
		CinRedirect input("position startpos\ngo perft 2\nquit\n");
		CoutRedirect redirect;
		handler.run();
		output = redirect.str();
	}

	REQUIRE(divide_total(output) == 400);
	REQUIRE(output.find("bestmove") == std::string::npos);
}

TEST_CASE("run(): a bare 'go' still searches after the perft branch was added", "[uci][perft]")
{
	UciHandler handler;

	std::string output;
	{
		CinRedirect input("position startpos\ngo depth 3\nquit\n");
		CoutRedirect redirect;
		handler.run();
		output = redirect.str();
	}

	REQUIRE(output.find("bestmove") != std::string::npos);
	REQUIRE(parse_divide(output).empty());
}

// ---------------------------------------------------------------------------
// cmd_go — per-iteration 'info' reporting (issue #237 stage 0)
// ---------------------------------------------------------------------------

namespace {

	// One parsed "info depth ..." line. `score` keeps the two-token form
	// ("cp 34" / "mate 3") verbatim so tests can check the kind without
	// re-deriving GameValues::Mate_Threshold.
	struct ParsedInfoLine {
		int depth = 0;
		std::string score;
		int64_t nodes = 0;
		int time_ms = 0;
		std::vector<std::string> pv;
	};

	// Every "info depth ..." line in `output`, in emission order. Deliberately
	// tolerant of any other 'info' line shape (e.g. 'info string ...') by only
	// matching the "info depth" prefix — those are unrelated protocol output,
	// not a parse failure.
	std::vector<ParsedInfoLine> parse_info_depth_lines(const std::string& output)
	{
		std::vector<ParsedInfoLine> result;
		for (const std::string& line : split_lines(output)) {
			if (!line.starts_with("info depth "))
				continue;

			std::istringstream iss(line);
			std::string tok;
			ParsedInfoLine info;
			while (iss >> tok) {
				if (tok == "depth") {
					iss >> info.depth;
				} else if (tok == "score") {
					std::string kind;
					std::string value;
					iss >> kind >> value;
					info.score = kind + " " + value;
				} else if (tok == "nodes") {
					iss >> info.nodes;
				} else if (tok == "time") {
					iss >> info.time_ms;
				} else if (tok == "pv") {
					std::string move;
					while (iss >> move)
						info.pv.push_back(move);
				}
			}
			result.push_back(std::move(info));
		}
		return result;
	}

	// The move token on a "bestmove <move>" line, or empty if none is present.
	std::string extract_bestmove(const std::string& output)
	{
		for (const std::string& line : split_lines(output)) {
			if (line.starts_with("bestmove ")) {
				return line.substr(std::string("bestmove ").size());
			}
		}
		return {};
	}

	// Replays a full PV (as UCI tokens) from `board`, resolving each token
	// against the position's own legal move list at that point -- mirrors
	// UciHandler's private find_replay_move() (UCIHandler.cpp), which is not
	// reachable from here. Returns false at the first token that is not a
	// legal move in the position it is played from.
	bool replay_pv_is_legal(Board board, const std::vector<std::string>& pv_tokens)
	{
		for (const std::string& token : pv_tokens) {
			MoveList moves;
			MoveGenerator::ComputeLegalMoves(board, moves);

			Move found;
			bool matched = false;
			for (const Move& candidate : moves) {
				if (MoveFormatter::ToUCI(candidate) == token) {
					found = candidate;
					matched = true;
					break;
				}
			}
			if (!matched || !board.DoMove(found))
				return false;
			board.ResetSearchDepth();
		}
		return true;
	}

} // namespace

TEST_CASE("cmd_go: 'go depth 4' emits per-iteration info lines with strictly increasing depth", "[uci]")
{
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go depth 4");
		fix.join_search();
		output = redirect.str();
	}

	const auto info_lines = parse_info_depth_lines(output);
	// At least the 4 per-iteration lines (depths 1-4) plus the unchanged final
	// summary line -- whose depth repeats the last iteration's, so only the
	// first 4 are asserted to be strictly increasing.
	REQUIRE(info_lines.size() >= 4);
	for (size_t i = 1; i < 4; ++i) {
		REQUIRE(info_lines[i].depth > info_lines[i - 1].depth);
	}
	REQUIRE(info_lines[0].depth == 1);

	int bestmove_lines = 0;
	for (const std::string& line : split_lines(output)) {
		if (line.starts_with("bestmove "))
			++bestmove_lines;
	}
	REQUIRE(bestmove_lines == 1);
}

TEST_CASE("cmd_go: iteration and final info times share one monotonic origin", "[uci][timing]")
{
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go depth 5");
		fix.join_search();
		output = redirect.str();
	}

	const auto info_lines = parse_info_depth_lines(output);
	REQUIRE(info_lines.size() >= 2);
	for (size_t i = 1; i < info_lines.size(); ++i)
		CHECK(info_lines[i].time_ms >= info_lines[i - 1].time_ms);
	CHECK(info_lines.back().time_ms >= info_lines[info_lines.size() - 2].time_ms);
}

TEST_CASE("cmd_go: the last info line's pv and score agree with bestmove", "[uci]")
{
	// This is the guarantee item 6 (issue #237 stage 0) exists for: the final
	// info line and 'bestmove' are built from the same shared score formatter
	// and the same SearchResult, so they cannot drift from each other.
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go depth 4");
		fix.join_search();
		output = redirect.str();
	}

	const auto info_lines = parse_info_depth_lines(output);
	REQUIRE_FALSE(info_lines.empty());
	const ParsedInfoLine& last = info_lines.back();
	REQUIRE_FALSE(last.pv.empty());

	const std::string bestmove = extract_bestmove(output);
	REQUIRE_FALSE(bestmove.empty());
	REQUIRE(last.pv.front() == bestmove);
}

TEST_CASE("cmd_go: 'go nodes N' stops on the node budget", "[uci]")
{
	// Driven past move 1 deliberately: from startpos the first search also pays a
	// cold transposition table, which is a different measurement.
	UciHandlerTestFixture fix;
	fix.position("position startpos moves e2e4 e7e5 g1f3");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go nodes 20000");
		fix.join_search();
		output = redirect.str();
	}

	const auto info_lines = parse_info_depth_lines(output);
	REQUIRE_FALSE(info_lines.empty());
	// At or past the budget, because the limit is only observed at a poll boundary.
	REQUIRE(info_lines.back().nodes >= 20000);
	REQUIRE_FALSE(extract_bestmove(output).empty());
}

TEST_CASE("cmd_go: 'go nodes 1' still returns a move", "[uci]")
{
	// The tightest budget expressible. The abort fires at the first poll, so no
	// iteration can complete normally and the answer has to come out of the
	// interrupted-iteration handling rather than a finished search.
	UciHandlerTestFixture fix;
	fix.position("position startpos moves e2e4 e7e5 g1f3");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go nodes 1");
		fix.join_search();
		output = redirect.str();
	}

	REQUIRE_FALSE(extract_bestmove(output).empty());
}

TEST_CASE("cmd_go: a node-limited search never reports a spliced pv", "[uci]")
{
	// #310: fastchess rejected moves in our pv lines at about 0.24 per game. The cause was
	// an aborted frame going on to splice an earlier sibling's line onto its own move, so
	// from the ply where the two positions diverge the line describes a different board.
	//
	// The node limit is what makes that reproducible: at Threads=1 the abort lands on the
	// same node every run. 10'000 from this position is where it was reproduced before the
	// unwind guard — depth 5 reported "pv d7d5 e4d5 d5e4 ...", a black move from a square
	// that by then holds a white pawn. The neighbouring budgets are not free coverage of a
	// second defect; they are the same check at other abort points, which is where a future
	// regression would just as likely land.
	const int budget = GENERATE(5'000, 10'000, 20'000, 50'000);

	UciHandlerTestFixture fix;
	fix.position("position startpos moves e2e4 e7e5 g1f3");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go nodes " + std::to_string(budget));
		fix.join_search();
		output = redirect.str();
	}

	const auto info_lines = parse_info_depth_lines(output);
	REQUIRE_FALSE(info_lines.empty());

	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2"));

	for (const ParsedInfoLine& line : info_lines) {
		INFO("budget " << budget << ", depth " << line.depth);
		REQUIRE(replay_pv_is_legal(board, line.pv));
	}

	// The interrupted iteration is kept rather than discarded, so the move played has to be
	// the one its own partial line names.
	REQUIRE_FALSE(info_lines.back().pv.empty());
	REQUIRE(info_lines.back().pv.front() == extract_bestmove(output));
}

TEST_CASE("cmd_go: a node limit bounds a multi-threaded search too", "[uci][smp]")
{
	// Only thread 0 polls, so the budget bounds thread 0's node count and the reported
	// total — which sums every helper — lands near Threads times it. The property worth
	// pinning is that the search still stops and still answers: helpers exit on the same
	// latched flag, and their aborted frames unwind through the same guard.
	UciHandlerTestFixture fix;
	fix.setoption("setoption name Threads value 4");
	fix.position("position startpos moves e2e4 e7e5 g1f3");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go nodes 20000");
		fix.join_search();
		output = redirect.str();
	}

	const auto info_lines = parse_info_depth_lines(output);
	REQUIRE_FALSE(info_lines.empty());
	REQUIRE_FALSE(extract_bestmove(output).empty());

	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2"));
	REQUIRE(replay_pv_is_legal(board, info_lines.back().pv));
}

TEST_CASE("cmd_go: an explicit depth still caps a node-bounded search", "[uci]")
{
	// A node budget lifts the default depth cap so it cannot silently truncate a
	// large budget, but an explicit 'depth' outranks both. Without that ordering a
	// node-limited match could not also be depth-limited.
	UciHandlerTestFixture fix;
	fix.position("position startpos moves e2e4 e7e5 g1f3");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go depth 5 nodes 100000000");
		fix.join_search();
		output = redirect.str();
	}

	const auto info_lines = parse_info_depth_lines(output);
	REQUIRE_FALSE(info_lines.empty());
	for (const ParsedInfoLine& line : info_lines) {
		CHECK(line.depth <= 5);
	}
	REQUIRE_FALSE(extract_bestmove(output).empty());
}

TEST_CASE("cmd_go: at depth >= 3 the pv carries more than one move and replays legally", "[uci]")
{
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go depth 4");
		fix.join_search();
		output = redirect.str();
	}

	const auto info_lines = parse_info_depth_lines(output);
	REQUIRE(info_lines.size() >= 4);

	// info_lines[3] is the fourth emitted line, i.e. the depth-4 per-iteration
	// line (see the strictly-increasing-depth test above for why depths 1-4
	// occupy the first four slots).
	const ParsedInfoLine& depth4 = info_lines[3];
	REQUIRE(depth4.depth >= 3);
	REQUIRE(depth4.pv.size() > 1);

	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));
	REQUIRE(replay_pv_is_legal(board, depth4.pv));
}

TEST_CASE("cmd_go: a forced mate reports 'mate N', not 'cp', in the score field", "[uci]")
{
	// Ra1-a8# -- verified elsewhere in this codebase's tactical suite
	// (TacticalTests.cpp: "M1: rook back rank", depth 4) as an actual mate-in-1
	// the search finds, so this FEN is not a fresh, unverified claim.
	UciHandlerTestFixture fix;
	fix.position("position fen 6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go depth 4");
		fix.join_search();
		output = redirect.str();
	}

	const auto info_lines = parse_info_depth_lines(output);
	REQUIRE_FALSE(info_lines.empty());
	REQUIRE(info_lines.back().score.starts_with("mate "));
	REQUIRE(extract_bestmove(output) == "a1a8");
}

TEST_CASE("AIPerplex::Search: emits no per-iteration output without a per-call observer", "[uci]")
{
	// A direct concrete call has no observer argument. This pins that observer
	// state lives only in that call, rather than on the AIPerplex service.
	UciHandlerTestFixture fix;
	fix.ucinewgame();
	fix.position("position startpos");

	std::string output;
	{
		CoutRedirect redirect;
		fix.run_search_directly(4);
		output = redirect.str();
	}

	REQUIRE(output.empty());
}

TEST_CASE("cmd_go: 'go movetime 300' final info line's nodes are >= the last per-iteration line's", "[uci]")
{
	// This is currently the only automated coverage of the ACCEPT_AND_STOP /
	// REJECT_AND_STOP paths through AIPerplex::iterative_deepening(): every
	// other 'go' test in this file is fixed-depth, which always finishes via
	// ACCEPT_AND_CONTINUE reaching max_depth rather than being interrupted.
	UciHandlerTestFixture fix;
	// Kiwipete: complex enough that 300ms will not reach UCI_DEFAULT_DEPTH (20).
	fix.position("position fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go movetime 300");
		fix.join_search();
		output = redirect.str();
	}

	const auto info_lines = parse_info_depth_lines(output);
	// At least one accepted per-iteration line plus the final summary line.
	REQUIRE(info_lines.size() >= 2);

	const ParsedInfoLine& last_iteration = info_lines[info_lines.size() - 2];
	const ParsedInfoLine& final_line = info_lines.back();

	REQUIRE(last_iteration.depth == final_line.depth);
	REQUIRE(last_iteration.score == final_line.score);
	// <=, not ==: a clocked search typically starts one more iteration that gets
	// interrupted and then rejected by assess_iteration_quality() -- REJECT_AND_STOP
	// emits no per-iteration line for it (iterative_deepening(), AIPerplex.cpp), but
	// that rejected iteration's nodes are already folded into td.nodes_searched by
	// the time Search() reports the final total (AIPerplex.h's IterationInfo doc).
	REQUIRE(last_iteration.nodes <= final_line.nodes);
}

TEST_CASE("cmd_go: immediate stop cannot be lost before Search arms", "[uci][immediate_stop]")
{
	UciHandlerTestFixture fix;
	fix.position("position fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

	const std::string stopped_output = capture_cout([&] {
		fix.dispatch("go infinite");
		fix.dispatch("stop"); // immediate: may arrive before Search arms its control
	});

	int bestmove_count = 0;
	for (const std::string& line : split_lines(stopped_output)) {
		if (line.starts_with("bestmove "))
			++bestmove_count;
	}
	REQUIRE(bestmove_count == 1);

	// A pending stop from the just-finished search must not poison the next
	// command. This search has no stop command and must complete its fixed depth.
	const std::string next_output = capture_cout([&] {
		fix.dispatch("go depth 2");
		fix.join_search();
	});
	const auto next_info = parse_info_depth_lines(next_output);
	REQUIRE_FALSE(next_info.empty());
	REQUIRE(next_info.back().depth == 2);
	REQUIRE_FALSE(extract_bestmove(next_output).empty());
}

TEST_CASE("cmd_go: stop during go infinite preserves output lines under concurrent isready", "[uci][output_integrity]")
{
	// This is separate from the no-sleep immediate-stop regression above. Wait for observed search
	// output before issuing ready replies so this case proves real output overlap without a blind
	// scheduler delay.
	UciHandlerTestFixture fix;
	fix.position("position fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

	constexpr int kIsReadyCount = 20;
	bool info_observed = false;
	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go infinite");
		info_observed = redirect.wait_for("info depth ", std::chrono::seconds(5));
		if (info_observed) {
			for (int i = 0; i < kIsReadyCount; ++i)
				fix.dispatch("isready");
		}
		fix.dispatch("stop");
		output = redirect.str();
	}

	REQUIRE(info_observed);
	const std::regex line_shape{R"(^(info|bestmove|readyok)\b)"};
	int info_count = 0;
	int readyok_count = 0;
	for (const std::string& line : split_lines(output)) {
		if (line.empty())
			continue;
		REQUIRE(std::regex_search(line, line_shape));
		if (line.starts_with("info "))
			++info_count;
		if (line == "readyok")
			++readyok_count;
	}
	REQUIRE(info_count >= 1);
	REQUIRE(readyok_count == kIsReadyCount);
}

TEST_CASE("cmd_go: back-to-back searches emit only their own per-call iterations", "[uci][back_to_back]")
{
	// The observer is passed into exactly one Search call. The second command must
	// therefore start a fresh stream rather than retaining the first call's closure.
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	const std::string first_output = capture_cout([&] {
		fix.dispatch("go depth 4");
		fix.join_search();
	});
	const auto first_search_info = parse_info_depth_lines(first_output);
	REQUIRE_FALSE(first_search_info.empty());
	REQUIRE(first_search_info.front().depth == 1);
	REQUIRE_FALSE(extract_bestmove(first_output).empty());

	const std::string second_output = capture_cout([&] {
		fix.dispatch("go depth 4");
		fix.join_search();
	});
	const auto second_search_info = parse_info_depth_lines(second_output);
	REQUIRE_FALSE(second_search_info.empty());
	REQUIRE(second_search_info.front().depth == 1);
	REQUIRE_FALSE(extract_bestmove(second_output).empty());
}

// ---------------------------------------------------------------------------
// Measurement contract over the wire (issue #312)
// ---------------------------------------------------------------------------
// Between the counters (covered in SearchTests) and the bench parser sits the
// protocol text itself, which nothing else asserts on.

namespace {

	// The 'main' and 'qs' operands of the last "info string treenodes ..." line,
	// or nullopt if the output carries none.
	std::optional<std::pair<int64_t, int64_t>> parse_treenodes(const std::string& output)
	{
		std::optional<std::pair<int64_t, int64_t>> found;
		for (const std::string& line : split_lines(output)) {
			if (!line.starts_with("info string treenodes "))
				continue;

			std::istringstream iss(line);
			std::string info;
			std::string string_tok;
			std::string treenodes;
			std::string main_tok;
			int64_t main_nodes = 0;
			std::string qs_tok;
			int64_t qs_nodes = 0;
			if (!(iss >> info >> string_tok >> treenodes >> main_tok >> main_nodes >> qs_tok >> qs_nodes))
				continue;
			if (main_tok != "main" || qs_tok != "qs")
				continue;
			found = std::make_pair(main_nodes, qs_nodes);
		}
		return found;
	}

} // namespace

TEST_CASE("cmd_uci: the handshake advertises a measurement contract version", "[uci][nodes]")
{
	// Run-Bench.ps1 reads absence as "pre-#312 build", so losing this line would not
	// error out — it would silently relabel every future run as an old one.
	UciHandlerTestFixture fix;
	const std::string output = capture_cout([&] { fix.uci(); });

	const std::regex contract_line(R"(^info string benchcontract [1-9]\d*$)");
	bool matched = false;
	for (const std::string& line : split_lines(output)) {
		if (std::regex_match(line, contract_line))
			matched = true;
	}
	REQUIRE(matched);
}

TEST_CASE("cmd_go: 'nodes' equals the reported main/quiescence split", "[uci][nodes]")
{
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go depth 5");
		fix.join_search();
		output = redirect.str();
	}

	const auto split = parse_treenodes(output);
	REQUIRE(split.has_value());
	const auto [main_nodes, qs_nodes] = *split;

	// A zero on either side is what a counter that stopped being incremented looks like
	// from outside the engine.
	CHECK(main_nodes > 0);
	CHECK(qs_nodes > 0);

	// The same invariant Run-Bench.ps1 enforces on every position it measures.
	const auto info_lines = parse_info_depth_lines(output);
	REQUIRE_FALSE(info_lines.empty());
	CHECK(info_lines.back().nodes == main_nodes + qs_nodes);
}
