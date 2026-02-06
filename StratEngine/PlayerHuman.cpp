// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"

#include <string>
#include <map>
#include <regex>
#include <algorithm>
#include "Utils\StrHelper.h"
#include "PlayerHuman.h"
#include "Board.h"
#include "MoveGenerator.h"
#include <MoveHelper.h>


// TODO: Add support for official input of castling moves (e.g. "0-0" or "0-0-0") 
// Right now its only possible through e1g1 (short) or e1-c1(long)
Move PlayerHuman::GetMove(_Inout_ GameInfo& info)
{
	Board& board = Board::Instance();
	MoveList moveList;
	
	if (!IsAnyLegalMoves(info, moveList))
	{
		// No legal moves left, bye!
		spdlog::default_logger()->info("Human has no legal moves left");
		if (board.InCheck())
		{
			EGameStateChanged.fire(this, board.GetCurrentColor() == WHITE ? GameStates::BLACK_WON : GameStates::WHITE_WON);
			this->_bestScore = -GameValues::Mate;

			return Move::EmptyMove();
		}
		// Remis: Godt hvis vi er bagud, men skidt hvis vi er foran
		EGameStateChanged.fire(this, GameStates::DRAW_PAT);
		return Move::EmptyMove();
	}

	const bool bInCheck = board.InCheck();
	std::string strOrg;

	// vi fortsaetter indtil traekket er lovligt - eller der quittes
	for (;;)
	{
		std::stringstream moveMsg;
		if (bInCheck)
			moveMsg << "Du er skak!!\n";
		moveMsg << "Dit traek: (format: e2e4, a7-e3, D7-D8Q)\nTraek> ";
		spdlog::default_logger()->warn(moveMsg.str());
		std::cin >> strOrg;

		// We only want _trimmed_ lower case all over
		const std::string strMove = StringHelper::toLower(StringHelper::trim(strOrg));	// Save the original input

		// mulighed for at skrive "quit" eller "exit" for at quitte
		if (strMove == "exit" || strMove == "quit")
		{
			spdlog::default_logger()->warn("User quitted");
			EGameStateChanged.fire(this, GameStates::HUMAN_EXITED);
			return Move::EmptyMove();
		}

		if (!ValidateInput(strMove))
		{
			std::stringstream sstream;
			sstream << "Ugyldigt input: " << strOrg << "\nProev igen\n";
			spdlog::default_logger()->info(sstream.str());
			continue;
		}

		// User input is validated. Now Parse user input
		Move userMove;
		const bool userPromote = ParseInput(strMove, userMove);

		auto moveIt = std::find(moveList.begin(), moveList.end(), userMove);
		if (moveIt == moveList.end())
		{
			// Traekket blev ikke fundet af computeren, saa deeet...
			std::stringstream sstream;
			sstream << "Traekket: " << strOrg << " er ikke lovligt!\n";
			spdlog::default_logger()->warn(sstream.str());
			continue;
		}

		// Special: Er det en Promotion? Saa er der 4 valgmuligheder!
		if (MoveHelper::IsPromote(*moveIt))
		{
			// There are four different moves. We only want one!
			if (userPromote) {
				// User specified a piece selection - is this it?
				if (moveIt->MovPiece != PieceHelper::AsPiece(userMove.MovPiece, PieceHelper::Color(moveIt->MovPiece)))
					continue;	//Nope - not this one
			}
			else {
				// User did not specify a selection. Then give him a queen!
				if (moveIt->MovPiece != PieceHelper::AsPiece(QUEEN, PieceHelper::Color(moveIt->MovPiece)))
					continue;	//Nope - not this one
			}
		}

		// Tjekker traekket for lovlighed(dvs ikke skak osv). Returnerer hvis OK
		// TODO: IsLegalMove bliver kaldt i IsAnyLegalMoves(), men illegale traek bliver ikke fjernet
		if (board.IsLegalMove(*moveIt))
		{
			info.UpdateBoardInfo(*moveIt );
			return *moveIt;
		}
	}
}

// Validates the user input using Poco regex
// Only lower case input - must be converted before
bool PlayerHuman::ValidateInput(_In_ const std::string& strInput)
{
	// We are allowing the row as lower case and the col as a number with an optional '-' in between
	// Also optional promotional choices i.e. Queen, Rook, Bishop or Knight
	// Allowed: e2e4, E2e4, e2-E4, e2-E4Q, D7D8R
	// Must be between 4 and 6 chars: letter-digit-(optional '-') letter-digit (optional promotional)
	static const std::regex regPiece(R"([a-h][1-8]-?[a-h][1-8](q|r|b|n)?)");
	return std::regex_match(strInput, regPiece);
}

// Returns true if we are trying a promotion
// Expects parameter input to be lower case
// The returned Move has a generic type
// Returns true if the user specified an allowed promotional character
bool PlayerHuman::ParseInput(_In_ const std::string& input,
	_Inout_ Move& move
)
{
	auto curIt = input.begin();
	const auto end = input.end();
	eSquare from = GetSquare(curIt, end);
	eSquare to = GetSquare(curIt, end);
	move.SetMove(from, to, MoveType::QUIET);	// Generic type for now
	
	// No promotion this time?
	if (curIt == end)
	{
		move.MovPiece = ePiece::NO_PIECE;
		return false;
	}

	// What did he ask for? We must have it in our map! Otherwise should be stopped by regex earlier
	MapPieces::const_iterator cit = PlayerHuman::GetPromoteMap().find(*curIt);
	move.MovPiece = static_cast<ePiece>(cit->second);
	return true;
}

// Returns true if any legal move is found, and false otherwise
// TODO: Remove illegal moves from ComputeLegalMoves?
bool PlayerHuman::IsAnyLegalMoves(_In_ const GameInfo& info, _Out_ MoveList& moveList)
{
	MoveGenerator::ComputeLegalMoves(info, moveList);

	return std::any_of(
		moveList.begin(),
		moveList.end(),
		[](const Move & move) {
		return Board::Instance().IsLegalMove(move);
	});
}
