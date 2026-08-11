// UCIHandler.h — UCI protocol command loop for StratChess engine.
#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <thread>
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

#ifdef STRAT_ENABLE_TEST_ACCESS
    // Enables unit tests for private command handlers (cmd_position replay).
    // Same mechanism as AIPerplex's fixture; defined only in the test project.
    friend class UciHandlerTestFixture;
#endif
};
