#include "StdAfx.h"
#include "FENParser.h"
#include "Board.h"
#include "PieceHelper.h"
#include <regex>
#include <sstream>
#include <array>
#include <optional>
#include <algorithm>

/*
FEN specifies the piece placement, the active color, the castling availability, the en passant target square, the halfmove clock, and the fullmove number.
These can all fit on a single text line in an easily read format.
The length of a FEN position description varies somewhat according to the position.
In some cases, the description could be eighty or more characters in length and so may not fit conveniently on some displays.
However, these positions aren't too common.
A FEN description has six fields. Each field is composed only of non-blank printing ASCII characters.
Adjacent fields are separated by a single ASCII space character.

<FEN> ::=  <Piece Placement>
' ' <Side to move>
' ' <Castling ability>
' ' <En passant target square>
' ' <Halfmove clock>
' ' <Fullmove counter>

Here's the FEN for the starting position:

rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1

And after the move 1. e4:

rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1

--- Other strings
5k2/ppp5/4P3/3R3p/6P1/1K2Nr2/PP3P2/8 b - - 1 32

Regex validation
\s*([rnbqkpRNBQKP1-8]+\/){7}([rnbqkpRNBQKP1-8]+)\s[bw-]\s(([a-hkqA-HKQ]{1,4})|(-))\s(([a-h][36])|(-))\s\d+\s\d+\s*
*/
std::optional<std::string> FENParser::ParseFEN(const std::string& fen, Config::GameConfig& outConfig, std::vector<std::tuple<ePiece, eSquare>>& outPieces) noexcept
{
	// Trim input
	std::string s = fen;
	while (!s.empty() && isspace((unsigned char)s.front())) s.erase(s.begin());
	while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();

	// Quick overall format check with regex: piece placement, side, castling, ep, halfmove, fullmove
	static const std::regex fenRx(R"(^\s*([rnbqkpRNBQKP1-8]+\/){7}([rnbqkpRNBQKP1-8]+)\s+[wb]\s+(-|[KQkq]+)\s+(-|[a-h][36])\s+\d+\s+\d+\s*$)");
	if (!std::regex_match(s, fenRx))
	{
		return std::string("overall format invalid");
	}

	// Tokenize by whitespace
	std::istringstream iss(s);
	std::vector<std::string> parts;
	std::string token;
	while (iss >> token) parts.emplace_back(token);

	if (parts.size() < 4) {
		return std::string("too few fields in FEN");
	}

	const std::string& pieceField = parts[0];

	// Validate rank expansion to 8 files before deeper parse
	{
		std::istringstream rankss(pieceField);
		std::string rankToken;
		std::vector<std::string> ranks;
		while (std::getline(rankss, rankToken, '/')) ranks.emplace_back(rankToken);
		if (ranks.size() != 8) {
			return std::string("expected 8 ranks in piece placement");
		}
		for (size_t i = 0; i < ranks.size(); ++i) {
			int files = 0;
			for (char c : ranks[i]) {
				if (std::isdigit(static_cast<unsigned char>(c))) files += c - '0';
				else files += 1;
			}
			if (files != 8) {
				return std::string("rank ") + std::to_string(i) + " expands to " + std::to_string(files) + " files (expected 8)";
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
	outConfig.color = (side[0] == 'b') ? eColor::BLACK : eColor::WHITE;

	// Castling rights
	if (auto err = populateCastlingFlags(parts[2], outConfig))
	{
		return err;
	}

	// En-passant
	const std::string& ep = parts[3];
	if (ep != "-") {
		if (ep.size() != 2 || ep[0] < 'a' || ep[0] > 'h' || (ep[1] != '3' && ep[1] != '6')) {
			return std::string("invalid en-passant square");
		}
		outConfig.epSquare = SquareFromString(ep);
	}

	// Halfmove clock (optional)
	if (parts.size() >= 5) {
		try {
			int half = std::stoi(parts[4]);
			outConfig.num50moves = std::max(0, half);
		}
		catch (...) {
			return std::string("invalid halfmove clock");
		}
	}

	// Fullmove number (optional)
	if (parts.size() >= 6) {
		try {
			int full = std::stoi(parts[5]);
			outConfig.fullMoveCounter = std::max(1, full);
		}
		catch (...) {
			return std::string("invalid fullmove counter");
		}
	}

	return std::nullopt; // success
}

std::optional<std::string> FENParser::populateCastlingFlags(const std::string& castling, Config::GameConfig& outConfig) noexcept
{
	// Castling rights - ensure only KQkq and no duplicates then populate outConfig
	if (castling != "-")	// no more castling rights left
	{	
		std::array<bool, 256> seen = {};
		for (char c : castling) {
			if (c != 'K' && c != 'Q' && c != 'k' && c != 'q') {
				return std::string("invalid castling character encountered");
			}
			if (seen[static_cast<unsigned char>(c)]) {
				return std::string("duplicate castling character encountered");
			}
			seen[static_cast<unsigned char>(c)] = true;
		}
		// Populate outConfig flags
		outConfig.whitekingcastle = (castling.find('K') != std::string::npos);
		outConfig.whitequeencastle = (castling.find('Q') != std::string::npos);
		outConfig.blackkingcastle = (castling.find('k') != std::string::npos);
		outConfig.blackqueencastle = (castling.find('q') != std::string::npos);
	}
	return std::nullopt; // success
}

/*
The first field represents the placement of the pieces on the board.
The board contents are specified starting with the eighth rank and ending with the first rank.
For each rank, the squares are specified from file a to file h.
White pieces are identified by uppercase SAN piece letters ("PNBRQK") and
black pieces are identified by lowercase SAN piece letters ("pnbrqk").
Empty squares are represented by the digits one through eight,
the digit used represents the count of contiguous empty squares along a rank.
A solidus character "/" is used to separate data of adjacent ranks.
*/
std::optional<std::string> FENParser::ParsePiecePlacementField(const std::string& placement, std::vector<std::tuple<ePiece, eSquare>>& outVec) noexcept
{
	outVec.clear();

	// Split ranks by '/'
	std::vector<std::string> ranks;
	{
		std::istringstream ss(placement);
		std::string token;
		while (std::getline(ss, token, '/')) ranks.emplace_back(token);
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
			}
			else {
				if (fileIndex >= 8) {
					return std::string("too many files in rank");
				}

				ePiece piece = ePiece::NO_PIECE;
				switch (c) {
				case 'K': piece = ePiece::WHITE_KING; ++whiteKingCount; break;
				case 'Q': piece = ePiece::WHITE_QUEEN; break;
				case 'R': piece = ePiece::WHITE_ROOK; break;
				case 'B': piece = ePiece::WHITE_BISHOP; break;
				case 'N': piece = ePiece::WHITE_KNIGHT; break;
				case 'P': piece = ePiece::WHITE_PAWN; ++whitePawnCount; break;
				case 'k': piece = ePiece::BLACK_KING; ++blackKingCount; break;
				case 'q': piece = ePiece::BLACK_QUEEN; break;
				case 'r': piece = ePiece::BLACK_ROOK; break;
				case 'b': piece = ePiece::BLACK_BISHOP; break;
				case 'n': piece = ePiece::BLACK_KNIGHT; break;
				case 'p': piece = ePiece::BLACK_PAWN; ++blackPawnCount; break;
				default:
					return std::string("invalid piece character in placement");
				}

				// Pawns cannot appear on first or last rank (rank indices 0==8th, 7==1st)
				if ((piece == ePiece::WHITE_PAWN || piece == ePiece::BLACK_PAWN) && (rank == 0 || rank == 7)) {
					return std::string("pawn on invalid rank");
				}

				// Compute square index consistent with existing code: rank 0 == 8th rank
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

// Converting the field referrrer in string format "e3" to the corresponding eSquare
eSquare FENParser::SquareFromString(const std::string& s) noexcept
{
	if (s.size() != 2) return NO_SQUARE;
	const char file = s[0];
	const char rankChar = s[1];
	if (file < 'a' || file > 'h' || rankChar < '1' || rankChar > '8') return NO_SQUARE;
	const unsigned int row = file - 'a';
	const unsigned int col = ((8 - (rankChar - '0')) << 3);
	return static_cast<eSquare>(col + row);
}

// TODO: This function interacts with Board state, so it may be better suited elsewhere.
bool FENParser::ValidatePositionAgainstFENMetadata(Config::GameConfig& outConfig) noexcept
{
	Board& board = Board::Instance();

	// Validate white castling rights: king must be on e1 and rooks on a1/h1
	if (outConfig.whitekingcastle || outConfig.whitequeencastle) {
		if (!PieceHelper::IsKing(board.GetPiece(e1)) || PieceHelper::Color(board.GetPiece(e1)) != WHITE) {
			spdlog::default_logger()->warn("Clearing white castling rights: king not on e1");
			outConfig.whitekingcastle = outConfig.whitequeencastle = false;
		}
		else {
			if (outConfig.whitekingcastle) {
				if (!PieceHelper::IsOfPiece(board.GetPiece(h1), PieceHelper::AsPiece(ROOK, WHITE))) {
					spdlog::default_logger()->warn("Clearing white king-side castling right: rook not on h1");
					outConfig.whitekingcastle = false;
				}
			}
			if (outConfig.whitequeencastle) {
				if (!PieceHelper::IsOfPiece(board.GetPiece(a1), PieceHelper::AsPiece(ROOK, WHITE))) {
					spdlog::default_logger()->warn("Clearing white queen-side castling right: rook not on a1");
					outConfig.whitequeencastle = false;
				}
			}
		}
	}

	// Validate black castling rights: king must be on e8 and rooks on a8/h8
	if (outConfig.blackkingcastle || outConfig.blackqueencastle) {
		if (!PieceHelper::IsKing(board.GetPiece(e8)) || PieceHelper::Color(board.GetPiece(e8)) != BLACK) {
			spdlog::default_logger()->warn("Clearing black castling rights: king not on e8");
			outConfig.blackkingcastle = outConfig.blackqueencastle = false;
		}
		else {
			if (outConfig.blackkingcastle) {
				if (!PieceHelper::IsOfPiece(board.GetPiece(h8), PieceHelper::AsPiece(ROOK, BLACK))) {
					spdlog::default_logger()->warn("Clearing black king-side castling right: rook not on h8");
					outConfig.blackkingcastle = false;
				}
			}
			if (outConfig.blackqueencastle) {
				if (!PieceHelper::IsOfPiece(board.GetPiece(a8), PieceHelper::AsPiece(ROOK, BLACK))) {
					spdlog::default_logger()->warn("Clearing black queen-side castling right: rook not on a8");
					outConfig.blackqueencastle = false;
				}
			}
		}
	}

	// Validate en-passant: must be consistent with a pawn that moved two squares last move
	if (outConfig.epSquare != NO_SQUARE) {
		const int epIndex = static_cast<int>(outConfig.epSquare);
		const int file = epIndex % 8;
		const int epRankIndex = epIndex / 8; // 0 == rank 8, 7 == rank 1

		// Determine last mover
		eColor lastMover = (outConfig.color == WHITE) ? BLACK : WHITE;
		int pawnRankIndex = -1;
		if (lastMover == WHITE) {
			// white double-step leaves ep target on rank 6? (FEN uses human ranks: white double-step sets ep on rank 6? Wait: standard: if white moves pawn from rank 2 to 4, ep target is rank 3)
			// Here we follow earlier logic: en-passant squares allowed are ranks 3 or 6. if last mover was white, ep must be rank 3 (index 5)
			if (epRankIndex != 5) {
				spdlog::default_logger()->warn("Clearing en-passant: ep square rank inconsistent with side to move");
				outConfig.epSquare = NO_SQUARE;
			}
			else {
				pawnRankIndex = 4; // pawn that moved would be on rank index 4
			}
		}
		else { // lastMover == BLACK
			if (epRankIndex != 2) {
				spdlog::default_logger()->warn("Clearing en-passant: ep square rank inconsistent with side to move");
				outConfig.epSquare = NO_SQUARE;
			}
			else {
				pawnRankIndex = 3;
			}
		}

		if (pawnRankIndex != -1 && outConfig.epSquare != NO_SQUARE) {
			const eSquare pawnSq = static_cast<eSquare>((pawnRankIndex << 3) + file);
			const ePiece p = board.GetPiece(pawnSq);
			if (!PieceHelper::IsPawn(p) || PieceHelper::Color(p) != lastMover) {
				spdlog::default_logger()->warn("Clearing en-passant: no pawn of expected color on square for ep capture");
				outConfig.epSquare = NO_SQUARE;
			}
		}
	}

	// Always return true: metadata was validated and adjusted if needed.
	return true;
}