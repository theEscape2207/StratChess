// UCIHandler.h — UCI protocol command loop for StratChess engine.
#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <spdlog/spdlog.h>
#include "PlayerBase.h"
#include "PlayerAI.h"
#include "GameState.h"
#include "Board.h"

class EvalManager;
class UciHandler {
public:
    UciHandler();
    ~UciHandler();
    void run();   // blocking command loop; reads from stdin

    /// Start logging every received command to `filename`, one line each.
    /// Off unless a caller asks for it: 'uci --log-commands[=path]'.
    /// Returns false if the file could not be opened, in which case nothing is logged.
    bool EnableCommandLog(const std::string& filename);

    /// Default log path for EnableCommandLog. The process id is part of the name because a match
    /// at -Concurrency 6 runs six engines from one working directory, and spdlog's file sink is
    /// thread-safe but not process-safe — one shared path yields interleaved or truncated files.
    static std::string DefaultCommandLogPath();

    /// Parameters parsed from a UCI 'go' command line.
    /// Public so unit tests can call parse_go() directly without a running handler.
    struct GoParams {
        int wtime     = 0;
        int btime     = 0;
        int winc      = 0;
        int binc      = 0;
        int movestogo = 0;
        int depth     = 0;     // 0 = use engine default
        int movetime  = 0;     // 0 = use clock / depth
        bool infinite = false;
    };

    /// Parse a UCI 'go' line into a GoParams struct.
    /// Pure function — no side effects; public for unit testing.
    static GoParams parse_go(std::string_view line);

private:
    void cmd_uci();
    void cmd_isready();
    void cmd_ucinewgame();
    void cmd_eval();
    void cmd_position(std::string_view line);
    void cmd_go(std::string_view line);
    void cmd_perft(std::string_view line);
    void cmd_stop();
    void cmd_setoption(std::string_view line);

    /// Routes one command line. Returns false for 'quit', which ends the loop.
    /// Separate from run() so the routing — and the command log — are unit-testable without
    /// feeding stdin.
    bool dispatch(std::string_view line);

    void stop_and_join();   // signal + join search thread
    void init_ai();         // construct the AIPerplex instance (once; see cmd_ucinewgame())

    /// Reports and returns true when a search is in flight, for the commands
    /// that mutate state the search is reading. Named for the decision it
    /// carries: those commands are refused, not queued and not honoured.
    bool refuse_while_searching(std::string_view command);

    static void send(std::string_view msg);   // writes line to stdout + flush

    // Must be declared (and thus constructed/destroyed) before ai_ —
    // ai_ holds a Board& reference into it that must outlive it.
    Board board_;

    std::unique_ptr<PlayerAiBase> ai_;
    std::unique_ptr<EvalManager> eval_;
    std::thread search_thread_;

    // Whether a search is actually running. search_thread_.joinable() cannot
    // answer this: a std::thread stays joinable after its function returns,
    // until someone joins it, and cmd_go only joins at the start of the NEXT
    // search. Testing joinable() would therefore refuse the 'position' of every
    // normal go -> bestmove -> position cycle.
    std::atomic<bool> searching_{ false };

    // Last thread count from a client 'setoption name Threads value N'.
    // Only needed for the case where 'setoption' arrives before ai_ exists
    // (init_ai() applies it once, at construction) — once ai_ exists,
    // 'setoption' also calls ai_->SetThreads() directly, and ai_ persists
    // across cmd_ucinewgame() (see AIPerplex::StartNewGame()), so there is
    // no later point where the live thread count needs restoring.
    unsigned configured_threads_{ 1 };

    // Null unless EnableCommandLog() succeeded. Owned here and nowhere else — it is deliberately
    // not registered with spdlog (see Logger::CreateUciCommandLogger), so the file is closed when
    // this handler is destroyed.
    std::shared_ptr<spdlog::logger> command_log_;

#ifdef STRAT_ENABLE_TEST_ACCESS
    // Enables unit tests for private command handlers (cmd_position replay).
    // Same mechanism as AIPerplex's fixture; defined only in the test project.
    friend class UciHandlerTestFixture;
#endif
};
