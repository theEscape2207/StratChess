#include "StdAfx.h"
#include "FENParser.h"
#include "Board.h"
#include "PieceHelper.h"
#include "Formatters.h" // For logging of pieces and squares
#include <regex>
#include <optional>

/*
FEN specifies the piece placement, the active color, the castling availability,
the en passant target square, the halfmove clock, and the fullmove number.

<FEN> ::=  <Piece Placement>
' ' <Side to move>
' ' <Castling ability>
' ' <En passant target square>
' ' <Halfmove clock>
' ' <Fullmove counter>

Starting position:
rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1

After 1. e4:
rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1
*/

// Convert unexpected parser exceptions into the existing error channel.
// NOLINTNEXTLINE(bugprone-exception-escape)
std::optional<std::string> FENParser::ParseFEN(const std::string& fen, FENGameState& outState,
                                               std::vector<std::tuple<ePiece, eSquare>>& outPieces) noexcept
{
	try {
		return ParseFENImpl(fen, outState, outPieces);
	} catch (...) {
		return std::string("internal error parsing FEN");
	}
}

std::optional<std::string> FENParser::ParseFENImpl(const std::string& fen, FENGameState& outState,
                                                   std::vector<std::tuple<ePiece, eSquare>>& outPieces)
{
	// Trim input
	std::string s = fen;
	while (!s.empty() && isspace((unsigned char)s.front()))
		s.erase(s.begin());
	while (!s.empty() && isspace((unsigned char)s.back()))
		s.pop_back();

	// Tokenize before the regex runs, so a line that is not a FEN at all gets the specific
	// "too few fields" diagnostic rather than the regex's generic message.
	std::istringstream iss(s);
	std::vector<std::string> parts;
	std::string token;
	while (iss >> token)
		parts.emplace_back(token);

	if (parts.size() < 4) {
		return std::string("too few fields in FEN");
	}

	// Quick overall format check with regex.
	//
	// Fields 5 (halfmove clock) and 6 (fullmove number) are OPTIONAL, matching the handling
	// further down and the `parts.size() < 4` floor above. Hand-authored positions and EPD-style
	// lines routinely omit them. Field 6 is only accepted when field 5 is present — "placement
	// side castling ep <n>" is a 5-field FEN, and there is no form that supplies the fullmove
	// number while omitting the halfmove clock.
	//
	// Trailing content after field 6 is rejected. Full EPD (operations such as `c9 "1-0";` after
	// the four core fields) is deliberately out of scope: that belongs in #117's corpus loader,
	// not in the FEN grammar.
	//
	// En-passant accepts any rank here; only ValidatePositionAgainstFENMetadata knows enough
	// (side to move, actual pieces) to say which rank is right, so rank checking lives there.
	static const std::regex fenRx(
	    R"(^\s*([rnbqkpRNBQKP1-8]+\/){7}([rnbqkpRNBQKP1-8]+)\s+[wb]\s+(-|[KQkq]+)\s+(-|[a-h][1-8])(\s+\d+(\s+\d+)?)?\s*$)");
	if (!std::regex_match(s, fenRx)) {
		return std::string("overall format invalid");
	}

	const std::string& pieceField = parts[0];

	// Validate rank expansion to 8 files before deeper parse
	{
		std::istringstream rankss(pieceField);
		std::string rankToken;
		std::vector<std::string> ranks;
		while (std::getline(rankss, rankToken, '/'))
			ranks.emplace_back(rankToken);
		if (ranks.size() != 8) {
			return std::string("expected 8 ranks in piece placement");
		}
		for (size_t i = 0; i < ranks.size(); ++i) {
			int files = 0;
			for (char c : ranks[i]) {
				if (std::isdigit(static_cast<unsigned char>(c)))
					files += c - '0';
				else
					files += 1;
			}
			if (files != 8) {
				return std::string("rank ") + std::to_string(i) + " expands to " + std::to_string(files) +
				       " files (expected 8)";
			}
		}
	}

	// Parse piece placement into outPieces
	if (auto err = ParsePiecePlacementField(pieceField, outPieces)) {
		return err;
	}

	// Side to move
	const std::string& side = parts[1];
	if (side.size() != 1 || (side[0] != 'w' && side[0] != 'b')) {
		return std::string("invalid side to move field");
	}
	outState.sideToMove = (side[0] == 'b') ? eColor::BLACK : eColor::WHITE;

	// Castling rights
	if (auto err = PopulateCastlingFlags(parts[2], outState.castlingRights)) {
		return err;
	}

	// En-passant. Shape is already guaranteed by fenRx above ("-" or [a-h][1-8]); rank
	// consistency is checked later, in ValidatePositionAgainstFENMetadata.
	const std::string& ep = parts[3];
	if (ep != "-") {
		outState.epSquare = SquareFromString(ep);
	}

	// Halfmove clock (optional).
	// When absent, outState keeps FENGameState's default of 0. Board::SetupFromFEN feeds this
	// into gameInfo_.fiftyCount, so a 4-field FEN is treated as having made no progress toward
	// the 50-move draw. That understates progress for a position lifted out of a real game, but
	// 0 is the conventional default (python-chess does the same) and it is bookkeeping only --
	// unlike a missing side-to-move field, it cannot change whose move it is (cf. issue #46).
	if (parts.size() >= 5) {
		try {
			int half = std::stoi(parts[4]);
			outState.halfMoveClock = std::max(0, half);
		} catch (...) {
			return std::string("invalid halfmove clock");
		}
	}

	// Fullmove number (optional). Defaults to FENGameState's 1 when absent -- move one, the
	// same convention every other FEN consumer uses. Affects display and ExtractFEN round-trips
	// only; nothing in search reads it.
	if (parts.size() >= 6) {
		try {
			int full = std::stoi(parts[5]);
			outState.fullMoveCounter = std::max(1, full);
		} catch (...) {
			return std::string("invalid fullmove counter");
		}
	}

	return std::nullopt; // success
}

std::optional<std::string> FENParser::PopulateCastlingFlags(const std::string& castling, uint8_t& outRights)
{
	if (castling == "-") {
		outRights = CastlingRights::NONE;
		return std::nullopt;
	}

	static constexpr std::array<std::pair<char, uint8_t>, 4> charToRight = {{{'K', CastlingRights::WHITE_KINGSIDE},
	                                                                         {'Q', CastlingRights::WHITE_QUEENSIDE},
	                                                                         {'k', CastlingRights::BLACK_KINGSIDE},
	                                                                         {'q', CastlingRights::BLACK_QUEENSIDE}}};

	uint8_t rights = CastlingRights::NONE;

	for (char c : castling) {
		uint8_t bit = CastlingRights::NONE;
		for (const auto& [ch, flag] : charToRight) {
			if (c == ch) {
				bit = flag;
				break;
			}
		}

		if (bit == CastlingRights::NONE)
			return std::string("invalid castling character encountered");

		if (rights & bit) // bit already set → duplicate
			return std::string("duplicate castling character encountered");

		rights |= bit;
	}

	outRights = rights;
	return std::nullopt;
}

std::optional<std::string> FENParser::ParsePiecePlacementField(const std::string& placement,
                                                               std::vector<std::tuple<ePiece, eSquare>>& outVec)
{
	outVec.clear();

	// Split ranks by '/'
	std::vector<std::string> ranks;
	{
		std::istringstream ss(placement);
		std::string token;
		while (std::getline(ss, token, '/'))
			ranks.emplace_back(token);
	}

	if (ranks.size() != 8) {
		return std::string("expected 8 ranks in placement field");
	}

	int whiteKingCount = 0;
	int blackKingCount = 0;
	int whitePawnCount = 0;
	int blackPawnCount = 0;
	int totalPieces = 0;

	for (std::size_t rank = 0; rank < 8; ++rank) {
		const std::string& rstr = ranks[rank];
		int fileIndex = 0; // 0..7

		for (char c : rstr) {
			if (std::isdigit(static_cast<unsigned char>(c))) {
				int n = c - '0';
				if (n < 1 || n > 8) {
					return std::string("invalid digit in rank expansion");
				}
				if (fileIndex + n > 8) {
					return std::string("rank expands past 8 files");
				}
				fileIndex += n;
			} else {
				if (fileIndex >= 8) {
					return std::string("too many files in rank");
				}

				ePiece piece = ePiece::NO_PIECE;
				switch (c) {
				case 'K':
					piece = ePiece::WHITE_KING;
					++whiteKingCount;
					break;
				case 'Q':
					piece = ePiece::WHITE_QUEEN;
					break;
				case 'R':
					piece = ePiece::WHITE_ROOK;
					break;
				case 'B':
					piece = ePiece::WHITE_BISHOP;
					break;
				case 'N':
					piece = ePiece::WHITE_KNIGHT;
					break;
				case 'P':
					piece = ePiece::WHITE_PAWN;
					++whitePawnCount;
					break;
				case 'k':
					piece = ePiece::BLACK_KING;
					++blackKingCount;
					break;
				case 'q':
					piece = ePiece::BLACK_QUEEN;
					break;
				case 'r':
					piece = ePiece::BLACK_ROOK;
					break;
				case 'b':
					piece = ePiece::BLACK_BISHOP;
					break;
				case 'n':
					piece = ePiece::BLACK_KNIGHT;
					break;
				case 'p':
					piece = ePiece::BLACK_PAWN;
					++blackPawnCount;
					break;
				default:
					return std::string("invalid piece character in placement");
				}

				// Pawns cannot appear on first or last rank
				if ((piece == ePiece::WHITE_PAWN || piece == ePiece::BLACK_PAWN) && (rank == 0 || rank == 7)) {
					return std::string("pawn on invalid rank");
				}

				const eSquare sq = static_cast<eSquare>((rank << 3) + fileIndex);
				outVec.emplace_back(std::make_tuple(piece, sq));

				++fileIndex;
				++totalPieces;
				if (totalPieces > 32) {
					return std::string("too many pieces (>32)");
				}
			}
		}

		if (fileIndex != 8) {
			return std::string("rank expands to wrong number of files");
		}
	}

	// Basic sanity checks
	if (whiteKingCount != 1 || blackKingCount != 1) {
		return std::string("expected one white king and one black king");
	}
	if (whitePawnCount > 8 || blackPawnCount > 8) {
		return std::string("too many pawns");
	}

	return std::nullopt;
}

eSquare FENParser::SquareFromString(const std::string& s) noexcept
{
	if (s.size() != 2)
		return NO_SQUARE;
	const char file = s[0];
	const char rankChar = s[1];
	if (file < 'a' || file > 'h' || rankChar < '1' || rankChar > '8')
		return NO_SQUARE;
	const unsigned int row = file - 'a';
	const unsigned int col = ((8 - (rankChar - '0')) << 3);
	return static_cast<eSquare>(col + row);
}

namespace {
	// Metadata correction must continue even when formatting, logging, or collecting a warning
	// fails -- each step is best-effort and none of them may abort the repair itself. outWarnings
	// is the channel a caller reads when spdlog is off (UCI mode); see cmd_position.
	template <typename... Args>
	void report_repair_noexcept(std::vector<std::string>* outWarnings, std::string_view fmt_str,
	                            const Args&... args) noexcept
	{
		std::string message;
		try {
			message = fmt::format(fmt::runtime(fmt_str), args...);
		} catch (...) { // NOLINT(bugprone-empty-catch) - formatting is best-effort here
			return;
		}
		try {
			spdlog::default_logger()->warn(message);
		} catch (...) { // NOLINT(bugprone-empty-catch) - logging is best-effort here
		}
		if (outWarnings) {
			try {
				outWarnings->push_back(message);
			} catch (...) { // NOLINT(bugprone-empty-catch) - collection is best-effort here
			}
		}
	}
} // namespace

// Validate against explicit Board reference
bool FENParser::ValidatePositionAgainstFENMetadata(const Board& board, FENGameState& state,
                                                   std::vector<std::string>* outWarnings) noexcept
{
	// Each entry defines one side's castling validation requirements
	static constexpr std::array<std::tuple<eSquare, eColor, uint8_t, uint8_t, eSquare, eSquare>, 2> sides = {
	    {{e1, WHITE, CastlingRights::WHITE_KINGSIDE, CastlingRights::WHITE_QUEENSIDE, h1, a1},
	     {e8, BLACK, CastlingRights::BLACK_KINGSIDE, CastlingRights::BLACK_QUEENSIDE, h8, a8}}};

	for (const auto& [kingSq, color, kingsideFlag, queensideFlag, rookKingSq, rookQueenSq] : sides) {
		uint8_t sideMask = kingsideFlag | queensideFlag;

		// Skip if neither right is claimed for this side
		if (!(state.castlingRights & sideMask))
			continue;

		// King must be present and correct color
		if (!PieceHelper::IsKing(board.GetPiece(kingSq)) || PieceHelper::Color(board.GetPiece(kingSq)) != color) {
			report_repair_noexcept(outWarnings, "Clearing {} castling rights: king not on {}", color, kingSq);
			state.castlingRights &= ~sideMask;
			continue;
		}

		// Check kingside rook
		if (state.castlingRights & kingsideFlag) {
			if (!PieceHelper::IsOfPiece(board.GetPiece(rookKingSq), PieceHelper::AsPiece(ROOK, color))) {
				report_repair_noexcept(outWarnings, "Clearing {} king-side castling right: rook not on {}", color,
				                       rookKingSq);
				state.castlingRights &= ~kingsideFlag;
			}
		}

		// Check queenside rook
		if (state.castlingRights & queensideFlag) {
			if (!PieceHelper::IsOfPiece(board.GetPiece(rookQueenSq), PieceHelper::AsPiece(ROOK, color))) {
				report_repair_noexcept(outWarnings, "Clearing {} queen-side castling right: rook not on {}", color,
				                       rookQueenSq);
				state.castlingRights &= ~queensideFlag;
			}
		}
	}

	// Validate en-passant
	if (state.epSquare != NO_SQUARE) {
		const int epIndex = static_cast<int>(state.epSquare);
		const int file = epIndex % 8;
		const int epRankIndex = epIndex / 8;

		eColor lastMover = (state.sideToMove == WHITE) ? BLACK : WHITE;
		int pawnRankIndex = -1;

		if (lastMover == WHITE) {
			if (epRankIndex != 5) { // rank 3 (index 5)
				report_repair_noexcept(outWarnings,
				                       "Clearing en-passant: ep square rank inconsistent with side to move");
				state.epSquare = NO_SQUARE;
			} else {
				pawnRankIndex = 4;
			}
		} else {                    // lastMover == BLACK
			if (epRankIndex != 2) { // rank 6 (index 2)
				report_repair_noexcept(outWarnings,
				                       "Clearing en-passant: ep square rank inconsistent with side to move");
				state.epSquare = NO_SQUARE;
			} else {
				pawnRankIndex = 3;
			}
		}

		if (pawnRankIndex != -1 && state.epSquare != NO_SQUARE) {
			const eSquare pawnSq = static_cast<eSquare>((pawnRankIndex << 3) + file);
			const ePiece p = board.GetPiece(pawnSq);
			if (!PieceHelper::IsPawn(p) || PieceHelper::Color(p) != lastMover) {
				report_repair_noexcept(outWarnings,
				                       "Clearing en-passant: no pawn of expected color on square for ep capture");
				state.epSquare = NO_SQUARE;
			}
		}
	}

	return true;
}

// Conversion utilities
void FENParser::ToGameConfig(const FENGameState& state, Config::GameConfig& outConfig) noexcept
{
	outConfig.sideToMove = state.sideToMove;
	outConfig.epSquare = state.epSquare;
	outConfig.castlingRights = state.castlingRights;
	outConfig.num50moves = state.halfMoveClock;
	outConfig.fullMoveCounter = state.fullMoveCounter;
}

FENParser::FENGameState FENParser::FromGameConfig(const Config::GameConfig& config) noexcept
{
	FENGameState state;
	state.sideToMove = config.sideToMove;
	state.epSquare = config.epSquare;
	state.castlingRights = config.castlingRights;
	state.halfMoveClock = config.num50moves;
	state.fullMoveCounter = config.fullMoveCounter;
	return state;
}
