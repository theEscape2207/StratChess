// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include "StdAfx.h"
#include "Game.h"
#include "Board.h"
#include "IPlayer.h"
#include "MoveFormatter.h"
#include "PlayerFactory.h"
#include <iomanip> // std::put_time

// ***************************************
// Method:      Game
// Description: Constructor
// FullName:    public Game::Game
// Returns:      -
// Remark:
// ***************************************
Game::Game() { Init(); }

// Test-seam constructor: an explicit position and two supplied players, with none of
// Init()'s settings-file reading or log-file creation. Everything Run() touches is either
// set here or default-constructed — m_GameMoves is empty and movesFile_ stays closed, which
// PrintGameMoves() tolerates.
Game::Game(const std::string& fen, std::unique_ptr<IPlayer> white, std::unique_ptr<IPlayer> black)
{
	owns_logging_ = false;
	if (!board_.SetupFromFEN(fen))
		board_.SetDefaultBoard();
	m_pPlayers[WHITE] = std::move(white);
	m_pPlayers[BLACK] = std::move(black);
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
	try {
		unsubscribePlayerEvents();

		// Under VisualStudio, this must be called before main finishes to workaround a known VS issue
		if (owns_logging_)
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
	// TODO: Calling clear() where unsubscribe() should be used, but we have no handles to unsubscribe with here.
	// Deregister our delegates. Both players always exist when Init() built them; the test-seam
	// constructor takes whatever it is handed, so neither is assumed.
	for (auto& player : m_pPlayers)
		if (player)
			player->ENewPVLineMove.clear();
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
	auto player = CreatePlayer(config, board_, {.verbose_search_logging = true});

	// Register only after construction and initial lifecycle configuration are complete.
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
		RecordPerformance(mover, result);

		bool committed = false;
		if (!result.best_move.is_null()) {
			// Foretag traekket paa det virkelige braet
			committed = rBoard.DoMove(result.best_move);
			assert(committed && "Unexpected illegal move found! Exiting...");
			if (!committed) {
				// Only an engine bug gets here. Stopping matters anyway: a rejected move leaves the
				// side to move unchanged, so the loop would ask the same player for the same move
				// forever in a build where the assert above is compiled out.
				spdlog::default_logger()->error("Player returned an illegal move - stopping the game");
				break;
			}
			rBoard.ResetSearchDepth();

			// Tilfoej traekket til traeklisten og opdater spil-variable
			AddGameMove(result.best_move);
		}

		game_state_ = result.game_state;

		// The fifty-move rule is a fact about the position the board now holds, so it is
		// adjudicated here rather than by a search that never visits that position. It can
		// only turn a still-running game into a draw: a mate, a stalemate or HUMAN_EXITED
		// already reported by the mover takes precedence and is never overwritten.
		if (committed && game_state_ == GameStates::STILL_PLAYING && board_.halfmove_clock() >= HALFMOVE_CLOCK_LIMIT)
			game_state_ = GameStates::DRAW_50_MOVES;

		// Prints out the current score message for AI players (score or "Mate in x moves")
		PrintStateMessage(mover, result);

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

void Game::RecordPerformance(const IPlayer& mover, const SearchResult& result)
{
	if (mover.IsHuman())
		return;

	const int64_t nodes = result.nodes_searched + result.qnodes_searched;
	total_nodes_ += nodes;
	total_elapsed_ += result.elapsed;

	// Preserve the historic 0ms guard for the display and divisions while keeping the
	// accumulated telemetry itself equal to the exact returned-search sums.
	auto elapsed_for_display = result.elapsed;
	if (elapsed_for_display == std::chrono::milliseconds::zero())
		elapsed_for_display = std::chrono::milliseconds(1);
	auto total_elapsed_for_display = total_elapsed_;
	if (total_elapsed_for_display == std::chrono::milliseconds::zero())
		total_elapsed_for_display = std::chrono::milliseconds(1);

	auto perf = Engine::Logger::GetPerfLogger();
	if (!perf)
		return;

	const int64_t nodes_per_ms = nodes / elapsed_for_display.count();
	const int64_t total_nodes_per_ms = total_nodes_ / total_elapsed_for_display.count();
	perf->info("{:>10} {:>13} {:>13} {:>19} {:>13} {:>13}", nodes, elapsed_for_display.count(), nodes_per_ms,
	           total_nodes_, total_elapsed_for_display.count(), total_nodes_per_ms);
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
void Game::PrintStateMessage(const IPlayer& mover, const SearchResult& result) const
{
	if (IsStillPlaying()) {
		if (mover.IsHuman()) // score doesn't make sense
			return;

		std::stringstream sstream;
		// AIs
		const int score = result.best_score;

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
	const bool ok = STRAT_LOCALTIME(&timeinfo, &now_c); // plain localtime() is not thread-safe
	// Print the current time -  // RFC 1123 format is like: "Sun, 06 Nov 1994 08:49:37 GMT"
	if (ok) {
		file << "Time: " << std::put_time(&timeinfo, "%a, %d %b %Y %H:%M:%S %Z") << "\n\n";
	}

	file << "----------------------------------------------\n";
	file << "White:" << m_pPlayers[WHITE]->getDescription() << '\n';
	file << "Black:" << m_pPlayers[BLACK]->getDescription() << '\n';
	file << "----------------------------------------------\n";
}
