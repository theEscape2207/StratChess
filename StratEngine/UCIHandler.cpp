// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "UCIHandler.h"
#include "AIPerplex.h"
#include "PlayerAI.h"
#include "MoveFormatter.h"
#include "Board.h"
#include "Tests/Perft.h"
#include "Eval.h"
#include "defines.h"
#include "Utils/Logger.h"
#include <spdlog/spdlog.h>
#include <iostream>
#include <sstream>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

static constexpr std::string_view STARTING_FEN =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
static constexpr unsigned UCI_DEFAULT_DEPTH = 20;

// Same bound the 'perft' CLI subcommand enforces, so a mistyped depth cannot
// start a run that will never finish.
static constexpr int PERFT_MAX_DEPTH = 10;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void UciHandler::send(std::string_view msg)
{
    std::cout << msg << '\n';
    std::cout.flush();
}

static int current_process_id()
{
#ifdef _WIN32
    return _getpid();
#else
    return static_cast<int>(getpid());
#endif
}

std::string UciHandler::DefaultCommandLogPath()
{
    return "logs/uci_commands_" + std::to_string(current_process_id()) + ".log";
}

bool UciHandler::EnableCommandLog(const std::string& filename)
{
    command_log_ = Engine::Logger::CreateUciCommandLogger(filename);
    return command_log_ != nullptr;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

UciHandler::UciHandler()
    // eval_ is constructed here, not in init_ai(): EvalManager holds no
    // per-game state (see the Lazy SMP sharing contract comment in Eval.h),
    // so there is nothing for it to reset between games regardless of where
    // it is constructed. Matches the COMPLEX type init_ai() configures for
    // the search evaluator.
    : eval_(EvalManager::Create(EvalManager::EvalTypes::COMPLEX))
{
}

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
    // Apply any thread count set via 'setoption' before ai_ existed (a
    // client may configure options before the first 'ucinewgame'). ai_ now
    // persists across games — cmd_ucinewgame() calls StartNewGame() instead
    // of rebuilding — so this only ever runs once per process, not on every
    // new game.
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
    // Hash budgets TT entry bytes. Arbitrary values round down to a power-of-two
    // bucket count; exact-fit values include 192 / 384 / 768 / 1536. The
    // separately queryable lock_bytes() is additional memory.
    send("option name Hash type spin default " + std::to_string(AIPerplex::DEFAULT_HASH_MB) +
         " min " + std::to_string(AIPerplex::MIN_HASH_MB) +
         " max " + std::to_string(AIPerplex::MAX_HASH_MB));
    send("uciok");
}

void UciHandler::cmd_isready()
{
    send("readyok");
}

void UciHandler::cmd_ucinewgame()
{
    stop_and_join();
    // run() constructs ai_ once before the command loop starts, so this is
    // normally already true. It can still be null here in a test fixture
    // that calls cmd_ucinewgame() directly without run().
    if (!ai_) init_ai();
    if (ai_) ai_->StartNewGame();

    // STARTING_FEN is a compile-time constant; a false return here would mean the constant itself
    // is malformed, so it is asserted rather than handled.
    [[maybe_unused]] const bool ok = board_.SetupFromFEN(std::string(STARTING_FEN));
    assert(ok && "STARTING_FEN failed to parse");
}

// Column layout for the per-term breakdown table (issue #129 phase 2). The
// widths are shared by the header, the rules and every row so the columns line
// up, and are named here rather than repeated as literals in each format call.
static constexpr int EVAL_TERM_COL  = 11;   // term name, left-aligned
static constexpr int EVAL_VALUE_COL = 7;    // each of white / black / net, right-aligned
// Total printed width of one row: the term column, the first '|', then three
// value columns of which the last two are each preceded by " |". The 'sum'
// line is right-aligned to this so its figure lands under the net column.
static constexpr int EVAL_TABLE_WIDTH =
    EVAL_TERM_COL + 1 + EVAL_VALUE_COL + 2 * (2 + EVAL_VALUE_COL);

// Right-align `text` in a field of `width`; returns it unpadded if it is
// already wider, so an implausibly large score widens the table rather than
// being truncated into a wrong number.
static std::string pad_left(const std::string& text, int width)
{
    const int fill = width - static_cast<int>(text.size());
    return (fill > 0) ? std::string(static_cast<size_t>(fill), ' ') + text : text;
}

static std::string pad_right(const std::string& text, int width)
{
    const int fill = width - static_cast<int>(text.size());
    return (fill > 0) ? text + std::string(static_cast<size_t>(fill), ' ') : text;
}

// One table row: "<name> | <white> | <black> | <net>", with net always
// white-minus-black — the direction in which Evaluate() combines the two.
static std::string eval_term_row(const char* name, int white, int black)
{
    return pad_right(name, EVAL_TERM_COL)
         + "|" + pad_left(std::to_string(white), EVAL_VALUE_COL)
         + " |" + pad_left(std::to_string(black), EVAL_VALUE_COL)
         + " |" + pad_left(std::to_string(white - black), EVAL_VALUE_COL);
}

// Prints the static evaluation of the current position (board_), as set up
// by the last 'position' command (startpos default if none has run yet).
// Not a search response: no 'info'/'bestmove' output, so a GUI cannot
// mistake this for one (D6, .claude/plans/uci-eval-command-term-breakdown.md).
//
// Two total lines are printed because the engine's score is side-to-move-
// relative (positive = good for whoever moves next) and that sign convention
// is the single most confusing thing about reading this engine's output — see
// D5 in the plan. The White-POV line removes any need to mentally flip the
// sign when Black is to move.
//
// Above them, phase 2 prints a per-term breakdown (D10). It is emitted only
// for EvalComplex: the terms are its own, and EvalSimple has none to report,
// so the base EvalManager interface was left alone rather than given a debug
// method only one subclass could implement (D7). The downcast mirrors the one
// init_ai() already performs.
//
// When the breakdown is available, the totals below are taken from its `total`
// field — which is Evaluate()'s own return value (D8) — rather than from a
// second Evaluate() call here. That is what makes the printed table and the
// printed total provably the same evaluation of the same position, and not
// merely two evaluations that happen to agree. Without a breakdown the command
// falls back to calling Evaluate() directly, degrading to phase 1's output.
void UciHandler::cmd_eval()
{
    const bool white_to_move = (board_.GetCurrentColor() == WHITE);
    int score = 0;

    if (const auto* complex_eval = dynamic_cast<const EvalComplex*>(eval_.get())) {
        const EvalBreakdown terms = complex_eval->Breakdown(board_);
        const std::string rule = std::string(static_cast<size_t>(EVAL_TERM_COL), '-') + "+"
                               + std::string(static_cast<size_t>(EVAL_VALUE_COL) + 1, '-') + "+"
                               + std::string(static_cast<size_t>(EVAL_VALUE_COL) + 1, '-') + "+"
                               + std::string(static_cast<size_t>(EVAL_VALUE_COL), '-');

        send(pad_right("term", EVAL_TERM_COL)
             + "|" + pad_left("white", EVAL_VALUE_COL)
             + " |" + pad_left("black", EVAL_VALUE_COL)
             + " |" + pad_left("net", EVAL_VALUE_COL));
        send(rule);
        // The material row is king-inclusive (10000 cp per side) because it is
        // EvalContext::material verbatim — see that field's comment in Eval.h.
        // It cancels in the net column. Documented there rather than printed on
        // every invocation: it is a fixed property of the evaluator, not
        // information about the position being examined.
        send(eval_term_row("material", terms.material[WHITE], terms.material[BLACK]));
        send(eval_term_row("pawns",    terms.pawns[WHITE],    terms.pawns[BLACK]));
        send(eval_term_row("rooks",    terms.rooks[WHITE],    terms.rooks[BLACK]));
        send(eval_term_row("pst",      terms.pst[WHITE],      terms.pst[BLACK]));
        send(eval_term_row("mopup",    terms.mopup[WHITE],    terms.mopup[BLACK]));
        send(eval_term_row("bishops",  terms.bishops[WHITE],  terms.bishops[BLACK]));
        send(eval_term_row("castling", terms.castling[WHITE], terms.castling[BLACK]));
        send(eval_term_row("mobility", terms.mobility[WHITE], terms.mobility[BLACK]));
        send(rule);

        // The sum of the net column. It must equal the 'white pov' line below;
        // both are printed so a drift between the terms and Evaluate() is
        // visible on inspection, and asserted on in StratChessTests (D9).
        const int net_sum = (terms.material[WHITE] - terms.material[BLACK])
                          + (terms.pawns[WHITE]    - terms.pawns[BLACK])
                          + (terms.rooks[WHITE]    - terms.rooks[BLACK])
                          + (terms.pst[WHITE]      - terms.pst[BLACK])
                          + (terms.mopup[WHITE]    - terms.mopup[BLACK])
                          + (terms.bishops[WHITE]  - terms.bishops[BLACK])
                          + (terms.castling[WHITE] - terms.castling[BLACK])
                          + (terms.mobility[WHITE] - terms.mobility[BLACK]);
        const std::string sum_label = "sum (white pov)";
        send(sum_label + pad_left(std::to_string(net_sum),
                                  EVAL_TABLE_WIDTH - static_cast<int>(sum_label.size())));

        // Phase rather than a stage name (issue #99): the evaluator no longer
        // has a middlegame/endgame boolean, and how far through the taper a
        // position sits is what actually explains the pst row.
        send("phase: " + std::to_string(terms.phase) + "/" + std::to_string(MAX_GAME_PHASE));

        score = terms.total;
    }
    else {
        score = eval_->Evaluate(board_);
    }

    const int white_pov = white_to_move ? score : -score;

    send("static eval: " + std::to_string(score) + " cp (" +
         (white_to_move ? "White" : "Black") +
         " to move; positive favours the side to move)");
    send("white pov: " + std::to_string(white_pov) + " cp");
}

void UciHandler::cmd_position(std::string_view line)
{
    // The search reads the board through its own ThreadData copy, taken on the
    // search thread after cmd_go returns -- so mutating board_ here can land
    // before that copy and make the engine answer for a position the client
    // never asked about, with no diagnostic.
    if (refuse_while_searching("position")) {
        return;
    }
    if (line.find("startpos") != std::string_view::npos) {
        [[maybe_unused]] const bool ok = board_.SetupFromFEN(std::string(STARTING_FEN));
        assert(ok && "STARTING_FEN failed to parse");
    } else {
        auto fen_pos = line.find("fen ");
        if (fen_pos != std::string_view::npos) {
            auto fen_start = fen_pos + 4;
            auto moves_pos = line.find(" moves", fen_start);
            std::string fen = (moves_pos != std::string_view::npos)
                ? std::string(line.substr(fen_start, moves_pos - fen_start))
                : std::string(line.substr(fen_start));

            // A rejected FEN resets to the starting position and says so. Both of
            // those matter:
            //
            // Leaving the previous position in place makes the engine answer for
            // a position the caller never sent, with the answer depending on what
            // was loaded before -- so the same command yields different results in
            // different sessions. That is how a whole perftcheck corpus run got
            // misread as move-generation faults (#200).
            //
            // `info string` is the error channel UCI actually has. The move list
            // below is deliberately not replayed: it describes a position that was
            // never established.
            if (!board_.SetupFromFEN(fen)) {
                send("info string position: rejected FEN, board reset to the starting position");
                [[maybe_unused]] const bool ok = board_.SetupFromFEN(std::string(STARTING_FEN));
                assert(ok && "STARTING_FEN failed to parse");
                return;
            }
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
            if (m.is_null()) {
                // Replay stops here rather than skipping the move: every later
                // move is relative to the position this one would have produced,
                // so applying them would build a position nobody described. What
                // was applied so far stands, and the caller is told where it
                // stopped.
                send("info string position: unparseable move '" + token +
                     "', remaining moves not replayed");
                return;
            }
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

void UciHandler::cmd_go(std::string_view line)
{
    stop_and_join();

    // Same guard, and the same reason, as cmd_ucinewgame(): run() constructs ai_ before the command
    // loop starts, so this is normally already true. It can still be null when dispatch() is driven
    // directly, as the unit tests do -- and there the search thread below would dereference it,
    // which crashes the test binary with no diagnostic rather than failing an assertion.
    if (!ai_) init_ai();

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
    // Raised on this thread, before the search exists, so a command arriving
    // immediately after 'go' cannot observe a stale false.
    searching_.store(true, std::memory_order_release);
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
        // Cleared BEFORE bestmove goes out, not after. `bestmove` is the only
        // thing a client waits for, so it will send the next `position` the
        // instant it reads that line -- and refuse_while_searching() silently
        // refuses `position` while this flag is set, leaving board_ on the
        // previous position. `go` is not refused, so the next search then runs
        // on a stale board and returns a move that is illegal in the real one:
        // typically the engine's own previous move, from a square it has already
        // vacated. That forfeited two games in the 19,980-game run 31281221815
        // (issue #245).
        //
        // Ordering it this way makes the window unreachable rather than narrow:
        // by the time the client can possibly observe bestmove, the engine is
        // already accepting commands. GetMove() has returned by here, so nothing
        // below touches board_ and clearing early races with no search work.
        searching_.store(false, std::memory_order_release);
        send("bestmove " + bm);
    });
}

// UCI has no error channel, so the refusal goes out as 'info string' -- the
// convention every GUI records in its engine log. Stdout only, deliberately:
// stdout IS the protocol channel here, so a stderr copy would just duplicate
// the line for anyone merging the two streams, and land it out of order
// because stderr is unbuffered.
//
// The command is refused rather than honoured: abandoning a running search on a
// protocol violation would discard work the client did ask for, and a
// conforming GUI never sends these mid-search anyway.
bool UciHandler::refuse_while_searching(std::string_view command)
{
    if (!searching_.load(std::memory_order_acquire)) {
        return false;
    }
    send("info string " + std::string(command) +
         ": ignored, a search is in progress -- send 'stop' first");
    return true;
}

// "perft <depth>" and "go perft <depth>": node counts per root move for the
// current position, in the divide format every UCI perft harness parses --
// "e2e4: 600" per move, then the total.
//
// stop_and_join() first: Perft::divide walks the tree with DoMove/UndoMove on
// board_, which a running search is reading.
void UciHandler::cmd_perft(std::string_view line)
{
    stop_and_join();

    // Depth is the last whitespace-separated token; anything unparseable, out of
    // range, or missing is ignored in keeping with the UCI convention for
    // malformed input.
    std::istringstream iss{std::string(line)};
    std::string token;
    int depth = -1;
    while (iss >> token) {
        if (token == "perft" || token == "go") continue;
        try {
            depth = std::stoi(token);
        } catch (const std::exception&) {
            return;
        }
    }

    if (depth < 0 || depth > PERFT_MAX_DEPTH) return;

    Testing::Perft::divide(board_, depth);
    std::cout.flush();
}

void UciHandler::cmd_stop()
{
    stop_and_join();
}

void UciHandler::cmd_setoption(std::string_view line)
{
    // Both supported options mutate a live AI, so they must not race a search.
    if (refuse_while_searching("setoption")) {
        return;
    }
    // Minimal UCI 'setoption' parser — recognizes exactly:
    //   setoption name Threads value N
    //   setoption name Hash value N
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

    if ((name != "Threads" && name != "Hash") ||
        value_pos == std::string_view::npos) return;

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

    if (name == "Threads") {
        configured_threads_ = n;
        if (ai_) ai_->SetThreads(n);
        return;
    }

    // run() constructs ai_ before dispatching commands, and ai_ persists across
    // ucinewgame. Hash therefore needs no configured shadow; a null ai_ exists
    // only in direct test-only handler calls.
    if (!ai_) return;

    // searching_ can clear before bestmove, but only after GetMove() has joined
    // helper threads and returned, so an accepted replacement has no concurrent
    // transposition-table reader.
    const auto result = ai_->SetHash(n);
    if (!result.success) {
        send("info string hash " + std::to_string(result.requested_mb) +
             " MiB allocation failed; previous table retained");
        return;
    }

    send("info string hash " + std::to_string(result.entry_mb) +
         " MiB (" + std::to_string(result.bucket_count) + " buckets)");
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void UciHandler::stop_and_join()
{
    if (ai_) ai_->StopSearch();
    if (search_thread_.joinable()) search_thread_.join();
    // Belt and braces: the search clears this itself, but a thread that ended
    // without reaching that point must not leave the engine refusing commands
    // for the rest of the session.
    searching_.store(false, std::memory_order_release);
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

bool UciHandler::dispatch(std::string_view line)
{
    // Logged BEFORE the command runs. The case this exists for is a command that hangs or crashes
    // the engine, and a line written afterwards is never written at all. The sink flushes per line
    // for the same reason. Never stdout — see Logger::CreateUciCommandLogger.
    if (command_log_) command_log_->debug(">> {}", line);

    if (line == "uci")                        { cmd_uci(); }
    else if (line == "isready")               { cmd_isready(); }
    else if (line == "ucinewgame")            { cmd_ucinewgame(); }
    else if (line == "eval")                  { cmd_eval(); }
    else if (line.starts_with("position"))    { cmd_position(line); }
    // Both spellings, and both before the bare 'go': "go perft" would
    // otherwise be parsed as a search whose unknown tokens are skipped.
    else if (line.starts_with("go perft"))    { cmd_perft(line); }
    else if (line.starts_with("perft"))       { cmd_perft(line); }
    else if (line.starts_with("go"))          { cmd_go(line); }
    else if (line.starts_with("setoption"))   { cmd_setoption(line); }
    else if (line == "stop")                  { cmd_stop(); }
    else if (line == "quit")                  { cmd_stop(); return false; }
    // unknown commands: ignore silently (UCI spec)
    return true;
}

void UciHandler::run()
{
    init_ai();
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!dispatch(line)) break;
    }
}
