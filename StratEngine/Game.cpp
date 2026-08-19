// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include "StdAfx.h"
#include "Game.h"
#include "Board.h"
#include "MoveFormatter.h"
#include "PlayerBase.h" // For factory create
#include "PlayerAI.h"   // For PlayerAiBase setters
#include "AIPerplex.h"  // For SearchTuning application
#include <iomanip>      // std::put_time

// ***************************************
// Method:      Game
// Description: Constructor
// FullName:    public Game::Game
// Returns:      -
// Remark:
// ***************************************
Game::Game() { Init(); }

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
		unsubscribePlayerEvents();

		// Under VisualStudio, this must be called before main finishes to workaround a known VS issue
		spdlog::drop_all();
	} catch (const std::exception&) { // NOLINT(bugprone-empty-catch)
		                              // Don't care if any deregistration fails, we're closing here -- a
		                              // destructor must not let this propagate regardless
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
	for (const auto& m : game.m_GameMoves) {
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
IPlayer& Game::GetCurrentPlayer() const noexcept { return *m_pPlayers[board_.GetCurrentColor()]; }

//***************************************
// Method:      Init
// Description:
// FullName:    private Game::Init
// Returns:     void -
// Remark:
//***************************************
void Game::Init()
{
	try {
		// Initialize engine default logger
		Engine::Logger::InitDefault();
		auto& logger = *spdlog::get("multi_sink");

		// Create performance logger
		Engine::Logger::EnsurePerfLogger("logs/SimplePerfStats.txt");
		auto perf = Engine::Logger::GetPerfLogger();
		if (perf) {
			perf->info(
			    "No. of nodes  |  Ms used  |  Nodes pr. ms  |  Total nodes  |  Total time  |  Total nodes pr. ms");
			perf->info(
			    "-----------------------------------------------------------------------------------------------------");
		} else {
			std::cout
			    << "No. of nodes  |  Ms used  |  Nodes pr. ms  |  Total nodes  |  Total time  |  Total nodes pr. ms\n";
			std::cout
			    << "-----------------------------------------------------------------------------------------------------\n";
		}

		// Existing startup code
		LoadConfigFileSettings();

		CreateGameMoveFile();

		std::stringstream sstream;
		logger.info(sstream.str());
		AddFileHeader(sstream);

	} catch (const spdlog::spdlog_ex& ex) {
		std::cout << "Log initialization failed: " << ex.what() << '\n';
	} catch (const std::exception& ex) {
		// Everything else in Init -- config parsing, player construction, board
		// setup -- used to propagate out of main and end the process with no
		// message at all, which makes a bad settings file look like a crash.
		std::cerr << "Game initialization failed: " << ex.what() << '\n';
		throw;
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
	Config reader;
	reader.ReadConfigFile("game_settings.json", board_);

	// TODO: Setup board explicitly here
	spdlog::default_logger()->debug("Creating players from Config File");
	//TODO: We are creating stuff we do not need (e.g. eval engine as human and NewPVLineMove event as non-iterative AI)
	const Config::PlayerConfig whiteConfig = reader.GetPlayerFromConfig(true);
	m_pPlayers[WHITE] = SetPlayerParams(whiteConfig);
	player_limits_[WHITE] = whiteConfig.search_limits;

	// Create black player
	const Config::PlayerConfig blackConfig = reader.GetPlayerFromConfig(false);

	m_pPlayers[BLACK] = SetPlayerParams(blackConfig);
	player_limits_[BLACK] = blackConfig.search_limits;
}

std::unique_ptr<IPlayer> Game::SetPlayerParams(const Config::PlayerConfig& config)
{
	auto player = PlayerBase::Create(static_cast<PlayerBase::ePlayerTypes>(config.type), config.depth, board_);
	player->SetEvalEngine(static_cast<EvalManager::EvalTypes>(config.eval));

	// Enable AIPerplex verbose logging in game mode (opt-in here; UCI/test modes disable it).
	if (dynamic_cast<AIPerplex*>(player.get())) {
		AIPerplex::SetVerboseLogging(true);
	}

	// Apply SearchTuning — only valid for AI_PERPLEX (type 6)
	if (config.search_tuning.has_value()) {
		constexpr unsigned kAiPerplex = static_cast<unsigned>(PlayerBase::ePlayerTypes::AI_PERPLEX);
		if (config.type != kAiPerplex) {
			spdlog::warn("search_tuning in game_settings.json is ignored for player type {} "
			             "(only supported by AI_PERPLEX)",
			             config.type);
		} else if (auto* perplex = dynamic_cast<AIPerplex*>(player.get())) {
			const auto& st = *config.search_tuning;
			auto& t = perplex->tuning();
			t.min_nodes_threshold = st.min_nodes_threshold;
			t.min_completion_ratio = st.min_completion_ratio;
			t.min_pv_ratio = st.min_pv_ratio;
			t.score_draw_threshold = st.score_draw_threshold;
			t.delta_pruning_margin = st.delta_pruning_margin;
			t.aspiration_initial_delta = st.aspiration_initial_delta;
			t.aspiration_max_retries = st.aspiration_max_retries;
			t.aspiration_enabled = st.aspiration_enabled;
		}
	}

	// Configure AI-only options and signal the new-game lifecycle before Run()
	// can request a move. Legacy AIs inherit no-op implementations; non-AI
	// players are skipped because the dynamic_cast fails.
	if (auto* ai = dynamic_cast<PlayerAiBase*>(player.get())) {
		if (config.threads.has_value()) {
			ai->SetThreads(*config.threads);
		}
		ai->StartNewGame();
	}

	//Register events
	player->ENewPVLineMove.subscribe([this](const void* s, const PVLine& pvl) { onNewPVLineMove(s, pvl); });
	return player;
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
	for (;;) {
		// The mover and its result are both captured before anything is committed: DoMove
		// flips the side to move, so GetCurrentPlayer() would name the opponent afterwards,
		// and the score to report belongs to the player that just moved.
		IPlayer& mover = GetCurrentPlayer();
		const SearchResult result = mover.GetMove(player_limits_[board_.GetCurrentColor()]);

		bool committed = false;
		if (!result.best_move.is_null()) {
			// Foretag traekket paa det virkelige braet
			committed = rBoard.DoMove(result.best_move);
			assert(committed && "Unexpected illegal move found! Exiting...");
			if (committed) {
				rBoard.ResetSearchDepth();

				// Tilfoej traekket til traeklisten og opdater spil-variable
				AddGameMove(result.best_move);
			}
		}

		game_state_ = result.game_state;

		// The fifty-move rule is a fact about the position the board now holds, so it is
		// adjudicated here rather than by a search that never visits that position. It can
		// only turn a still-running game into a draw: a mate, a stalemate or HUMAN_EXITED
		// already reported by the mover takes precedence and is never overwritten.
		if (committed && game_state_ == GameStates::STILL_PLAYING && board_.halfmove_clock() >= HALFMOVE_CLOCK_LIMIT)
			game_state_ = GameStates::DRAW_50_MOVES;

		// Prints out the current score message for AI players (score or "Mate in x moves")
		PrintStateMessage(mover);

		if (committed)
			// Print the board and last move to screen (and debug)
			PrintBoardAndMove(result.best_move);

		if (!IsStillPlaying()) // Test om spillet er slut
			break;

		// A null move with the game still running means the user typed "exit" or "quit".
		if (HasHumanExited(result.best_move)) {
			spdlog::default_logger()->warn("User has exited the game\n");
			break;
		}
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
void Game::PrintStateMessage(const IPlayer& mover) const
{
	if (IsStillPlaying()) {
		if (mover.IsHuman()) // score doesn't make sense
			return;

		std::stringstream sstream;
		// AIs
		const int score = mover.GetBestScore();

		// Can we see a mate?
		if (score >= GameValues::Mate_Threshold || (-score >= GameValues::Mate_Threshold))
			// Antal ply: GameValues::Mate-Lastscore. Antal traek: Ply+1/2 => (MATE-Last)/2
			sstream << "Check mate in " << ((GameValues::Mate - abs(score) + 1) / 2) << " moves\n";
		else
			sstream << "Score: " << score << '\n';

		spdlog::default_logger()->info(sstream.str());
	} else {
		switch (game_state_) {
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

	if (!move.is_null()) {
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
void Game::AddFileHeader(std::ostream& file) const
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
	bool ok = STRAT_LOCALTIME(&timeinfo, &now_c); // plain localtime() is not thread-safe
	// Print the current time -  // RFC 1123 format is like: "Sun, 06 Nov 1994 08:49:37 GMT"
	if (ok) {
		file << "Time: " << std::put_time(&timeinfo, "%a, %d %b %Y %H:%M:%S %Z") << "\n\n";
	}

	file << "----------------------------------------------\n";
	file << "White:" << m_pPlayers[WHITE]->getDescription() << '\n';
	file << "Black:" << m_pPlayers[BLACK]->getDescription() << '\n';
	file << "----------------------------------------------\n";
}
