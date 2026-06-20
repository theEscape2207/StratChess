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
    auto base = PlayerBase::Create(PlayerBase::ePlayerTypes::AI_PERPLEX, UCI_DEFAULT_DEPTH);
    AIPerplex::SetVerboseLogging(false);
    base->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);   // public via IPlayer; must be before downcast
    ai_.reset(dynamic_cast<PlayerAiBase*>(base.release()));
}

// ---------------------------------------------------------------------------
// Command implementations
// ---------------------------------------------------------------------------

void UciHandler::cmd_uci()
{
    send("id name StratChess");
    send("id author Thees");
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
    Board::Instance().SetupFromFEN(std::string(STARTING_FEN));
}

void UciHandler::cmd_position(std::string_view line)
{
    if (line.find("startpos") != std::string_view::npos) {
        Board::Instance().SetupFromFEN(std::string(STARTING_FEN));
    } else {
        auto fen_pos = line.find("fen ");
        if (fen_pos != std::string_view::npos) {
            auto fen_start = fen_pos + 4;
            auto moves_pos = line.find(" moves", fen_start);
            std::string fen = (moves_pos != std::string_view::npos)
                ? std::string(line.substr(fen_start, moves_pos - fen_start))
                : std::string(line.substr(fen_start));
            Board::Instance().SetupFromFEN(fen);
        }
    }

    // Apply move list if present
    auto moves_pos = line.find("moves ");
    if (moves_pos != std::string_view::npos) {
        std::string moves_str(line.substr(moves_pos + 6));
        std::istringstream ss(moves_str);
        std::string token;
        while (ss >> token) {
            Move m = MoveFormatter::FromUCI(token, Board::Instance());
            if (!m.is_null()) {
                Board::Instance().DoMove(m);
            }
        }
        // These moves are a permanent replay, never undone — reset the
        // undo-stack depth so it only spans the search that follows (issue #53).
        Board::Instance().ResetSearchDepth();
    }
}

void UciHandler::cmd_go(std::string_view line)
{
    stop_and_join();

    GoParams p = parse_go(line);
    GameInfo info = Board::Instance().GetGameInfo();
    const bool white = (Board::Instance().GetCurrentColor() == WHITE);

    // Configure time — pick first matching case
    if (p.movetime > 0) {
        ai_->SetTimeLimit(std::chrono::milliseconds(p.movetime));
    } else if (p.wtime > 0 || p.btime > 0) {
        auto remaining = std::chrono::milliseconds(white ? p.wtime : p.btime);
        auto inc       = std::chrono::milliseconds(white ? p.winc  : p.binc);
        ai_->SetClockInfo(remaining, inc, p.movestogo);
    } else if (p.infinite || p.depth > 0) {
        // Pure depth constraint or infinite: give the engine a large time budget.
        // The depth cap in iterative_deepening() is the sole stopping criterion.
        // For go infinite, the UCI 'stop' command is the intended termination.
        // hours(1) avoids potential overflow from milliseconds::max() in comparisons.
        ai_->SetTimeLimit(std::chrono::hours(1));
    } else {
        // No constraints at all — apply a safe fallback.
        ai_->SetTimeLimit(std::chrono::seconds(10));
    }

    if (p.depth > 0) {
        ai_->SetMaxDepth(static_cast<unsigned>(p.depth));
    } else {
        ai_->SetMaxDepth(p.infinite ? 50u : UCI_DEFAULT_DEPTH);
    }

    auto start = std::chrono::steady_clock::now();
    search_thread_ = std::thread([this, info, start]() mutable {
        Move best = ai_->GetMove(info);

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
        else if (line == "stop")                  { cmd_stop(); }
        else if (line == "quit")                  { cmd_stop(); break; }
        // unknown commands: ignore silently (UCI spec)
    }
}
