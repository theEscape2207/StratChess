#pragma once

#include "BitBoardHelper.h"
#include "HashElement.h"
#include "Move.h"
#include "GameState.h"
#include "SquareHelper.h"
#include "PieceHelper.h"
#include <unordered_map>
#include <span>
#include <tuple>

#include <intrin.h>	// For _BitScanForward64
#pragma intrinsic(_BitScanForward64)

class Board final
{
	friend std::ostream& operator<<(std::ostream&, _In_ const Board&);

	Board();
	~Board();

	using TBitboards = std::array<BITBOARD, ALL_BITBOARDS>;

public:
	//***************************************
	// Method:      Instance
	// Description: Singleton stuff
	// FullName:    public static Board::Instance 
	// Returns:     Board& - 
	// Remark:      
	//***************************************
	static inline Board& Instance() noexcept
	{
		static Board _instance;
		return _instance;
	}

	using sqPieces = std::tuple<ePiece, eSquare>;

	using squareCol = std::vector< sqPieces >;

	void SetDefaultBoard();
	void SetupBoard(_In_ const squareCol&);
	void SetupBoardFromFEN(_In_ const std::string& fen);
	std::string ExtractFENFromBoard() const;
	bool DoMove(_In_ const Move&);
	void UndoMove(_In_ const Move&);

	bool InCheck() const noexcept;

	// Returnerer et view af bitboards arrayet
	std::span<BITBOARD> GetBitBoards() noexcept;

	// Inline funktioner
	//---------------------

	// Returnerer farven paa aktive spiller
	eColor GetCurrentColor() const noexcept			{	return sideToMove_;		}
	void SetInitialColor(eColor color) noexcept		{	sideToMove_ = color;	}

	// Returnerer brikken paa paagaeldende felt
	ePiece GetPiece(_In_ eSquare square) const noexcept {
		return m_iBoard.at(square);
	}
	// Helper
	ePiece GetPiece(_In_ int square) const noexcept {
		return m_iBoard.at(static_cast<eSquare>(square));
	}

	int GetMaterialScore(_In_ eColor color) const noexcept {	return m_MaterialScore[color];	}

	// Returns whether the squares contained in the bit-mask is occupied by a piece
	bool IsOccupied(_In_ BITBOARD mask) const noexcept
	{	
		return Bits::isAnyBitSet(m_bitboards.at(ALL_PIECES), mask);
	}

	// Helper: Is the parameter move legal on the current board
	bool IsLegalMove(_In_ const Move& move)
	{
		if (!DoMove(move)) { // Move will get rolled back if false
			return false;
		}
		UndoMove(move);
		return true;
	}

	// We are always assuming that we have at least one bit left in the mask
	static eSquare GetFirstPiece(BITBOARD mask) noexcept
	{
		unsigned long index;

		const unsigned char isNonzero = _BitScanForward64(&index, mask);
		assert(isNonzero);
		// Found first bit, directly translatable to Square :)
		// Example: 00001100 - first bit is 2
		return static_cast<eSquare>(index);
	}

	Board& operator=(const Board&) = delete;
	Board(const Board&) = delete;
	Board(Board&&) = delete;
	Board& operator=(Board&&) = delete;


	// Hjaelpefunktioner
	void ClearBoard();		// Really clears the board!

	void ClearSquare( _In_ eSquare squareType ) noexcept
	{
		assert(PieceHelper::IsNotEmpty( GetPiece(squareType)));
		m_iBoard[squareType] = ePiece::NO_PIECE;
	}

	void SetSquare( _In_ eSquare squareType, _In_ ePiece piece) noexcept
	{
		assert(PieceHelper::IsNoPiece( GetPiece(squareType)));
		m_iBoard[squareType] = piece;
	}

	// Thin helper around ClearBit - Only for normal Bitboards
	bool ClearBitboardSquare( _In_ TBitboards::size_type iBoard, _In_ eSquare square ) {
		return BitBoardHelper::ClearBitboardMask(m_bitboards[iBoard], g_bbMask[square]);
	}

	// Thin helper around SetBit - Only for normal Bitboards
	void SetBitboardSquare( _In_ TBitboards::size_type iPiece, _In_  eSquare square ) noexcept {
		BitBoardHelper::SetBitboardMask(m_bitboards[iPiece], g_bbMask[square] );
	}

	// Debugfunktion: tester bitboardene for konsistens
	bool TestBitBoards(std::ostream& stream = std::cout) const;
	void PrintAllBitboards(_In_ const TBitboards& bbBitBoards, std::ostream& = std::cout) const;
	
	// These is to be called with the side to move changes, or if in the first
	// position, it is black to move.
	void HashkSwitch() noexcept {
		Bits::toogleBits(curBoardHashKey, 0x21D420B884CD6731U);
	}
	void ChangePlayer() noexcept {
		(sideToMove_ = (sideToMove_ == eColor::WHITE) ? eColor::BLACK : eColor::WHITE);
		HashkSwitch();
	}
	
	constexpr size_t GetBitboard(ePieceType piece, eColor color)
	{
		return static_cast<size_t>(piece) + static_cast<size_t>(color);
	}
	
	void _RemovePiece(_In_ eSquare square, _In_ ePiece piece );
	void _AddPiece(_In_ eSquare square, _In_ ePiece piece);

	//***************************************
	// Method:      AddPieceToBoard
	// Description: Adds a new piece on the board. Used during startup and promotion.
	//				Adjusts material score
	// FullName:    private Board::AddPieceToBoard 
	// Returns:     void
	// Parameter:   ePiece piece - The piece to add
	// Parameter:   eSquare sq - The square to add it on
	//***************************************
	void AddPieceToBoard(_In_ ePiece piece, _In_ eSquare sq )
	{
		_AddPiece( sq, piece );
		m_MaterialScore[ PieceHelper::Color(piece) ] += PieceHelper::Value(piece);
	}

	
	//************************************
	// Method:      RemovePieceFromBoard
	// Description: Remove a slain piece from the board. Also used during promotion.
	//				Maintains material score
	// FullName:    private Board::RemovePieceFromBoard 
	// Returns:     void
	// Parameter:   const Piece& piece - The piece to remove
	// Parameter:   const Square& sq - The square to move it from
	//************************************
	void RemovePieceFromBoard(_In_ ePiece piece, _In_ eSquare sq )
	{
		_RemovePiece( sq, piece );
		m_MaterialScore[ PieceHelper::Color(piece) ] -= PieceHelper::Value(piece);
	}

	//***************************************
	// Method:      MovePiece
	// Description: Helper to move a piece on the board
	// FullName:    private Board::MovePiece 
	// Returns:     void
	// Parameter:   const Piece& piece - The piece to be moved
	// Parameter:   const Square& from - The square to move from
	// Parameter:   const Square& to - The square to move to
	//***************************************
	void MovePiece(_In_ const Move& move)
	{
		//MovePiece(move.MovPiece, move.From, move.To);
		_RemovePiece(move.from(), move.MovPiece);
		_AddPiece(move.to(), move.MovPiece);
	}

	//***************************************
	// Method:      MovePiece
	// Description: Helper to move a piece on the board
	// FullName:    private Board::MovePiece 
	// Returns:     void
	// Parameter:   const Piece& piece - The piece to be moved
	// Parameter:   const Square& from - The square to move from
	// Parameter:   const Square& to - The square to move to
	//***************************************
	void MovePiece(_In_ ePiece piece, _In_ eSquare from, _In_ eSquare to )
	{
		_RemovePiece( from, piece );
		_AddPiece( to, piece );
	}
	

private:
	// ========================================================================
	// Member Variables
	// ========================================================================

	using TBoardSquares = std::array<ePiece, ALL_SQUARES>;
	TBoardSquares m_iBoard {};	// Array af alle pieces paa boardet - et paa hver square
	eColor sideToMove_{ eColor::WHITE };				// Hvis tur er det ?
	GameInfo gameInfo_;

	TBitboards m_bitboards{ { 0 } };

	int m_MaterialScore[2]{ 0 };		// Den materielle score for de to spillere

	// ------------------------------------------------
	// Transposition Table stuff
	// -----------------------------------------------
	// Hash Table for Transposition Table (Note: not used for AI Perplex)
	using TMoveHashTable = std::unordered_map<unsigned int, HashElement>;
	TMoveHashTable hashTable_;
	
	// Zobrist Hash keys
	std::array<std::array<uint64_t, ALL_SQUARES>, ALL_PIECETYPES> allHashKeys;	// Piece Hash key table
	// Current board hash key
	unsigned int curBoardHashKey{ 0 };

public:
	//---------------------------
	// For Transposition tables
	//
	std::pair<int, Move> ProbeHash(_In_ size_t ply, _In_ int alpha, _In_ int beta) const;
	void RecordHash(_In_ size_t ply, _In_ int score, _In_ eHashFlags flags, _In_ const Move& m);

	// Resets all memory in the HashTable
	void ClearHashTable();

	unsigned int GetCurBoardHKey() const noexcept { return curBoardHashKey; }
private:

	//-----------------------------------------------
	// Transposition Table
	//

	// Initialiserer alle enkelte hashkey-vaerdier
	void InitHashkey();
	
	void SetCurBoardHKey(_In_ unsigned int newkey) noexcept {	curBoardHashKey = newkey;	}
};
