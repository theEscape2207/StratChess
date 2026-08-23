// FenParsingTests.cpp — Catch2 [fen] tests for FenBatch::ClassifyLine,
// FENParser::ParseFEN/ValidatePositionAgainstFENMetadata, Board::SetupFromFEN
// and Board(fen). Split out of UCITests.cpp (#302): these exercise FEN
// parsing directly, with no UciHandler involved.

#include <catch_amalgamated.hpp>
#include "Board.h"
#include "Utils/FenBatch.h"
#include <algorithm>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>

// ---------------------------------------------------------------------------
// FenBatch::ClassifyLine — batch-mode FEN validation (issue #140)
// ---------------------------------------------------------------------------
//
// evalrunner() (StratChessEvolved.cpp, issue #129 phase 1) is untestable
// directly — it's a static function in a translation unit the test project
// does not link. FenBatch::ClassifyLine (StratEngine/Utils/FenBatch.h) is
// the extracted, header-only classification it now delegates to, so these
// cases exercise the same two-tier guard evalrunner() relies on: without
// it, a malformed line would silently score a fresh, empty Board as 0 —
// plausible-looking garbage in a tuning corpus rather than an obvious
// failure.

TEST_CASE("FenBatch::ClassifyLine: blank line is Skip", "[fen]")
{
	auto r = FenBatch::ClassifyLine("");
	REQUIRE(r.kind == FenBatch::LineKind::Skip);

	auto r_ws = FenBatch::ClassifyLine("   \t  ");
	REQUIRE(r_ws.kind == FenBatch::LineKind::Skip);
}

TEST_CASE("FenBatch::ClassifyLine: '#' comment line is Skip", "[fen]")
{
	auto r = FenBatch::ClassifyLine("# this is a comment");
	REQUIRE(r.kind == FenBatch::LineKind::Skip);
}

TEST_CASE("FenBatch::ClassifyLine: 2-field line is Malformed with the field-count message", "[fen]")
{
	auto r = FenBatch::ClassifyLine("8/8/8/8/8/8/8/8 w");
	REQUIRE(r.kind == FenBatch::LineKind::Malformed);
	REQUIRE_FALSE(r.error.empty());
	REQUIRE(r.error.find("too few fields") != std::string::npos);
}

TEST_CASE("FenBatch::ClassifyLine: 4-field line is Valid", "[fen]")
{
	// EPD corpora and hand-authored positions are overwhelmingly 4-field, so this is the form
	// #117's tuning work needs to ingest.
	auto r = FenBatch::ClassifyLine("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq -");
	REQUIRE(r.kind == FenBatch::LineKind::Valid);
	REQUIRE(r.error.empty());
}

TEST_CASE("FenBatch::ClassifyLine: 5-field line is Valid", "[fen]")
{
	auto r = FenBatch::ClassifyLine("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 3");
	REQUIRE(r.kind == FenBatch::LineKind::Valid);
	REQUIRE(r.error.empty());
}

TEST_CASE("FenBatch::ClassifyLine: EPD operations are still rejected", "[fen]")
{
	// Accepting 4-6 fields is deliberately NOT the same as accepting EPD. Trailing operations
	// are out of scope for the FEN grammar; #117's corpus loader owns them.
	auto r = FenBatch::ClassifyLine(R"(rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - c9 "1-0";)");
	REQUIRE(r.kind == FenBatch::LineKind::Malformed);
	REQUIRE_FALSE(r.error.empty());
}

// ---------------------------------------------------------------------------
// FENParser::ParseFEN — optional halfmove/fullmove fields (issue #143)
// ---------------------------------------------------------------------------

TEST_CASE("FENParser::ParseFEN: 4-field FEN defaults halfmove to 0 and fullmove to 1", "[fen]")
{
	FENParser::FENGameState state;
	std::vector<std::tuple<ePiece, eSquare>> pieces;
	auto err = FENParser::ParseFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq -", state, pieces);

	REQUIRE_FALSE(err.has_value());
	CHECK(state.sideToMove == eColor::WHITE);
	CHECK(state.halfMoveClock == 0);
	CHECK(state.fullMoveCounter == 1);
}

TEST_CASE("FENParser::ParseFEN: 5-field FEN keeps the halfmove clock, defaults fullmove to 1", "[fen]")
{
	FENParser::FENGameState state;
	std::vector<std::tuple<ePiece, eSquare>> pieces;
	auto err = FENParser::ParseFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 7", state, pieces);

	REQUIRE_FALSE(err.has_value());
	CHECK(state.sideToMove == eColor::BLACK);
	CHECK(state.halfMoveClock == 7);
	CHECK(state.fullMoveCounter == 1);
}

TEST_CASE("FENParser::ParseFEN: 6-field FEN is unaffected by the relaxation", "[fen]")
{
	FENParser::FENGameState state;
	std::vector<std::tuple<ePiece, eSquare>> pieces;
	auto err = FENParser::ParseFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 12 34", state, pieces);

	REQUIRE_FALSE(err.has_value());
	CHECK(state.halfMoveClock == 12);
	CHECK(state.fullMoveCounter == 34);
}

TEST_CASE("FENParser::ParseFEN: fewer than 4 fields reports the field-count error", "[fen]")
{
	FENParser::FENGameState state;
	std::vector<std::tuple<ePiece, eSquare>> pieces;
	auto err = FENParser::ParseFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq", state, pieces);

	REQUIRE(err.has_value());
	CHECK(err->find("too few fields") != std::string::npos);
}

namespace {
	// Throws on every log call to verify metadata correction does not depend on logging.
	class ThrowingSink final : public spdlog::sinks::base_sink<std::mutex> {
	  protected:
		void sink_it_(const spdlog::details::log_msg&) override { throw std::runtime_error("sink failure (test)"); }
		void flush_() override {}
	};

	// Restores the default logger's sink list on every scope exit.
	class ScopedThrowingSink {
	  public:
		ScopedThrowingSink() : sink_(std::make_shared<ThrowingSink>())
		{
			spdlog::default_logger()->sinks().push_back(sink_);
		}
		~ScopedThrowingSink()
		{
			try {
				auto& sinks = spdlog::default_logger()->sinks();
				sinks.erase(std::remove(sinks.begin(), sinks.end(), sink_), sinks.end());
			} catch (...) { // NOLINT(bugprone-empty-catch) - cleanup in a destructor
			}
		}
		ScopedThrowingSink(const ScopedThrowingSink&) = delete;
		ScopedThrowingSink& operator=(const ScopedThrowingSink&) = delete;

	  private:
		std::shared_ptr<ThrowingSink> sink_;
	};
} // namespace

TEST_CASE("FENParser::ValidatePositionAgainstFENMetadata: every correction still applies when "
          "logging throws",
          "[fen]")
{
	// Two independent corrections ensure a logging failure cannot stop validation early.
	const std::string fen = "4k3/8/8/8/4P3/8/8/3K4 w Q e6 0 1";

	Board board;
	REQUIRE(board.SetupFromFEN(fen));

	FENParser::FENGameState state;
	std::vector<std::tuple<ePiece, eSquare>> pieces;
	REQUIRE_FALSE(FENParser::ParseFEN(fen, state, pieces));
	REQUIRE(state.castlingRights == CastlingRights::WHITE_QUEENSIDE);
	REQUIRE(state.epSquare == e6);

	bool ok = false;
	{
		ScopedThrowingSink throwing;
		REQUIRE_NOTHROW(ok = FENParser::ValidatePositionAgainstFENMetadata(board, state));
	}

	CHECK(ok);
	CHECK(state.castlingRights == CastlingRights::NONE);
	CHECK(state.epSquare == NO_SQUARE);
}

TEST_CASE("Board::SetupFromFEN: 4-field FEN yields halfmoveClock 0", "[fen]")
{
	// The halfmove default is not inert: SetupFromFEN feeds it into Board::halfmove_clock(),
	// which drives 50-move draw detection. Pin the value rather than leave it implicit.
	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq -"));

	// The kings are checked as well as the return value: an empty board's halfmoveClock is also 0,
	// so without them a position that never landed would pass this test for the wrong reason.
	REQUIRE(board.GetPiece(e1) == WHITE_KING);
	REQUIRE(board.GetPiece(e8) == BLACK_KING);
	CHECK(board.halfmove_clock() == 0);
}

// ---------------------------------------------------------------------------
// Board::SetupFromFEN — halfmove/fullmove counter bounds (GameState.h's
// MAX_FEN_HALFMOVE_CLOCK / MAX_FEN_FULLMOVE_COUNT). A value past these
// describes a game that cannot be played, so it is rejected, not repaired.
// ---------------------------------------------------------------------------

TEST_CASE("Board::SetupFromFEN: halfmove clock past MAX_FEN_HALFMOVE_CLOCK is rejected", "[fen]")
{
	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));

	REQUIRE_FALSE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 11798 1"));

	// Untouched means the previous position, not a reset and not an empty board.
	CHECK(board.GetPiece(e1) == WHITE_KING);
	CHECK(board.halfmove_clock() == 0);
}

TEST_CASE("Board::SetupFromFEN: halfmove clock at MAX_FEN_HALFMOVE_CLOCK loads", "[fen]")
{
	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 11797 1"));

	std::istringstream fss(board.ExtractFEN());
	std::vector<std::string> fenParts{std::istream_iterator<std::string>(fss), std::istream_iterator<std::string>()};
	REQUIRE(fenParts.size() == 6);
	CHECK(fenParts[4] == "11797");
}

TEST_CASE("Board::SetupFromFEN: fullmove counter past MAX_FEN_FULLMOVE_COUNT is rejected", "[fen]")
{
	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));

	REQUIRE_FALSE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 5900"));

	CHECK(board.GetPiece(e1) == WHITE_KING);
	CHECK(board.fullmove_count() == 1);
}

TEST_CASE("Board::SetupFromFEN: fullmove counter at MAX_FEN_FULLMOVE_COUNT loads", "[fen]")
{
	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 5899"));

	std::istringstream fss(board.ExtractFEN());
	std::vector<std::string> fenParts{std::istream_iterator<std::string>(fss), std::istream_iterator<std::string>()};
	REQUIRE(fenParts.size() == 6);
	CHECK(fenParts[5] == "5899");
}

// Pins the deliberate repair: a FEN fullmove number is 1-based, so 0 is not out-of-range input,
// it is a hand-authored position that meant "move one" -- std::max(1, full) fixes it up rather
// than rejecting it.
TEST_CASE("Board::SetupFromFEN: fullmove counter 0 is repaired to 1", "[fen]")
{
	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 0"));

	std::istringstream fss(board.ExtractFEN());
	std::vector<std::string> fenParts{std::istream_iterator<std::string>(fss), std::istream_iterator<std::string>()};
	REQUIRE(fenParts.size() == 6);
	CHECK(fenParts[5] == "1");
}

TEST_CASE("Board::SetupFromFEN: reports failure and leaves the board untouched", "[fen]")
{
	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));

	// Missing the side-to-move, castling and en-passant fields: rejected by the field-count floor.
	REQUIRE_FALSE(board.SetupFromFEN("6k1/5ppp/8/8/8/8/5PPP/R5K1"));

	// Untouched means the previous position, not a reset and not an empty board.
	CHECK(board.GetPiece(e1) == WHITE_KING);
	CHECK(board.GetPiece(d8) == BLACK_QUEEN);
	CHECK(board.GetCurrentColor() == WHITE);
}

// ---------------------------------------------------------------------------
// Piece placement sanity: exactly one king per color (issue #163)
//
// A board missing a king, or holding two of one color, is what let a generated
// king-capturing move reach Board::DoMove and read one entry past
// g_bbKingMoves' 64-entry table before #45 closed the only known route in. The
// initial-position invariant was already enforced by FENParser::ParsePiecePlacementField.
// UCI replay separately resolves every client token against generated moves before DoMove,
// so malformed protocol input cannot remove a king after setup either.
// ---------------------------------------------------------------------------

TEST_CASE("Board::SetupFromFEN: rejects a FEN with no black king", "[fen]")
{
	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));

	// Black king square (e8) left empty; every other black piece unchanged.
	REQUIRE_FALSE(board.SetupFromFEN("rnbq1bnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));

	CHECK(board.GetPiece(e8) == BLACK_KING);
}

TEST_CASE("Board::SetupFromFEN: rejects a FEN with two white kings", "[fen]")
{
	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));

	// White queen (d1) replaced by a second white king.
	REQUIRE_FALSE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBKKBNR w KQkq - 0 1"));

	CHECK(board.GetPiece(d1) == WHITE_QUEEN);
}

// Board(fen) documents an assert-in-Debug / empty-board-in-Release contract for a malformed
// FEN (Board.h: "every caller passes a literal, so a malformed one is a bug in the caller") --
// it is not meant to validate untrusted input, unlike SetupFromFEN. Gated to Release only:
// under Debug the constructor's own assert would abort the process, which is correct behavior
// for a caller bug but not something a REQUIRE-based test can observe without crashing the
// Debug/sanitizer CI leg along with it.
#ifdef NDEBUG
TEST_CASE("Board(fen): a missing king leaves the board empty, matching SetupFromFEN's contract", "[fen]")
{
	Board board("rnbq1bnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

	CHECK(board.GetPiece(e1) == NO_PIECE);
	CHECK(board.GetPiece(a1) == NO_PIECE);
}
#endif

TEST_CASE("Board::SetupFromFEN: repairs overload replaces previous diagnostics", "[fen]")
{
	Board board;
	std::vector<std::string> repairs;

	REQUIRE(board.SetupFromFEN("4k3/8/8/8/4P3/8/8/3K4 w Q e6 0 1", repairs));
	REQUIRE_FALSE(repairs.empty());

	REQUIRE(board.SetupFromFEN("4k3/8/8/8/8/8/8/4K3 w - - 0 1", repairs));
	CHECK(repairs.empty());
}

// ---------------------------------------------------------------------------
// Position legality: the side NOT to move may not be in check (issue #45)
// ---------------------------------------------------------------------------

// The issue's exact repro. White's rook on e1 attacks the black king on e8 with an empty file
// between them, so Black — the waiting side — is in check. Searching such a position used to reach
// a board with one king removed, where attack generation reads past its move table.
TEST_CASE("Board::SetupFromFEN: rejects a position with the waiting side in check", "[fen]")
{
	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));

	REQUIRE_FALSE(board.SetupFromFEN("4k3/8/8/8/8/5b2/8/4RK2 w - - 0 1"));

	// Rejected means nothing was applied, illegality included.
	CHECK(board.GetPiece(d8) == BLACK_QUEEN);
	CHECK(board.GetCurrentColor() == WHITE);
}

// The control for the test above: the same position with the other side to move is perfectly legal
// — Black is in check and must answer it. If the rule tested the wrong king this would fail.
TEST_CASE("Board::SetupFromFEN: accepts the same position with the checked side to move", "[fen]")
{
	Board board;
	REQUIRE(board.SetupFromFEN("4k3/8/8/8/8/5b2/8/4RK2 b - - 0 1"));

	CHECK(board.GetCurrentColor() == BLACK);
	CHECK(board.InCheck());
	CHECK_FALSE(board.WaitingSideInCheck());
}

// Adjacent kings need no rule of their own: each king attacks the other, so the waiting king is
// attacked and the same test rejects the position.
TEST_CASE("Board::SetupFromFEN: rejects adjacent kings", "[fen]")
{
	Board board;
	REQUIRE_FALSE(board.SetupFromFEN("8/8/8/3kK3/8/8/8/8 w - - 0 1"));
	REQUIRE_FALSE(board.SetupFromFEN("8/8/8/3kK3/8/8/8/8 b - - 0 1"));
}

// Guards against over-rejection: an ordinary position where the side to move is in check has to keep
// loading, since that is what most tactical test positions are.
TEST_CASE("Board::SetupFromFEN: a normal check position still loads", "[fen]")
{
	Board board;
	REQUIRE(board.SetupFromFEN("rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3"));

	CHECK(board.InCheck());
	CHECK_FALSE(board.WaitingSideInCheck());
}

TEST_CASE("FenBatch::ClassifyLine: valid 6-field White-to-move FEN is Valid", "[fen]")
{
	auto r = FenBatch::ClassifyLine("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
	REQUIRE(r.kind == FenBatch::LineKind::Valid);
	REQUIRE(r.error.empty());
}

TEST_CASE("FenBatch::ClassifyLine: valid 6-field Black-to-move FEN is Valid", "[fen]")
{
	auto r = FenBatch::ClassifyLine("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R b KQkq - 0 1");
	REQUIRE(r.kind == FenBatch::LineKind::Valid);
	REQUIRE(r.error.empty());
}
