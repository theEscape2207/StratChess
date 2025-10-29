// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include "StdAfx.h"
#include "Game.h"
#include "Board.h"
#include "PlayerBase.h"		// For factory create

#include "spdlog/sinks/stdout_color_sinks.h" // or "../stdout_sinks.h" if no colors needed
#include "spdlog/sinks/basic_file_sink.h"

extern std::ofstream outLegalMoves;

// ***************************************
// Method:      Game
// Description: Constructor
// FullName:    public Game::Game
// Returns:      - 
// Remark:      
// ***************************************
Game::Game()
{
	Init();
}

// ************************************
// Method:      LogMessage
// Description: 
// FullName:    private Game::LogMessage const
// Returns:     void - 
// Parameter:   const std::string &text - 
// Parameter:   Poco::Message::Priority prio - 
// Remark:      FIXME: Hardcoded number of loggers
// ************************************
//void Game::LogMessage(const std::string &text, Poco::Message::Priority prio )
//{
//	//FIXME: The root and consoleLogger is the same in Release mode for some reason
//	spdlog::default_logger()->info(text);
//	/*Poco::Message msg("Game", text, prio);
//	Logger::root().log( msg );
//	Logger::get( "consoleLogger" ).log( msg );*/
//}


//***************************************
// Method:      ~Game
// Description: Destructor
// FullName:    public Game::~Game 
// Returns:      - 
// Remark:      
//***************************************
Game::~Game()
{
	try {
		// Deregister our delegates
		m_pPlayers[WHITE]->ENewPVLineMove.subscribe([this](const void* s, const PVLine& pvl) { onNewPVLineMove(s, pvl); });
		m_pPlayers[BLACK]->ENewPVLineMove.subscribe([this](const void* s, const PVLine& pvl) { onNewPVLineMove(s, pvl); });

		m_pPlayers[WHITE]->EGameStateChanged.subscribe([this](const void* s, const GameStates& gs) { OnGameStateChanged(s, gs); });
		m_pPlayers[BLACK]->EGameStateChanged.subscribe([this](const void* s, const GameStates& gs) { OnGameStateChanged(s, gs); });

		// Under VisualStudio, this must be called before main finishes to workaround a known VS issue
		spdlog::drop_all();
	}
	catch (const std::exception&)
	{
		// Don't care if any deregistration fails, we're closing here
	}
}

//***************************************
// Method:      operator<<
// Description: 
// FullName:    public << 
// Returns:     std::ostream& - 
// Parameter:   std::ostream &os - 
// Parameter:   const Game &game - 
// Remark:      
//***************************************
std::ostream& operator<<(std::ostream& os, const Game& game)
{
	for (const auto& m : game.m_GameMoves)
	{
		os << m << std::endl;
	}
	return os;
}

//***************************************
// Method:      GetCurrentPlayer
// Description: 
// FullName:    private Game::GetCurrentPlayer const
// Returns:     PlayerBase& - 
// Remark:      
//***************************************
IPlayer& Game::GetCurrentPlayer() const noexcept
{
	return *m_pPlayers[Board::Instance().GetCurrentColor()];
}

//***************************************
// Method:      Init
// Description: 
// FullName:    private Game::Init 
// Returns:     void - 
// Remark:      
//***************************************
void Game::Init()
{
	try
	{
		auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
		console_sink->set_level(spdlog::level::info);
		console_sink->set_pattern("[multi_sink_example] [%^%l%$] %v");

		auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/multisink.txt", true);
		file_sink->set_level(spdlog::level::trace);

		spdlog::sinks_init_list sink_list = { file_sink, console_sink };

		spdlog::logger logger("multi_sink", sink_list.begin(), sink_list.end());
		logger.set_level(spdlog::level::debug);
		logger.warn("this should appear in both console and file");
		logger.debug("this message should not appear in the console, only in the file");

		// or you can even set multi_sink logger as default logger
		spdlog::set_default_logger(std::make_shared<spdlog::logger>("multi_sink", spdlog::sinks_init_list({ console_sink, file_sink })));

		// Existing stuff...
		LoadConfigFileSettings();

		logger.info("Config File Loaded");
		CreateGameMoveFile();

		std::stringstream sstream;
		AddFileHeader(sstream);
		logger.warn(sstream.str());

	}
	catch (const spdlog::spdlog_ex& ex)
	{
		std::cout << "Log initialization failed: " << ex.what() << std::endl;
	}

}

void Game::CreateGameMoveFile()
{
	auto logger = spdlog::default_logger();
	logger->debug("Creating Moves Log File: 'gamelist.txt'");
	movesFile.open("gamelist.txt", std::ios::trunc | std::ios::out);
	AddFileHeader(movesFile);
	logger->debug("Created Moves Log File: 'gamelist.txt'");
}

//***************************************
// Method:      LoadConfigFileSettings
// Description: 
// FullName:    private Game::LoadConfigFileSettings 
// Returns:     void - 
// Remark:      FIXME: Validate input ranges - all over!!
//***************************************
void Game::LoadConfigFileSettings()
{
	Config reader(this);
	reader.ReadConfigFile("game_settings.json");

	// TODO: Setup board explicitly here
	spdlog::default_logger()->info("Creating players from Config File");
	//TODO: We are creating stuff we do not need (e.g. eval engine as human and NewPVLineMove event as non-iterative AI)
	const Config::PlayerConfig whiteConfig = reader.GetPlayerFromConfig(true);
	m_pPlayers[WHITE] = SetPlayerParams(whiteConfig);

	// Create black player
	const Config::PlayerConfig blackConfig = reader.GetPlayerFromConfig(false);

	m_pPlayers[BLACK] = SetPlayerParams(blackConfig);
}


std::unique_ptr<IPlayer> Game::SetPlayerParams(const Config::PlayerConfig& config)
{
	auto player = PlayerBase::Create(static_cast<PlayerBase::ePlayerTypes>(config.type), config.depth);
	player->SetEvalEngine(static_cast<EvalManager::EvalTypes>(config.eval));

	//Register events
	player->ENewPVLineMove.subscribe([this](const void* s, const PVLine& pvl) { onNewPVLineMove(s, pvl); });
	player->EGameStateChanged.subscribe([this](const void* s, const GameStates& gs) { OnGameStateChanged(s, gs); });
	return player;
}

void Game::SetGameParams(const Config::GameConfig& config) noexcept
{
	// Set active color
	Board::Instance().SetInitialColor(config.color);
	// Set ep square
	gameInfo_.epSquare = config.epSquare;
	if (config.epSquare != NO_SQUARE)
	{
		gameInfo_.lastMove.To = NO_SQUARE;
		gameInfo_.lastMove.From = NO_SQUARE;
		gameInfo_.lastMove.Type = MoveType::PawnTwoForward;
		gameInfo_.lastMove.MovPiece = ((config.color == WHITE) ? BLACK_PAWN : WHITE_PAWN);
		gameInfo_.lastMove.Content = NO_PIECE;
	}

	// Castling availability
	gameInfo_.blackLongCastle = config.blackqueencastle;
	gameInfo_.blackShortCastle = config.blackkingcastle;
	gameInfo_.whiteLongCastle = config.whitequeencastle;
	gameInfo_.whiteShortCastle = config.whitekingcastle;

	// Halfmove clock
	gameInfo_.fiftyCount = config.num50moves;
	// Fullmove number
	gameInfo_.fullMoveCount = config.fullMoveCounter;

}

//***************************************
// Method:      RunGame
// Description: This is the main game loop
// FullName:    private Game::RunGame 
// Returns:     void - 
// Remark:      
//***************************************
void Game::Run()
{
	Board& rBoard = Board::Instance();

	// First print of board
	PrintBoardAndMove(Move::EmptyMove());

	/*
	*	Main game loop
	*	Player->GetMove() is the main driver of the game
	*/
	for (;;)
	{
		// Hent traekket fra den aktive spiller - GameInfo get updated every time
		Move newMove = GetCurrentPlayer().GetMove(gameInfo_);

		// Prints out the current score message for AI players (score or "Mate in x moves")
		PrintStateMessage();

		if (!IsStillPlaying())	// Test om spillet er slut
			break;

		// Has the user typed "exit" or "quit"?
		if (HasHumanExited(newMove))
		{
			spdlog::default_logger()->warn("User has exited the game\n");
			break;
		}

		// Foretag traekket paa det virkelige braet
		if (!rBoard.DoMove(newMove))
			assert(!"Unexpected illegal move found! Exiting...");

		// Traekket er godkendt! Vi spiller videre!!

		// Set whether the move is Checking
		// TODO: This should be set elsewhere, but we at least need to print it to the player...
		newMove.IsCheck = rBoard.InCheck();

		// Tilfoej traekket til traeklisten og opdater spil-variable
		AddGameMove(newMove);

		// Print the board and last move to screen (and debug)
		PrintBoardAndMove(newMove);
	}

	// FIXME: Add a menu allowing a new game to be played - including option to override from game-setup file
	spdlog::default_logger()->warn("Spillet er slut\n\nTryk enter for at afslutte!");
}


//***************************************
// Method:      AddGameMove
// Description: 
// FullName:    private Game::AddGameMove 
// Returns:     void - 
// Parameter:   const Move& move - move to be added
// Remark:      
//***************************************
void Game::AddGameMove(const Move& move)
{
	m_GameMoves.emplace_back(move);
	PrintGameMoves();
}

//***************************************
// Method:      PrintStateMessage
// Description: 
// FullName:    private Game::PrintStateMessage const
// Returns:     void
// Remark:      FIXME: Export the Max depth from the AIs to prevent hardcoding
//***************************************
void Game::PrintStateMessage() const
{
	if (IsStillPlaying())
	{
		if (GetCurrentPlayer().IsHuman())	// score doesn't make sense
			return;

		std::stringstream sstream;
		// AIs
		const int score = GetCurrentPlayer().GetBestScore();

		// Can we see a mate? // TODO: This is a poor way of measuring the distance
		// FIXME: Export the Max depth from the AIs
		if (score >= GameValues::Mate - 10 || (-score >= GameValues::Mate - 10))
			// Antal ply: GameValues::Mate-Lastscore. Antal traek: Ply+1/2 => (MATE-Last)/2
			sstream << "Skakmat i " << ((GameValues::Mate - abs(score) + 1) / 2) << " traek" << std::endl;
		else
			sstream << "Score: " << score << std::endl;

		spdlog::default_logger()->warn(sstream.str());
	}
	else	// TODO: This should be moved to the OnGameStateChanged event method
	{
		switch (gameInfo_.gameState)
		{
		case GameStates::WHITE_WON:
			spdlog::default_logger()->warn("\nCHECK MATE !\nWhite is the Winner!");
			break;
		case GameStates::BLACK_WON:
			spdlog::default_logger()->warn("\nCHECK MATE !\nBlack is the Winner!");
			break;
		case GameStates::DRAW_PAT:
			spdlog::default_logger()->warn("\nIts a Draw !\nNo more legal moves - but not in check!");
			break;
		case GameStates::DRAW_50_MOVES:
			spdlog::default_logger()->warn("\nIts a Draw !\n50 moves has passed since last Capture or peasant move!");
			break;
		case GameStates::HUMAN_EXITED:
			spdlog::default_logger()->warn("\nHuman player exited game. \nOpponent is the Winner!");
			break;
		case GameStates::STILL_PLAYING:
		default:
			assert(!"Oops - forgot to add a Game State");
			break;
		}
	}
}


//***************************************
// Method:      PrintBoardAndMove
// Description: 
// FullName:    private Game::PrintBoardAndMove const
// Returns:     void - 
// Parameter:   const Move& move - 
// Remark:      TODO: Fix printing to file - maybe just a static ofstream object?
//***************************************
void Game::PrintBoardAndMove(const Move& move) const
{
	const Board& board = Board::Instance();
	// Print to various places
	std::stringstream sstream;
	sstream << "\nBoard " << GetBoardCount() << std::endl << std::endl;
	sstream << board << move;
	spdlog::default_logger()->warn(sstream.str());

#ifdef PRINT_MOVES
	outLegalMoves << "===============================================================" << std::endl << std::endl;
	outLegalMoves << "\nBoard " << GetBoardCount() << std::endl << std::endl; //-V128
	outLegalMoves << board;

#endif // PRINT_MOVES
}

//***************************************
// Method:      PrintGameMoves
// Description: 
// FullName:    private Game::PrintGameMoves
// Returns:     void - 
// Remark:      Always only one game object
//***************************************
void Game::PrintGameMoves()
{
	movesFile << "Move " << m_GameMoves.size() << std::endl; //-V128
	movesFile << m_GameMoves.back() << std::endl;
}

//***************************************
// Method:      AddFileHeader
// Description: Use this for printing out a nice file header for each game file
//				Prints out Players, depth, date and time
// FullName:    private Game::AddFileHeader const
// Returns:     void - 
// Parameter:   std::ostream& file - the out stream to write to
// Remark:      Adds the following header :
//				Chess Game
//				White: <White player type>
//				Black: <Black player type>
//				Date: <Current Date and Local Time>
//				-------------------------
//***************************************
void Game::AddFileHeader(std::ostream& file)const
{
	using namespace std::chrono;
	auto now = system_clock::now();
	std::time_t now_c = system_clock::to_time_t(now);

	std::tm timeinfo{};

	file << R"(
*********************************
*      Escapes Chess game       *
*      Version: 0.6             *
*********************************
)";
	errno_t err = localtime_s(&timeinfo, &now_c);	// MSVC complains about localtime() usage
	// Print the current time -  // RFC 1123 format is like: "Sun, 06 Nov 1994 08:49:37 GMT"
	if (err == 0)
	{
		file << "Time: " << std::put_time(&timeinfo, "%a, %d %b %Y %H:%M:%S %Z") << "\n\n";
	}

	file << "----------------------------------------------" << std::endl;
	file << "White:" << m_pPlayers[WHITE]->getDescription() << std::endl;
	file << "Black:" << m_pPlayers[BLACK]->getDescription() << std::endl;
	file << "----------------------------------------------" << std::endl;
}
