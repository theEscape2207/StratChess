// UCIHandler.h — UCI protocol command loop for StratChess engine.
#pragma once
#include <memory>
#include <string>
#include <thread>
#include "PlayerBase.h"
#include "PlayerAI.h"
#include "GameState.h"

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
    void cmd_position(std::string_view line);
    void cmd_go(std::string_view line);
    void cmd_stop();

    void stop_and_join();   // signal + join search thread
    void init_ai();         // (re)create AIPerplex instance

    static void send(std::string_view msg);   // writes line to stdout + flush

    std::unique_ptr<PlayerAiBase> ai_;
    std::thread search_thread_;
};
