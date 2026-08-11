#pragma once

#include "PlayerAI.h"

class PlayerAiIterBase : public PlayerAiBase
{
public:
	~PlayerAiIterBase() = default;
	PlayerAiIterBase(const PlayerAiIterBase&) = delete;
	PlayerAiIterBase& operator=(const PlayerAiIterBase&) = delete;
	PlayerAiIterBase(PlayerAiIterBase&&) = delete;
	PlayerAiIterBase& operator=(PlayerAiIterBase&&) = delete;
protected:
	// Force use of factory by preventing constructor, copy-construction & operator=
	explicit PlayerAiIterBase(Board& board, unsigned md)
		: PlayerAiBase(board, md)
	{
		m_infoSeq.reserve(32);
	}

	// Iter classes store the currently best move in the PVL, so maintain the list there
	void InitMoveVariables(const GameInfo& info) override
	{
		m_SearchCount = 0;					// Clear search counter

		m_infoSeq.clear();					// resets parent boardInfo sequence
		m_infoSeq.emplace_back(info);		// Store boardInfo from after last move

		// remove the first two ply from the line in preparation for the next move
		// we are not clearing due to we want this to seed our search (TODO: get data)
		if (m_Line.size() >= 2)
		{
			// TODO: Make sure that move no 2 IS the info.LastMove. Otherwise
			m_Line.erase(m_Line.begin(), m_Line.begin() + 2);
		}
	}

	const Move* GetIterMove(size_t currentPly) const
	{
		if (m_Line.empty() || currentPly >= m_Line.size())
			return nullptr;
		return &m_Line.at(currentPly);
	}

	// ************************************
	// Method:     GetBestMove
	// Description: 
	// FullName:   protected PlayerAiIterBase::GetBestMove 
	// Returns:    const Move - The best move or EmptyMove if game is over
	// Parameter:  BoardInfo& info - 
	// Remark:    
	// ************************************
	Move GetBestMove(GameInfo& info) noexcept override
	{
		// Returner det bedste traek - hvis der er noget
		if (!m_Line.empty())
		{
			info.UpdateBoardInfo(m_Line.front(), m_Board.GetEffectiveMovPiece(m_Line.front()));
			return m_Line.front();		// Henter det foerste traek fra PVL
		}

		// No moves found!
		assert(info.gameState != GameStates::STILL_PLAYING);
		return Move::EmptyMove();
	}

	constexpr bool IsOutsideWindow(int score, int alpha, int beta) noexcept
	{
		return (score <= alpha) || (score >= beta);
	}

	/* Protected variables */
	PVLine m_Line;			// For iterative
	size_t depth_{ 0 };		// For iterative
};
