// UCIHandler.h — UCI protocol command loop for StratChess engine.
#pragma once
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
    void cmd_stop();
    void cmd_setoption(std::string_view line);

    void stop_and_join();   // signal + join search thread
    void init_ai();         // (re)create AIPerplex instance

    static void send(std::string_view msg);   // writes line to stdout + flush

    // Must be declared (and thus constructed/destroyed) before ai_ —
    // ai_ holds a Board& reference into it that must outlive it.
    Board board_;

    std::unique_ptr<PlayerAiBase> ai_;
    std::unique_ptr<EvalManager> eval_;
    std::thread search_thread_;

    // Last thread count from a client 'setoption name Threads value N'.
    // Persists across init_ai() calls (cmd_ucinewgame() rebuilds ai_ from
    // scratch, which would otherwise silently reset the thread count back
    // to the AIPerplex default of 1).
    unsigned configured_threads_{ 1 };

#ifdef STRAT_ENABLE_TEST_ACCESS
    // Enables unit tests for private command handlers (cmd_position replay).
    // Same mechanism as AIPerplex's fixture; defined only in the test project.
    friend class UciHandlerTestFixture;
#endif
};
