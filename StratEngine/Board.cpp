// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "Board.h"
#include "MoveGenerator.h"
#include "MoveHelper.h"
#include "PieceHelper.h"
#include <random>

extern std::ofstream outLegalMoves;

Board::Board()
{
	// Fylder Keys med rand64 
	InitHashkey();

	m_iBoard.fill(ePiece::NO_PIECE);	// Default: no piece on all square. Board setup comes later
}

Board::~Board() 
{
	try
	{
		// Only needs to get deleted if we actually uses the hash table - but no danger either
		ClearHashTable();
	}
	catch (const std::exception&)
	{
		// Don't care if it happens, we are closing up here.
	}
}

// Toemmer de forskellige boards
void Board::ClearBoard()
{
	m_bitboards.fill(0);

	m_iColor = WHITE;

	m_MaterialScore[WHITE] = m_MaterialScore[BLACK] = 0;
	//m_PlaceScore[WHITE] = m_PlaceScore[BLACK] = 0;
	
	SetCurBoardHKey(0);
	
	spdlog::default_logger()->info("Board cleared" );
}

// ************************************
// Method:      _AddPiece
// Description: Tilfoejer brik fra de forskellige bitboards
//				 Med tjek i debug om der allerede staar en brik
// FullName:    private Board::_AddPiece
// Returns:     void - 
// Parameter:   const Square& square - 
// Parameter:   const Piece& piece - 
// Remark:      
// ************************************
void Board::_AddPiece(_In_ eSquare square, _In_ ePiece piece )
{
	// Tilfoejer brikken paa dens eget bitboard
	SetBitboardSquare( piece, square);
		
	// Her tilfoejes brikken til bitboardet, der indeholder alle brikker i farven
	// Der goeres brug af, at hvide brikker har lige indeks og omvendt
	// Dvs 12, der er ALL_WHITE eller 13, der er ALL_BLACK
	SetBitboardSquare(GetBitboard(ALL_FROM_COLOR, PieceHelper::Color(piece)), square);
	
	// Tilfoejer brikken paa ALLE_BRIKKER bitboardet
	SetBitboardSquare(ALL_PIECES, square);

	// Tilfoejer brikken paa de rotated bitboards
	BitBoardHelper::SetBitboardMask(m_bitboards[ROTATED90], g_bbMaskRotated90[square]);
	BitBoardHelper::SetBitboardMask(m_bitboards[ROTATED45R], g_bbMaskRotated45R[square]);
	BitBoardHelper::SetBitboardMask(m_bitboards[ROTATED45L], g_bbMaskRotated45L[square]);

	// Tilfoejer brikkens vaerdi fra Board hash table
	curBoardHashKey		^= allHashKeys[piece][square];

	// Et almindelige array opdateres ogsaa
	SetSquare(square, piece);

	// Test af bitboard-konsistens 
	assert(TestBitBoards(outLegalMoves));
}


//////////////////////////////////////////////////////////
// Fjerner brik fra de forskellige bitboards
//
// Med tjek paa om der rent faktisk staar en brik paa paagaeldende felt 
void Board::_RemovePiece(_In_ eSquare square, _In_ ePiece piece)
{
	// Fjerner brikken paa dens eget bitboard
	if( !ClearBitboardSquare(piece, square) )
		TestBitBoards(std::cout );
	
	// Fjerner brikken paa farvens eget samlede bitboard
	// Der goeres brug af, at hvide brikker har lige indeks og omvendt
	// Dvs 12, der er ALL_WHITE eller 13, der er ALL_BLACK
	ClearBitboardSquare(GetBitboard(ALL_FROM_COLOR, PieceHelper::Color(piece)), square);
	
	// Fjerner brikken paa ALLE_BRIKKER bitboardet
	ClearBitboardSquare(ALL_PIECES, square);

	// Fjerner brikken paa de forskellige rotated bitboards
	BitBoardHelper::ClearBitboardMask(m_bitboards[ROTATED90], g_bbMaskRotated90[square]);
	BitBoardHelper::ClearBitboardMask(m_bitboards[ROTATED45R], g_bbMaskRotated45R[square]);
	BitBoardHelper::ClearBitboardMask(m_bitboards[ROTATED45L], g_bbMaskRotated45L[square]);

	// Fjerner brikkens vaerdi fra Board hash table
	curBoardHashKey		^= allHashKeys[piece][square];
	
	ClearSquare(square);

	// Ekstra Test af bitboard-konsistens 
	assert(TestBitBoards(outLegalMoves));
}


// Sets up the Board as per the data from XML file
//***************************************
// Method:      SetupBoard
// Description: Setup the custom board using the supplied 
// FullName:    public Board::SetupBoard 
// Returns:     void - 
// Parameter:   const squareCol& col - 
// Remark:      TODO: We should do some validation on the newly created setup
//***************************************
void Board::SetupBoard(_In_ const squareCol& col )
{
	ClearBoard();

	for (const auto &sqPiece : col)
	{
		AddPieceToBoard( std::get<0>(sqPiece), std::get<1>(sqPiece) );
	}

	spdlog::default_logger()->info("Custom board set up" ); 

	// FIXME: We should do some further validation on the newly created setup

}
// Setup the default board
void Board::SetDefaultBoard()
{
	// Empty the board
	ClearBoard();

	// Black officers
	AddPieceToBoard(ePiece::BLACK_ROOK,		a8);
	AddPieceToBoard(ePiece::BLACK_KNIGHT,	b8);
	AddPieceToBoard(ePiece::BLACK_BISHOP,	c8);
	AddPieceToBoard(ePiece::BLACK_QUEEN,	d8);
	AddPieceToBoard(ePiece::BLACK_KING,		e8);
	AddPieceToBoard(ePiece::BLACK_BISHOP,	f8);
	AddPieceToBoard(ePiece::BLACK_KNIGHT,	g8);
	AddPieceToBoard(ePiece::BLACK_ROOK,		h8);
		
	// Sorts boender
	for( int i=a7; i<=h7; i++ )
	{
		AddPieceToBoard(ePiece::BLACK_PAWN, static_cast<eSquare>(i));
	}

	// Hvids boender
	for( int i=a2; i<=h2; i++)
	{
		AddPieceToBoard( ePiece::WHITE_PAWN, static_cast<eSquare>( i ) );
	}
	
	// Resten af hvids brikker
	AddPieceToBoard(ePiece::WHITE_ROOK,		a1);
	AddPieceToBoard(ePiece::WHITE_KNIGHT,	b1);
	AddPieceToBoard(ePiece::WHITE_BISHOP,	c1);
	AddPieceToBoard(ePiece::WHITE_QUEEN,	d1);
	AddPieceToBoard(ePiece::WHITE_KING,		e1);
	AddPieceToBoard(ePiece::WHITE_BISHOP,	f1);
	AddPieceToBoard(ePiece::WHITE_KNIGHT,	g1);
	AddPieceToBoard(ePiece::WHITE_ROOK,		h1);

	spdlog::default_logger()->info("Default Board Set up" );
}

// ************************************
// Method:      GetBitBoards
// Description: returns a view of all bitboards
// FullName:    public Board::GetBitBoards
// Returns:     gsl::span<BITBOARD> 
// Parameter:   -
// Remark:      
// ************************************
std::span<BITBOARD> Board::GetBitBoards() noexcept
{
	return std::span(m_bitboards);
}


// Foretager et traek
bool Board::DoMove(_In_ const Move& m)
{
	assert( MoveHelper::IsValid( m ));
	assert(GetCurrentColor() == PieceHelper::Color(m.MovPiece) );

	// Hvilken type slag er det ?
	switch ( m.Type )
	{
	case MoveType::Normal:
		assert(!PieceHelper::IsActual(m.Content));
		MovePiece( m );
		break;
	case MoveType::Capture:
		assert( MoveHelper::IsCapture( m ));
		// Fjerner brikken, der bliver slaaet. 
		RemovePieceFromBoard( m.Content, m.To );
		MovePiece(m);
		break;
	case MoveType::PawnTwoForward:
		// Kun boender
		assert(!PieceHelper::IsActual(m.Content));	// ingen slag
		assert(MoveHelper::IsPawnMove(m));
		MovePiece(m);
		break;
	case MoveType::En_Passant:
		// Det er et en-passant slag
		assert(MoveHelper::IsPawnMove(m));
		assert(MoveHelper::IsCapture(m) && PieceHelper::IsPawn(m.Content));	// slaar altid en bonde
		// Saa skal modstanderens bonde fjernes fra det rigtige felt
		if (m_iColor == WHITE)
			// Fjerner den sorte brik paa feltet nedenunder
			RemovePieceFromBoard(ePiece::BLACK_PAWN, SquareHelper::Calc(m.To, + ONE_ROW));
		else
			// Fjerner den hvide brik paa feltet ovenover
			RemovePieceFromBoard( ePiece::WHITE_PAWN, SquareHelper::Calc(m.To, -ONE_ROW ));

		MovePiece(m);
		break;
	case MoveType::PromoteCapture:
		assert(!MoveHelper::IsPawnMove(m));	// Its written as the new piece is moving
		assert(MoveHelper::IsCapture(m));
		RemovePieceFromBoard(m.Content, m.To);	// Capture - remove then captured piece
		// Promote - Remote Pawn from the From field
		RemovePieceFromBoard( PieceHelper::AsPiece(PAWN, m_iColor), m.From);
		// Den valgte brik saettes paa det nye felt
		AddPieceToBoard(m.MovPiece, m.To);	// Promote: Add the new Piece
		break;
	case MoveType::Promote:
		assert(!MoveHelper::IsPawnMove(m));	// Its written as the new piece is moving
		assert(!MoveHelper::IsCapture(m));
		// Fjerner bonden fra det gamle felt
		RemovePieceFromBoard( PieceHelper::AsPiece(PAWN, m_iColor ), m.From );
		// Den valgte brik saettes paa det nye felt
		AddPieceToBoard( m.MovPiece, m.To );
		break;
	case MoveType::Castling:
		assert(m.From == e1 || m.From == e8);	// must be in starting position
		assert(PieceHelper::IsKing(GetPiece(m.From)));
		assert(MoveHelper::IsKingMove(m));

		switch( m.To )
		{
		case g1:	// Short castling
		case g8:	
			// Move Rook at h1|h8 to f1|f8
			assert(PieceHelper::IsNoPiece(GetPiece(m.From + 1)));
			assert(PieceHelper::IsNoPiece(GetPiece(m.From + 2)));
			assert(PieceHelper::IsOfPiece(GetPiece(m.From + 3), PieceHelper::AsPiece(ROOK, m_iColor)));
			MovePiece( PieceHelper::AsPiece(ROOK, m_iColor),
				SquareHelper::Calc(m.To, +1), 
				SquareHelper::Calc(m.To, - 1));
			break;
		case c1:	// Long castling
		case c8:
			assert(PieceHelper::IsNoPiece(GetPiece(m.From - 1)));
			assert(PieceHelper::IsNoPiece(GetPiece(m.From - 2)));
			assert(PieceHelper::IsNoPiece(GetPiece(m.From - 3)));
			assert(PieceHelper::IsOfPiece(GetPiece(m.From - 4), PieceHelper::AsPiece(ROOK, m_iColor)));
			// Move Rook at a1|a8 to d1|d8
			MovePiece( PieceHelper::AsPiece(ROOK, m_iColor),
				SquareHelper::Calc(m.To, -2), 
				SquareHelper::Calc(m.To, +1 ));
			break;
		default:	// Unknown castling ? ;-)
			assert( !"Invalid castling 'to'-field" );
			break;
		}
		MovePiece( m );	// Moves the King
		break;
	default:
		assert(!"Unsupported move type");
		break;
	}

	//
	//	Check for check
	//	This can possibly be done smarter, but it's easy and it works!
	//	TODO: Move this into the algoritms - it calls GetAttackBoard() from MoveGen
	//
	if (InCheck())
	{
		// assert(m.IsCheck == true);   //TODO: We are not setting the IsCheck flag on the Move until we are printing it out to the screen
		ChangePlayer();

		UndoMove(m);
		return false;
	}
	// Nu er det den andens tur
	ChangePlayer();
	return true;
}

// Undoer et traek - 
// Ren spejling af DoMove()
void Board::UndoMove(_In_ const Move& m)
{
	assert( MoveHelper::IsValid( m ));
	assert(GetCurrentColor() != PieceHelper::Color(m.MovPiece));

	ChangePlayer();

	switch (m.Type)
	{
	case MoveType::Normal:
		assert(!PieceHelper::IsActual( m.Content));
		MovePiece( m.MovPiece, m.To, m.From );	// Moves the Piece back
		break;
	case MoveType::Capture:
		assert( MoveHelper::IsCapture( m ));
		MovePiece(m.MovPiece, m.To, m.From);	// Moves the Piece back
		AddPieceToBoard( m.Content, m.To );		// Re-adds the captured piece
		break;
	case MoveType::PawnTwoForward:
		assert(m_iColor == PieceHelper::Color( m.MovPiece));
		assert(!PieceHelper::IsActual(m.Content));	// No capture on this type
		assert(MoveHelper::IsPawnMove(m));
		MovePiece(m.MovPiece, m.To, m.From);	// Moves the Piece back
		break;
	case MoveType::En_Passant:
		assert(MoveHelper::IsPawnMove(m));
		assert(MoveHelper::IsCapture(m) && PieceHelper::IsPawn(m.Content));	// slaar altid en bonde
		MovePiece(m.MovPiece, m.To, m.From);	// Moves the Piece back
		// Er det et en-passant slag, skal modstanderens bonde fjernes fra det rigtige felt
		if (m_iColor == WHITE)
			// Tilfoejer den sorte bonde paa feltet nedenunder
			AddPieceToBoard( ePiece::BLACK_PAWN, SquareHelper::Calc(m.To, +ONE_ROW ));
		else
			// Tilfoejer den hvide bonde paa feltet ovenover
			AddPieceToBoard( ePiece::WHITE_PAWN, SquareHelper::Calc(m.To, -ONE_ROW ));
		break;
	case MoveType::PromoteCapture:
		assert(!MoveHelper::IsPawnMove(m));	// The Pawn is implicit
		RemovePieceFromBoard(m.MovPiece, m.To);	// Remove the Piece just promoted
		assert(MoveHelper::IsCapture(m));
		AddPieceToBoard(m.Content, m.To);	// Readd the captured Piece
		AddPieceToBoard( PieceHelper::AsPiece(PAWN, m_iColor), m.From);	// Adds the Pawn again
		break;
	case MoveType::Promote:
		assert(!MoveHelper::IsPawnMove(m));	// The Pawn is implicit
		RemovePieceFromBoard( m.MovPiece, m.To );	// Remove the Piece just promoted
		assert(!MoveHelper::IsCapture(m));	// Not a capture
		AddPieceToBoard( PieceHelper::AsPiece(PAWN, m_iColor), m.From );	// Adds the Pawn again
		break;	
	case MoveType::Castling:
		// Castling also involves moving two pieces, the King and a Rook
		assert( m.From == e1 || m.From == e8);	// must be in starting position
		assert(PieceHelper::IsKing(GetPiece(m.To)));
		assert(MoveHelper::IsKingMove(m));

		switch( m.To)
		{
		case g1:	// Short castling
		case g8:	
			assert(PieceHelper::IsNoPiece(GetPiece(m.From)));	// nothing on e1+h1 now, they are on f1+g1
			assert(PieceHelper::IsNoPiece(GetPiece(m.To + 1)));

			// Move Rook now at f1|f8 back to h1|h8
			MovePiece( PieceHelper::AsPiece(ROOK, m_iColor),
				SquareHelper::Calc(m.To, -1), 
				SquareHelper::Calc(m.To, +1 ));
			break;
		case c1:	// Long castling
		case c8:
			assert(PieceHelper::IsNoPiece(GetPiece(m.From)));	// nothing on e1+a1+b1 now, they are on c1+d1
			assert(PieceHelper::IsNoPiece(GetPiece(m.To - 2)));
			assert(PieceHelper::IsNoPiece(GetPiece(m.To - 1)));

			// Move Rook now at d1|d8 back to a1|a8
			MovePiece(PieceHelper::AsPiece(ROOK, m_iColor),
				SquareHelper::Calc(m.To, +1),
				SquareHelper::Calc(m.To, -2 ));
			break;
		default:	// Unknown castling ? ;-)
			assert( !"Invalid castling 'to'-field" );
			break;
		}
		// Readding the King to the starting position
		MovePiece(m.MovPiece, m.To, m.From);
		break;
	default:
		assert(!"Unsupported move type");
	}
}

// Tester om kongen staar i skak efter det lige foretagne traek!
// Med denne rutine er det ikke muligt at se _hvilken_ brik der truer 'pos'
// Uses private variables m_iColor and m_bitboards
bool Board::InCheck() const noexcept
{
	// Inverterer farven, da vi vil generere traek for modstanderen
	const eColor byColor = ( m_iColor == WHITE ? BLACK : WHITE );

	const BITBOARD bb = MoveGenerator::GetAttackBoard( byColor );
	
	// Hvis angrebsbitboardet indeholder vores konges placering returneres true
	return Bits::isAnyBitSet(bb, m_bitboards.at(static_cast<BITBOARD>(KING) + m_iColor));
}

//////////////////////////////////////////////////
//
//	Utility methods
//

// ************************************
// Method:      TestBitBoards
// Description: Test til at finde hvor uoverensstemmelsen mellem ALL_PIECES og de andre
// FullName:    public Board::TestBitBoards const
// Returns:     bool - 
// Parameter:   ostream& stream - 
// Remark:      
// ************************************
bool Board::TestBitBoards(std::ostream& stream)const
{
	BITBOARD bbOR = 0;

	// OR alle BitBoards for de enkelte brikker
	for (auto i = 0; i < ALL_PIECETYPES; i++)
		bbOR |= m_bitboards.at(i);

	//Check for ALL_PIECES er lig vores OR
	if (bbOR != m_bitboards.at(ALL_PIECES))
	{
		stream << "Alle enkelt-boards OR'ed sammen\n";
		BitBoardHelper::PrintBitboardBinary(bbOR, stream);

		// Printer alle andre
		PrintAllBitboards(m_bitboards, stream);

		return false;
	}
	return true;
}

void Board::PrintAllBitboards(_In_ const TBitboards& boards, std::ostream& stream) const
{
	// Udprint af boardet i testoutput.txt
	stream << *this;

	// Printer indholdet af de forskellige bitboards ud i testoutput.txt
	stream << "ALL_BLACK_PIECES\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::ALL_BLACK_PIECES), stream);
	stream << "ALL_WHITE_PIECES\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::ALL_WHITE_PIECES), stream);
	stream << "ALL_PIECES\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ALL_PIECES), stream);
	stream << "WHITE_KING\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::WHITE_KING), stream);
	stream << "BLACK_KING\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::BLACK_KING), stream);
	stream << "WHITE_PAWN\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::WHITE_PAWN), stream);
	stream << "BLACK_PAWN\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::BLACK_PAWN), stream);
	stream << "WHITE_BISHOP\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::WHITE_BISHOP), stream);
	stream << "BLACK_BISHOP\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::BLACK_BISHOP), stream);
	stream << "WHITE_QUEEN\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::WHITE_QUEEN), stream);
	stream << "BLACK_QUEEN\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::BLACK_QUEEN), stream);
	stream << "WHITE_ROOK\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::WHITE_ROOK), stream);
	stream << "BLACK_ROOK\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::BLACK_ROOK), stream);
	stream << "WHITE_KNIGHT\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::WHITE_KNIGHT), stream);
	stream << "BLACK_KNIGHT\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::BLACK_KNIGHT), stream);
}

// Printer boardet til streamen
std::ostream& operator<<(std::ostream& os, _In_ const Board& board )
{
	constexpr std::size_t numRanks = 8;
	constexpr std::size_t numFiles = 8;

	for(unsigned int rank=0; rank < numRanks; ++rank)
	{
		os << ONE_ROW-rank << " ";

		for(unsigned int file=0; file < numFiles; ++file)
		{
			const BITBOARD squareMask = g_bbMask[(rank << 3) + file];
			
			// Skanner bitboards for at finde om der staar en brik paa feltet
			std::size_t piece = 0;
			while ((piece < ALL_PIECETYPES) && ((squareMask & board.m_bitboards[piece]) == 0))
				++piece;
			
			// Hvis ingen brik fundet, sæt piece til tom-felt indeks
			if (piece >= ALL_PIECETYPES) {
				piece = ALL_PIECETYPES;
			}
			// Udskriver brikken
			os << " " << g_cPieceNames[piece];
		}
		os << '\n';
	}

	os << "\n   A B C D E F G H\n\n";

	return os;
}

//-----------------------------------------------
//
//	Transposition tables funktioner
//

// ************************************
// Method:      ProbeHash
// Description: 
// FullName:    public Board::ProbeHash const
// Returns:     int - 
// Parameter:   unsigned int ply - 
// Parameter:   int alpha - 
// Parameter:   int beta - 
// Parameter:   Move&  - 
// Remark:      
// ************************************
std::pair<int, Move> Board::ProbeHash(_In_ size_t ply, _In_ int alpha, _In_ int beta) const
{
	int returnScore = GameValues::Unknown_Hash;
	auto returnPair = std::make_pair(returnScore, Move::EmptyMove());
	
	TMoveHashTable::const_iterator cit = hashTable_.find( GetCurBoardHKey() );
	if (cit != hashTable_.end())
	{
		const HashElement& elem = cit->second;
		assert(elem.hkey == GetCurBoardHKey());
		assert(ply > 0);	// <- Should be removed, just to test size_t -> unsigned int conversion
		if( elem.iDepth >= ply )
		{
			if (abs(elem.iValue) > GameValues::Mate-10 )	// Ignore moves too close to mate
				returnScore = GameValues::Unknown_Hash;	// TODO - is this (<--) really needed? It's obviously correct, but will just cause us to spend longer time searching - needs to be measured
			//FIXME: This is wrong - should be used in the MoveSorter!!
			returnPair.second = elem.BestMove;

			if (elem.hashflag == eHashFlags::hashfEXACT)
			{
				if (elem.iValue >= beta)	// Er den _for_ god ? Cutoff !
				{
					returnScore = beta;	// Hmm... hvornaar sker det?
				}
				else if (elem.iValue > alpha)	// Er det en bedre vaerdi?
				{
					returnScore = elem.iValue;
				}
				else
				{
					returnScore = GameValues::Unknown_Hash;	// Value is out-of-bounds, try again
					// Optimize: AFAICT this causes a full (continued) search even if we know it's not good enough
				}
			}
			else if ((elem.hashflag == eHashFlags::hashfALPHA) && (elem.iValue <= alpha))
				returnScore = alpha;
			else if ((elem.hashflag == eHashFlags::hashfBETA) && (elem.iValue >= beta))	// CHECK? Else or else-if?
				returnScore = beta;
			else
			{
				// No action needed
			}
		}
	}
	returnPair.first = returnScore;
	return returnPair;
}

// ************************************
// Method:      RecordHash
// Description: 
// FullName:    public Board::RecordHash
// Returns:     void - 
// Parameter:   unsigned int iPly - 
// Parameter:   int iValue - 
// Parameter:   eHashFlags hashflag - 
// Parameter:   const Move&  - 
// Remark:      
// ************************************
void Board::RecordHash(_In_ size_t ply, _In_ int score, _In_ eHashFlags flags, _In_ const Move& m)
{
	if (ply < 2)	// Heuristics: Only interested in moves at ply 2 or lower
		return;

	if( m.IsEmpty())
		return;
	
	assert(IsLegalMove( m ));	// sanity check the move

	/*
	 *	FIXME: Det virker ikke saerlig godt - specielt omkring mat-situationer!
	 *		Jeg tror at vi skal til at lave en explicit whitelisting -dvs. finde ud af praecist hvilke situationer der virker hver gang 
	 *		og hvilke der kun virker nogle gange! Specielt omkring mat!
	 *		Maaske skal vi ikke recorde hash'es i forbindelse med mat-situationer, saa laenge vi ikke kan garantere value-beregningerne bedre
	 */

	HashElement newElem; 
	newElem.hkey = GetCurBoardHKey();
	newElem.iValue = score;
	newElem.iDepth = static_cast<unsigned short>(ply);
	newElem.hashflag = flags;
	//		assert( MoveHelper::IsValid( m ));	// only makes sense for PVLine based algoritms
	newElem.BestMove = m;

	auto pair = hashTable_.insert( std::make_pair(GetCurBoardHKey(), newElem) );	// Insert move
	if( pair.second == false) // -> Move already existing
	{
		// Match - The Move has already been stored. Update the move if we are deeper now
		HashElement& oldElem = pair.first->second;

		if ( newElem.iDepth > oldElem.iDepth )
		{
			oldElem = newElem;	// Its deeper! Update the move element
		}
		else if( newElem.iDepth == oldElem.iDepth )	// Ok, same ply - let's check flags
		{
			if( newElem.hashflag == eHashFlags::hashfEXACT && oldElem.hashflag != eHashFlags::hashfEXACT)	// is the new one exact and the old wasn't? 
				oldElem = newElem;													// then store the better value
			if( newElem.hashflag == eHashFlags::hashfBETA && oldElem.hashflag == eHashFlags::hashfALPHA )	// Hmm... why is beta better than alpha?
				oldElem = newElem;
		}
		else
		{
			// No action needed
		}
	}
}

// ************************************
// Method:      ClearHashTable
// Description: Clears the hash tables - gets called upon startup and before each move search
// FullName:    public Board::ClearHashTable
// Returns:     void - 
// Remark:      
// ************************************
void Board::ClearHashTable()
{
	hashTable_.clear();
}

//	-	-	-	-	-	-	-	-	-	-	-	-	-	-	-	-	-	-	-

//	Fills up the hash key table with random gibberish.  These will be XOR'd
//	together in order to create the semi-unique hash key for any given
//	position.  They need to be as random as possible otherwise there will be
//	too many hash collisions.
void Board::InitHashkey()
{
	// Create random device and seed mt19937 engine
	std::random_device rd;
	std::mt19937 rng(rd());  // non-deterministic seed

	// Distribution to fill with 64-bit keys
	std::uniform_int_distribution<uint64_t> dist(0, UINT64_MAX);

	// Fill hash keys
	for (int piece = ePiece::WHITE_PAWN; piece < ALL_PIECETYPES; ++piece) {
		for (int square = 0; square < ALL_SQUARES; ++square) {
			allHashKeys[piece][square] = dist(rng);
		}
	}
}
