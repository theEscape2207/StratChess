// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "Config.h"

#include "Board.h"
#include "Game.h"

// For regular expression: Validating user input
//#include <Poco/RegularExpression.h>

using json = nlohmann::json;

//***************************************
// Method:      ReadBoardSetup
// Description: 
// FullName:    private Game::ReadBoardSetup const
// Returns:     void - 
// Parameter:   const json& config - 
// Remark:      
//***************************************

void Config::ReadBoardSetup(const json& config) const
{
	// The Board is ready - set it up
	// if nothing is found - use default setup
	const std::string setupType = config["game"].value("setup", "default");
	//const std::string setupType = config->getString("setup", std::string("default"));
	if (0 == _strnicmp(setupType.c_str(), "custom", 7))
	{
		spdlog::default_logger()->info("Custom board setup chosen - not yet migrated. Setting default board");
		Board::Instance().SetDefaultBoard();	// FIXME setting default setup instead

		// parse placement complex below
		//Board::squareCol vec;
		//bool bSuccess = true;

		//const std::string PlacementKey = "placement";

		//Poco::Util::AbstractConfiguration::Keys configSquares;
		//config->keys(PlacementKey, configSquares);

		//const Poco::Util::AbstractConfiguration* pConfPieces = config->createView(PlacementKey);

		//// Iterate over game.placement section and store all found modules in our squareCol object vec.
		//for (const auto &confSq : configSquares)
		//{
		//	// We only want _trimmed_ lower case all over
		//	const auto strSq = Poco::toLower(Poco::trim(confSq));

		//	// Use regular expression: Accept only square tags like a1 or f7
		//	static const std::string regSq("[a-h][1-8]");

		//	bSuccess = CheckBoardSetupData(strSq, regSq);
		//	if (!bSuccess)
		//		break;

		//	// square should be valid now - lets get it
		//	const unsigned int row = strSq.at(0) - 'a';
		//	const unsigned int col = ((8 - (strSq.at(1) - '0')) << 3);
		//	const auto sq = static_cast<eSquare>(col + row);

		//	// What Piece is supposed to go there...
		//	const std::string strPiece = pConfPieces->getString(strSq);

		//	// Use regular expression: Accept only piece values from 0-11 (for fun we allow '0009' also)
		//	static const std::string regPiece("^0*([0-9]|1[01])$");	// see http://www.regextester.com/index2.html for test
		//	bSuccess = CheckBoardSetupData(strPiece, regPiece);
		//	if (!bSuccess)
		//		break;

		//	auto piece = static_cast<ePiece>(std::stoi(strPiece));

		//	vec.emplace_back(std::make_tuple(piece, sq));
		//}
		//if (bSuccess) {
		//	Board::Instance().SetupBoard(vec);		// Format is valid - use the data
		//	const GameConfig gConfig;
		//	pGame_->SetCustomGame(gConfig);		// Use default values (no epSquare, no castling options, 50 moves counter = 0
		//}
		//else
		//{
		//	spdlog::default_logger()->warn("Invalid Custom board entries - selecting default board instead ");
		//	Board::Instance().SetDefaultBoard();	// Oops... Use default setup instead
		//}
	}
	else if (0 == _strnicmp(setupType.c_str(), "FEN", 3))
	{
		spdlog::default_logger()->info("FEN string detected - Custom board setup chosen");

		const std::string FENKey = "FEN";
		const std::string FENstring = config["game"].value("FEN", "");
		if (FENstring.empty())
		{
			spdlog::default_logger()->warn("No FEN string found - selecting default board");
		}

		ReadFEN(FENstring);
	}
	else
	{
		spdlog::default_logger()->info("Default board selected");
		Board::Instance().SetDefaultBoard();
	}
}

// Parse FEN castling string format either "KQkq" or "-" if no more available
void Config::GetCastlingRights(const std::string& str, Config::GameConfig& info) const noexcept
{
	assert((info.whitekingcastle || info.whitequeencastle || info.blackkingcastle || info.blackqueencastle) == false);
	if (str.front() != '-') // no more castling rights left
	{
		for (const auto& c : str)
		{
			switch (c)
			{
			case 'K':
				info.whitekingcastle = true;
				break;
			case 'Q':
				info.whitequeencastle = true;
				break;
			case 'k':
				info.blackkingcastle = true;
				break;
			case 'q':
				info.blackqueencastle = true;
				break;
			default:	// ignore all else
				break;
			}
		}
	}
}

/*FEN specifies the piece placement, the active color, the castling availability, the en passant target square, the halfmove clock, and the fullmove number.
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
void Config::ReadFEN(const std::string& fen) const
{
	std::vector<std::string> spaceSplit;

	//Split full FEN string
	std::stringstream ss(fen);
	std::string item;
	while (std::getline(ss, item, ' ')) {
		spaceSplit.emplace_back(item);
	}

	// 1. Setup piece positions
	ParseRankFENstring(spaceSplit[0]);
	
	GameConfig gameConfig;

	// 2. Whose turn is it?
	if (spaceSplit[1][0] == 'b')
	{
		gameConfig.color = eColor::BLACK;
	}
	else // WHITE is the default
	{
		// No action needed
	}

	// 3. castling options
	// <Castling ability> ::= '-' | ['K'] ['Q'] ['k'] ['q'] (1..4)
	GetCastlingRights(spaceSplit[2], gameConfig);

	// 4. Setup En passant square
	if (spaceSplit[3][0] != '-') // format: either '-' og col-row, e.g. "e3", "h7"
	{
		gameConfig.epSquare = GetSquare(spaceSplit[3]);
	}

	// 5. Half move clock
	if (spaceSplit.size() >= 5) // in case the FEN format doesn't include the move counters
	{
		gameConfig.num50moves = std::stoi(spaceSplit[4]);
	}

	// 6. Full move number
	if (spaceSplit.size() >= 6)
	{
		gameConfig.fullMoveCounter = std::stoi(spaceSplit[5]);
	}

	pGame_->SetCustomGame(gameConfig);
}

// Converting the field referrrer in string format "e3" to the corresponding eSquare
 eSquare Config::GetSquare(const std::string& input)
{
	auto begin = input.begin();
	const unsigned int row = *begin++ - 'a';
	const unsigned int col = ((8 - ((*begin) - '0')) << 3); //-V783
	return static_cast<eSquare>(col + row);
}

/* The first field represents the placement of the pieces on the board.
The board contents are specified starting with the eighth rank and ending with the first rank.
For each rank, the squares are specified from file a to file h.
White pieces are identified by uppercase SAN piece letters ("PNBRQK") and
black pieces are identified by lowercase SAN piece letters ("pnbrqk").
Empty squares are represented by the digits one through eight,
the digit used represents the count of contiguous empty squares along a rank.
A solidus character "/" is used to separate data of adjacent ranks.
*/
bool Config::ParseRankFENstring(const std::string& rankFEN) const
{
	std::vector<std::string> piecesByRank;
	std::string item;

	auto ss = std::stringstream(rankFEN);
	while (std::getline(ss, item, '/')) {
		piecesByRank.emplace_back(item);	// find 8 strings - one per row
	}

	Board::squareCol vec;
	eSquare square = NO_SQUARE;
	ePiece piece = NO_PIECE;

	for (unsigned int rank = 0; rank < 8; rank++)
	{
		int x = -1;
		for (const auto& pieceChar : piecesByRank[rank])
		{
			if (isdigit(pieceChar))
			{
				x += pieceChar - '0';
			}
			else
			{
				x++;
				square = static_cast<eSquare>(8 * rank + x);

				switch (pieceChar)
				{
				case 'K':
					piece = ePiece::WHITE_KING;
					break;
				case 'R':
					piece = ePiece::WHITE_ROOK;
					break;
				case 'B':
					piece = ePiece::WHITE_BISHOP;
					break;
				case 'Q':
					piece = ePiece::WHITE_QUEEN;
					break;
				case 'N':
					piece = ePiece::WHITE_KNIGHT;
					break;
				case 'P':
					piece = ePiece::WHITE_PAWN;
					break;
				case 'k':
					piece = ePiece::BLACK_KING;
					break;
				case 'r':
					piece = ePiece::BLACK_ROOK;
					break;
				case 'b':
					piece = ePiece::BLACK_BISHOP;
					break;
				case 'q':
					piece = ePiece::BLACK_QUEEN;
					break;
				case 'n':
					piece = ePiece::BLACK_KNIGHT;
					break;
				case 'p':
					piece = ePiece::BLACK_PAWN;
					break;
				default:
					// invalid data received - fail FEN loading
					return false;
				}
				vec.emplace_back(std::make_tuple(piece, square));
			}
		}
	}
	Board::Instance().SetupBoard(vec);		// Format is valid - use the data
	return true;
}

//***************************************
// Method:      CheckBoardSetupData
// Description: 
// FullName:    private Game::CheckBoardSetupData const
// Returns:     bool - 
// Parameter:   const std::string& strPiece - 
// Parameter:   const std::string& regexStr - 
// Remark:      
//***************************************
bool Config::CheckBoardSetupData(const std::string& /*strPiece*/, const std::string& /*regex*/) const
{
	//FIXME: Not yet ported away from Poco
	/*const Poco::RegularExpression regPiece(regex);
	if (!regPiece.match(strPiece))
	{
		std::stringstream str;

		str << "Invalid board setup data found: Piece on square "
			<< strPiece.c_str() << " with type: " << strPiece.c_str() << std::endl;
		spdlog::default_logger()->warn(str.str());
		return false;
	}*/
	return true;
}
void Config::ReadConfigFile(const std::string& filename)
{
	spdlog::default_logger()->info("Reading Config File");

	std::ifstream configFile(filename);
	if (!configFile) {
		std::cerr << "Cannot open game_settings.json\n";
		return;		// FIXME: add error handling
	}

	nlohmann::json config;
	configFile >> config;

	SetupPlayerConfig(config);

	// Read any Board setup from the config file
	ReadBoardSetup(config);
}

void Config::SetupPlayerConfig(const json& config)
{
	// Read white player - with default settings
	white_.depth = config["game"]["players"]["white"].value("depth", DEFAULT_DEPTH);
	white_.type = config["game"]["players"]["white"].value("type", DEFAULT_EVAL);	// default aiagent
	white_.eval = config["game"]["players"]["white"].value("eval", 0);	// default SIMPLE

	black_.depth = config["game"]["players"]["black"].value("depth", DEFAULT_DEPTH);
	black_.type = config["game"]["players"]["black"].value("type", DEFAULT_EVAL);	// default aiagent
	black_.eval = config["game"]["players"]["black"].value("eval", 0);	// default SIMPLE
}

Config::PlayerConfig Config::GetPlayerFromConfig(bool bWhite) const noexcept
{
	if (bWhite)
	{
		return white_;
	}
	return black_;
}
