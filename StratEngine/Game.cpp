// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include "StdAfx.h"
#include "Game.h"
#include "Board.h"
#include "MoveFormatter.h"
#include "PlayerBase.h"		// For factory create
#include "PlayerAI.h"		// For PlayerAiBase setters
#include "AIPerplex.h"		// For SearchTuning application

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

//***************************************
// Method:      ~Game
// Description: Destructor
// FullName:    public Game::~Game 
// Returns:      - 
// Remark:      
//***************************************
Game::~Game()
{
	try
	{
		unsubscribePlayerEvents();

		// Under VisualStudio, this must be called before main finishes to workaround a known VS issue
		spdlog::drop_all();
	}
	catch (const std::exception&)
	{
		// Don't care if any deregistration fails, we're closing here
	}
}

//***************************************
// Method:      unsubscribePlayerEvents
// Description: Unsubscribes from all player events. Calling this in destructor to avoid dangling references.
// FullName:    private Game::unsubscribePlayerEvents
// Returns:     void -
// Remark:
//***************************************
void Game::unsubscribePlayerEvents()
{
	// TODO: Potential issue if players were not created. This function assumes both players exist.
	// TODO: Calling clear() where unsubscribe() should be used, but we have no handles to unsubscribe with here.
	// Deregister our delegates
	m_pPlayers[WHITE]->ENewPVLineMove.clear();
	m_pPlayers[BLACK]->ENewPVLineMove.clear();

	m_pPlayers[WHITE]->EGameStateChanged.clear();
	m_pPlayers[BLACK]->EGameStateChanged.clear();
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
		os << m << '\n';
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
	return *m_pPlayers[board_.GetCurrentColor()];
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
		// Initialize engine default logger
		Engine::Logger::InitDefault();
		auto& logger = *spdlog::get("multi_sink");

		// Create performance logger
		Engine::Logger::EnsurePerfLogger("logs/SimplePerfStats.txt");
		auto perf = Engine::Logger::GetPerfLogger();
		if (perf) {
			perf->info("No. of nodes  |  Ms used  |  Nodes pr. ms  |  Total nodes  |  Total time  |  Total nodes pr. ms");
			perf->info("-----------------------------------------------------------------------------------------------------");
		}
		else {
			std::cout << "No. of nodes  |  Ms used  |  Nodes pr. ms  |  Total nodes  |  Total time  |  Total nodes pr. ms\n";
			std::cout << "-----------------------------------------------------------------------------------------------------\n";
		}
		
		// Existing startup code
		LoadConfigFileSettings();

		CreateGameMoveFile();

		std::stringstream sstream;
		logger.info(sstream.str());
		AddFileHeader(sstream);

	}
	catch (const spdlog::spdlog_ex& ex)
	{
		std::cout << "Log initialization failed: " << ex.what() << '\n';
	}

}

void Game::CreateGameMoveFile()
{
	auto logger = spdlog::default_logger();
	logger->debug("Creating Moves Log File: 'logs/gamelist.txt'");
	movesFile_.open("logs/gamelist.txt", std::ios::trunc | std::ios::out);
	AddFileHeader(movesFile_);
	logger->debug("Created Moves Log File: 'logs/gamelist.txt'");
}

//***************************************
// Method:      LoadConfigFileSettings
// Description: 
// FullName:    private Game::LoadConfigFileSettings 
// Returns:     void - 
// Remark:      FIXME: Validate input ranges - depth, eval engine type, player type
//***************************************
void Game::LoadConfigFileSettings()
{
	Config reader(this);
	reader.ReadConfigFile("game_settings.json", board_);

	// TODO: Setup board explicitly here
	spdlog::default_logger()->debug("Creating players from Config File");
	//TODO: We are creating stuff we do not need (e.g. eval engine as human and NewPVLineMove event as non-iterative AI)
	const Config::PlayerConfig whiteConfig = reader.GetPlayerFromConfig(true);
	m_pPlayers[WHITE] = SetPlayerParams(whiteConfig);

	// Create black player
	const Config::PlayerConfig blackConfig = reader.GetPlayerFromConfig(false);

	m_pPlayers[BLACK] = SetPlayerParams(blackConfig);
}


std::unique_ptr<IPlayer> Game::SetPlayerParams(const Config::PlayerConfig& config)
{
	auto player = PlayerBase::Create(static_cast<PlayerBase::ePlayerTypes>(config.type), config.depth, board_);
	player->SetEvalEngine(static_cast<EvalManager::EvalTypes>(config.eval));

	// Enable AIPerplex verbose logging in game mode (opt-in here; UCI/test modes disable it).
	if (dynamic_cast<AIPerplex*>(player.get())) {
		AIPerplex::SetVerboseLogging(true);
	}

	// Apply shared AI config (time_limit) to any AI type
	if (auto* ai = dynamic_cast<PlayerAiBase*>(player.get())) {
		ai->SetTimeLimit(std::chrono::milliseconds(config.time_limit_ms));
	}

	// Apply SearchTuning — only valid for AI_PERPLEX (type 6)
	if (config.search_tuning.has_value()) {
		constexpr unsigned kAiPerplex = static_cast<unsigned>(PlayerBase::ePlayerTypes::AI_PERPLEX);
		if (config.type != kAiPerplex) {
			spdlog::warn("search_tuning in game_settings.json is ignored for player type {} "
			             "(only supported by AI_PERPLEX)", config.type);
		} else if (auto* perplex = dynamic_cast<AIPerplex*>(player.get())) {
			const auto& st = *config.search_tuning;
			auto& t = perplex->tuning();
			t.min_nodes_threshold    = st.min_nodes_threshold;
			t.min_completion_ratio   = st.min_completion_ratio;
			t.min_pv_ratio           = st.min_pv_ratio;
			t.score_draw_threshold   = st.score_draw_threshold;
			t.delta_pruning_margin   = st.delta_pruning_margin;
			t.aspiration_initial_delta = st.aspiration_initial_delta;
			t.aspiration_max_retries   = st.aspiration_max_retries;
			t.aspiration_enabled       = st.aspiration_enabled;
		}
	}

	//Register events
	player->ENewPVLineMove.subscribe([this](const void* s, const PVLine& pvl) { onNewPVLineMove(s, pvl); });
	player->EGameStateChanged.subscribe([this](const void* s, const GameStates& gs) { OnGameStateChanged(s, gs); });
	return player;
}

void Game::SetGameParams(const GameInfo& info) noexcept
{
	// Set ep square
	gameInfo_.epSquare = info.epSquare;
	/*if (info.epSquare != NO_SQUARE)
	{
		const auto movPiece = (config.sideToMove == WHITE) ? BLACK_PAWN : WHITE_PAWN;
		gameInfo_.lastMove.SetMove(NO_SQUARE, NO_SQUARE, MoveType::DOUBLE_PAWN_PUSH, movPiece, NO_PIECE );
	}*/

	// Castling availability
	gameInfo_.castlingRights = info.castlingRights;

	// Halfmove clock
	gameInfo_.fiftyCount = info.fiftyCount;
	// Fullmove number
	gameInfo_.fullMoveCount = info.fullMoveCount;
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
	Board& rBoard = board_;

	// First print of board
	PrintBoardAndMove(Move::EmptyMove());

	/*
	*	Main game loop
	*	Player->GetMove() is the main driver of the game
	*/
	for (;;)
	{
		// Hent traekket fra den aktive spiller - GameInfo get updated every time
		Move newMove = GetCurrentPlayer().GetMove(gameInfo_, SearchLimits{});

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
		rBoard.ResetSearchDepth();

		// Traekket er godkendt! Vi spiller videre!!

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

		// Can we see a mate?
		if (score >= GameValues::Mate_Threshold || (-score >= GameValues::Mate_Threshold))
			// Antal ply: GameValues::Mate-Lastscore. Antal traek: Ply+1/2 => (MATE-Last)/2
			sstream << "Check mate in " << ((GameValues::Mate - abs(score) + 1) / 2) << " moves\n";
		else
			sstream << "Score: " << score << '\n';

		spdlog::default_logger()->info(sstream.str());
	}
	else	// TODO: This should be moved to the OnGameStateChanged event method
	{
		switch (gameInfo_.gameState)
		{
		case GameStates::WHITE_WON:
			spdlog::default_logger()->info("CHECK MATE ! White is the Winner!");
			break;
		case GameStates::BLACK_WON:
			spdlog::default_logger()->info("CHECK MATE ! Black is the Winner!");
			break;
		case GameStates::DRAW_PAT:
			spdlog::default_logger()->info("Its a Draw ! No more legal moves - but not in check!");
			break;
		case GameStates::DRAW_50_MOVES:
			spdlog::default_logger()->info("Its a Draw ! 50 moves has passed since last Capture or peasant move!");
			break;
		case GameStates::HUMAN_EXITED:
			spdlog::default_logger()->info("Human player exited game. Opponent is the Winner!");
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
	const Board& board = board_;
	// Print to various places
	std::stringstream sstream;
	sstream << "\nBoard " << GetBoardCount() << "\n\n";
	sstream << board;

	if (!move.is_null())
	{
		// MoveFormatter requires the board to be in the post-DoMove state, which it is
		// at this call site. GetPiece(to) returns the moved (or promoted) piece.
		sstream << "Last move: " << MoveFormatter::ToShort(move, board) << '\n';
		sstream << MoveFormatter::ToVerbose(move, board) << '\n';
	}

	spdlog::default_logger()->info(sstream.str());
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
	// Called from AddGameMove(), which is called after DoMove() in Run().
	// MoveFormatter requires the board to be in the post-DoMove state, which it is here.
	const Move& move = m_GameMoves.back();
	movesFile_ << "Move " << m_GameMoves.size() << '\n'; //-V128
	movesFile_ << "Last move: " << MoveFormatter::ToShort(move, board_) << '\n';
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
*      Version: 0.8             *
*********************************
)";
	errno_t err = localtime_s(&timeinfo, &now_c);	// MSVC complains about localtime() usage
	// Print the current time -  // RFC 1123 format is like: "Sun, 06 Nov 1994 08:49:37 GMT"
	if (err == 0)
	{
		file << "Time: " << std::put_time(&timeinfo, "%a, %d %b %Y %H:%M:%S %Z") << "\n\n";
	}

	file << "----------------------------------------------\n";
	file << "White:" << m_pPlayers[WHITE]->getDescription() << '\n';
	file << "Black:" << m_pPlayers[BLACK]->getDescription() << '\n';
	file << "----------------------------------------------\n";
}
