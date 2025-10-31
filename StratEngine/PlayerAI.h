#pragma once
#include "PlayerBase.h"
#include "Board.h"		// includes Move
#include "Eval.h"

#include <vector>
#include <sstream>
#include <chrono>

//#include <Poco/Stopwatch.h>

class PlayerAiBase : public PlayerBase
{
public:
	~PlayerAiBase() = default;

	std::string getDescription() const override {
		std::stringstream str;
		str << "\n\tEngine type:\t" << GetType() << 
			"\n\tDepth:\t\t" << m_MaxDepth <<
			"\n\tEvaluation:\t" << Eval->GetType() << '\n';
		return str.str();
	}
	const char* GetType() const noexcept override		{ return "AI";	}

	PlayerAiBase(const PlayerAiBase&) = delete;
	PlayerAiBase& operator=(const PlayerAiBase&) = delete;
	PlayerAiBase(PlayerAiBase&&) = delete;
	PlayerAiBase& operator=(PlayerAiBase&&) = delete;

protected:
	// Force use of factory by
	// Preventing constructor, copy-construction & operator=
	explicit PlayerAiBase(unsigned md):
			m_Board(Board::Instance()), 
			m_MaxDepth(md)
		 {
			 // Create the Evaluation strategy - Right now only possible to select two: SIMPLE and COMPLEX ;-)
		 }

		 // default constructor - not used but needs to be there
		 PlayerAiBase() noexcept:
			 m_Board(Board::Instance()),
			 m_MaxDepth(5) // some default
		 {

		 }

	//virtual Move GetMove(_Inout_ GameInfo& info) = 0;

	/* AI helper methods */

	// Quiescent soegning modvirker horisont-effekten
	int Quiescent(size_t, int, int );

	// Registers the amount of used time and prints out if PRINT_STATS is set
	void StopTimer() const;

	// Tilfoejer dette traek til nuvaerende traekfoelge	
	// Sletter eksisterende traek fra listen fra denne ply og ned
	// Benyttes til Last Move sorting - Saetter parent node
	void AddMoveToSeq( const Move& move, size_t ply );

	// Returns the best first move currently found
	virtual Move GetBestMove(_In_ GameInfo& info) noexcept;

	/*
	*		Inline methods
	*/

	// StartTimer
	void StartTimer()							{	_startingTime = std::chrono::high_resolution_clock::now();	}

	void SetEvalEngine(EvalManager::EvalTypes type) override
	{
		Eval = EvalManager::Create(type);	// create new eval
	}

	static void PrintMovesAndScore(std::ostream& stream, size_t numMove, size_t TotalCount, const Move& move, int score )
	{
		// Udskriver traekkene til fil
		stream << "Move " << numMove + 1 << " of " << TotalCount << '\n' //-V128
			<< move;
		if (score != (-GameValues::Search_Init -1))
			stream << "Score: " << score << "\n\n";
		else
			stream << "Invalid move!" << "\n\n";
	}

	// ************************************
	// Method:      InitMoveVariables
	// Description: 
	// FullName:    protected PlayerAiBase::InitMoveVariables
	// Returns:     void - 
	// Parameter:   const GameInfo& info - 
	// Remark:      TODO: Burde flyttes ned som en template metode DoInit efter m_MoveSeq
	//	  		    Non Iter edition
	// ************************************
	virtual void InitMoveVariables(_In_ const GameInfo& info )
	{
		m_SearchCount = 0;

		// nulstiller parent boardInfo-sekvensen
		// Store boardInfo from after last move
		m_infoSeq.clear();
		m_infoSeq.emplace_back( info );

		// Nulstiller best move
		m_BestMove.Clear();
	}

	// Returns the Current Move's predecessor in the current move sequence tree
	const Move& GetParentMove(size_t currentPly) const	
	{
		return GetLastBoardInfo(currentPly).lastMove; 
	}

	const GameInfo& GetLastBoardInfo( size_t currentPly ) const
	{
		// This must always contain the last move, hence the one extra info 
		return m_infoSeq.at( currentPly );
	}

	// ************************************
	// Method:      UpdateGameState
	// Description: Updates the current game state at the top level
	// FullName:    protected PlayerAiBase::UpdateGameState
	// Returns:     void
	// Parameter:   unsigned int currentPly - the current depth
	// Parameter:   const GameStates& newState - The new state
	// Remark:      
	// ************************************
	void UpdateGameState( size_t currentPly, GameStates newState )
	{
		if ( currentPly == 0 )
		{
			GameInfo& info = m_infoSeq.at( currentPly );
			if (newState != info.gameState)
			{
				info.gameState = newState;
			}
		}
	}

	void CheckGameOver( GameInfo &info )
	{
		info = GetLastBoardInfo(0);
		if (info.gameState != GameStates::STILL_PLAYING)
		{
			EGameStateChanged.fire(this, info.gameState);
		}
	}

	// ************************************
	// Method:      IsFiftyMoves
	// Description: Test for 50 moves rules
	// FullName:    protected PlayerAiBase::IsFiftyMoves 
	// Returns:     bool - true if 
	// Parameter:   const BoardInfo& info - 
	// Remark:		
	// ************************************
	static bool IsFiftyMoves( const GameInfo& info) noexcept
	{
		if ( info.fiftyCount >= 50 )
		{
			assert( info.gameState == GameStates::DRAW_50_MOVES );
			return true;
		}
		return false;
	}

	/*
	*	Protected Variables	- used by nested classes
	*/
	// Debug: taeller hvor mange gange Alpha-Beta koeres igennem
	size_t m_SearchCount{ 0 };
	// Lokal reference til Board
	Board& m_Board;

	// The embedded Eval object for per-player evaluation
	std::unique_ptr<EvalManager> Eval;

	// Gemmer en sequence af BoardInfo for Do/Undo
	std::vector< GameInfo > m_infoSeq;

	// Det bedste traek indtil nu
	Move m_BestMove;

	size_t m_MaxDepth{ 0 };	// Max seeking depth for this algorithm

	std::chrono::time_point<std::chrono::high_resolution_clock> _startingTime;

//#ifdef PRINT_STATS

	// Samlet tid og antal nodes for begge computerspillere - TODO: Separer evt til per spiller. Human burde ogsaa have en klokke
	static std::chrono::milliseconds m_TotalTime;
	static size_t m_TotalCount;

//#endif	// PRINT_STATS
};
