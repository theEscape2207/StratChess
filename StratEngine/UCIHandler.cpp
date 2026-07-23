// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "UCIHandler.h"
#include "AIPerplex.h"
#include "PlayerAI.h"
#include "MoveFormatter.h"
#include "Board.h"
#include "Eval.h"
#include "defines.h"
#include <spdlog/spdlog.h>
#include <iostream>
#include <sstream>

static constexpr std::string_view STARTING_FEN =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
static constexpr unsigned UCI_DEFAULT_DEPTH = 20;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void UciHandler::send(std::string_view msg)
{
    std::cout << msg << '\n';
    std::cout.flush();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

UciHandler::UciHandler() = default;

UciHandler::~UciHandler()
{
    stop_and_join();
}

void UciHandler::init_ai()
{
    auto base = PlayerBase::Create(PlayerBase::ePlayerTypes::AI_PERPLEX, UCI_DEFAULT_DEPTH, board_);
    AIPerplex::SetVerboseLogging(false);
    base->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);   // public via IPlayer; must be before downcast
    ai_.reset(dynamic_cast<PlayerAiBase*>(base.release()));
    // Restore the last client-configured thread count — init_ai() rebuilds
    // ai_ from scratch (AIPerplex threads_ defaults to 1), so without this
    // a prior 'setoption name Threads value N' would be silently lost on
    // every cmd_ucinewgame().
    if (ai_) ai_->SetThreads(configured_threads_);
}

// ---------------------------------------------------------------------------
// Command implementations
// ---------------------------------------------------------------------------

void UciHandler::cmd_uci()
{
    send("id name StratChess");
    send("id author Thees");
    send("option name Threads type spin default 1 min 1 max 32");
    send("uciok");
}

void UciHandler::cmd_isready()
{
    send("readyok");
}

void UciHandler::cmd_ucinewgame()
{
    stop_and_join();
    init_ai();
    board_.SetupFromFEN(std::string(STARTING_FEN));
}

void UciHandler::cmd_position(std::string_view line)
{
    if (line.find("startpos") != std::string_view::npos) {
        board_.SetupFromFEN(std::string(STARTING_FEN));
    } else {
        auto fen_pos = line.find("fen ");
        if (fen_pos != std::string_view::npos) {
            auto fen_start = fen_pos + 4;
            auto moves_pos = line.find(" moves", fen_start);
            std::string fen = (moves_pos != std::string_view::npos)
                ? std::string(line.substr(fen_start, moves_pos - fen_start))
                : std::string(line.substr(fen_start));
            board_.SetupFromFEN(fen);
        }
    }

    // Apply move list if present
    auto moves_pos = line.find("moves ");
    if (moves_pos != std::string_view::npos) {
        std::string moves_str(line.substr(moves_pos + 6));
        std::istringstream ss(moves_str);
        std::string token;
        while (ss >> token) {
            Move m = MoveFormatter::FromUCI(token, board_);
            if (!m.is_null()) {
                board_.DoMove(m);
                // Each replayed move is permanent, never undone — reset per
                // move (exactly like Game.cpp after every committed move), NOT
                // once after the loop: the ply-indexed history arrays hold
                // MAX_PLY entries, so a single post-loop reset lets DoMove
                // write out of bounds during any replay longer than MAX_PLY
                // plies (issue #53 follow-up; found by the first fastchess
                // smoke match — 265-ply game, access violation in Release).
                board_.ResetSearchDepth();
            }
        }
    }
}

void UciHandler::cmd_go(std::string_view line)
{
    stop_and_join();

    GoParams p = parse_go(line);
    GameInfo info = board_.GetGameInfo();
    const bool white = (board_.GetCurrentColor() == WHITE);

    // Build the per-call constraints — cmd_go no longer mutates AI state.
    SearchLimits limits;
    if (p.movetime > 0) {
        limits.movetime = std::chrono::milliseconds(p.movetime);
    } else if (p.wtime > 0 || p.btime > 0) {
        limits.clock = ClockInfo{ std::chrono::milliseconds(white ? p.wtime : p.btime),
                                  std::chrono::milliseconds(white ? p.winc  : p.binc),
                                  p.movestogo };
    } else if (!p.infinite && p.depth <= 0) {
        // No constraints at all — apply a safe fallback.
        limits.movetime = std::chrono::seconds(10);
    }
    limits.infinite = p.infinite;
    limits.depth = (p.depth > 0) ? std::optional<int>(p.depth)
                                 : std::optional<int>(p.infinite ? 50 : static_cast<int>(UCI_DEFAULT_DEPTH));

    auto start = std::chrono::steady_clock::now();
    search_thread_ = std::thread([this, info, start, limits]() mutable {
        Move best = ai_->GetMove(info, limits);

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

        auto* perplex = dynamic_cast<AIPerplex*>(ai_.get());
        SearchResult result = perplex ? perplex->GetLastResult() : SearchResult{};

        const int cp = result.best_score;
        const bool is_mate = std::abs(cp) >= GameValues::Mate_Threshold;

        std::string score_str;
        if (is_mate) {
            int plies  = GameValues::Mate - std::abs(cp);
            int mate_n = (plies + 1) / 2;
            score_str  = "mate " + std::to_string(cp > 0 ? mate_n : -mate_n);
        } else {
            score_str = "cp " + std::to_string(cp);
        }

        send("info depth "  + std::to_string(result.depth_completed) +
             " score "      + score_str                               +
             " nodes "      + std::to_string(result.nodes_searched)   +
             " time "       + std::to_string(elapsed.count())         +
             " pv "         + (best.is_null() ? "0000" : MoveFormatter::ToUCI(best)));

        const std::string bm = best.is_null() ? "0000" : MoveFormatter::ToUCI(best);
        send("bestmove " + bm);
    });
}

void UciHandler::cmd_stop()
{
    stop_and_join();
}

void UciHandler::cmd_setoption(std::string_view line)
{
    // Minimal UCI 'setoption' parser — recognizes exactly:
    //   setoption name Threads value N
    // Any other option name, or a malformed/missing value, is silently
    // ignored (standard UCI convention — same as unknown top-level commands
    // in run()). Case-sensitive match on "Threads", matching the convention
    // used by Stockfish and other engines.
    auto trim = [](std::string_view s) {
        const size_t b = s.find_first_not_of(' ');
        if (b == std::string_view::npos) return std::string_view{};
        const size_t e = s.find_last_not_of(' ');
        return s.substr(b, e - b + 1);
    };

    const auto name_pos = line.find("name");
    if (name_pos == std::string_view::npos) return;

    const auto value_pos = line.find("value");
    const std::string_view name = trim((value_pos != std::string_view::npos)
        ? line.substr(name_pos + 4, value_pos - (name_pos + 4))
        : line.substr(name_pos + 4));

    if (name != "Threads" || value_pos == std::string_view::npos) return;

    const std::string_view value_str = trim(line.substr(value_pos + 5));
    if (value_str.empty()) return;
    for (char c : value_str) {
        if (c < '0' || c > '9') return;   // non-numeric — ignore malformed value
    }

    unsigned n = 0;
    try {
        n = static_cast<unsigned>(std::stoul(std::string(value_str)));
    } catch (...) {
        return;   // out-of-range or otherwise unparsable — ignore
    }

    configured_threads_ = n;
    if (ai_) ai_->SetThreads(n);
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void UciHandler::stop_and_join()
{
    if (ai_) ai_->StopSearch();
    if (search_thread_.joinable()) search_thread_.join();
    if (ai_) ai_->SetMaxDepth(UCI_DEFAULT_DEPTH);
    if (ai_) ai_->SetTimeLimit(std::chrono::seconds(15));
}

UciHandler::GoParams UciHandler::parse_go(std::string_view line)
{
    GoParams p;
    std::string line_str(line);
    std::istringstream ss(line_str);
    std::string token;
    while (ss >> token) {
        if      (token == "wtime")     { ss >> p.wtime; }
        else if (token == "btime")     { ss >> p.btime; }
        else if (token == "winc")      { ss >> p.winc; }
        else if (token == "binc")      { ss >> p.binc; }
        else if (token == "movestogo") { ss >> p.movestogo; }
        else if (token == "depth")     { ss >> p.depth; }
        else if (token == "movetime")  { ss >> p.movetime; }
        else if (token == "infinite")  { p.infinite = true; }
    }
    return p;
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void UciHandler::run()
{
    init_ai();
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "uci")                        { cmd_uci(); }
        else if (line == "isready")               { cmd_isready(); }
        else if (line == "ucinewgame")            { cmd_ucinewgame(); }
        else if (line.starts_with("position"))    { cmd_position(line); }
        else if (line.starts_with("go"))          { cmd_go(line); }
        else if (line.starts_with("setoption"))   { cmd_setoption(line); }
        else if (line == "stop")                  { cmd_stop(); }
        else if (line == "quit")                  { cmd_stop(); break; }
        // unknown commands: ignore silently (UCI spec)
    }
}
