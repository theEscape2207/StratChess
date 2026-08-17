// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "AIPerplex.h"
#include "MoveGenerator.h"
#include "PVIntegrity.h"
#include "Sort.h"
#include "Utils/Logger.h"
#include "defines.h"
#include "MoveHelper.h"
#include "MoveFormatter.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <new>
#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

// Lazy SMP thread-safety: s_logger's sinks
// (stdout_color_sink_mt, basic_file_sink_mt — see ensure_logger_initialized()
// below, and the equivalent _mt sinks in Utils/Logger.cpp's default/perf
// loggers) are all spdlog "_mt" thread-safe variants, so concurrent log
// calls from multiple threads would be safe at the sink level. That said,
// s_logger itself and ensure_logger_initialized()'s lazy-init (a plain
// `if (s_logger) return;` check, not a magic static) are NOT safe to race:
// initialization only ever happens from SetVerboseLogging(), invoked once
// during single-threaded AIPerplex setup before any search (or future
// helper thread) starts. Helper threads run the same search_with_aspiration()
// code path as the main thread and reach the same log_* call sites, but
// those calls are gated on `td.thread_id == 0` (see search_with_aspiration()
// below), so only the main thread ever actually logs; s_logger remains
// read-only (already-initialized-or-null) from every thread's perspective.
// If a future revision lets helper threads log too, ensure_logger_initialized()
// must be made safe to call concurrently (e.g. via std::call_once) first.
static std::shared_ptr<spdlog::logger> s_logger = nullptr;
static void ensure_logger_initialized()
{
	// ensure the general default logger is initialized first (no-op if already)
	Engine::Logger::InitDefault();
	if (s_logger)
		return;

	// create AIPerplex specific logger if desired
	// Use spdlog directly via Engine::Logger utilities
	if (!Engine::Logger::GetLogger("AIPerplex")) {
		try {
			// create an async logger specifically for AIPerplex diagnostics - small thread pool (queue size 8192, 1 backing thread)
			//spdlog::init_thread_pool(8192, 1);
			//auto tp = spdlog::thread_pool();

			// add both console and file sinks (file sink keeps a record for diagnostics)
			auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
			console_sink->set_level(spdlog::level::info);
			console_sink->set_pattern(("%T.%e %^%l%$: %v"));
			auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/aiperplex.log", true);
			file_sink->set_level(spdlog::level::debug);
			file_sink->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

			s_logger = std::make_shared<spdlog::logger>( // was spdlog:async_logger
			    "AIPerplex", spdlog::sinks_init_list{console_sink, file_sink});
			//tp,
			//spdlog::async_overflow_policy::block);

			spdlog::register_logger(s_logger);
			s_logger->set_level(spdlog::level::debug);
			s_logger->flush_on(spdlog::level::debug);
		} catch (...) { // NOLINT(bugprone-empty-catch) - best-effort logger init
		}
	}
}

void AIPerplex::SetVerboseLogging(bool enabled) noexcept
{
	s_verbose_logging = enabled;
	try {
		if (enabled) {
			ensure_logger_initialized();
			if (s_logger)
				s_logger->set_level(spdlog::level::debug);
		} else if (s_logger) {
			s_logger->set_level(spdlog::level::off);
		}
	} catch (...) { // NOLINT(bugprone-empty-catch) - best-effort logging toggle
	}
}

AIPerplex::AIPerplex(Board& board, unsigned md) : PlayerAiBase(board, md)
{
	_tt = std::make_unique<TranspositionTable>(DEFAULT_HASH_MB);

	// Seed the thread-local board so td_ is valid from birth (init_search()
	// re-copies before every search). Board-reading helpers must work without
	// a prior GetMove() call — the [search] unit tests rely on that.
	td_.board = m_Board;

	// td_'s constructor clears killers and history.
	// Verbose logging is opt-in per call site:
	//   game mode  → Game::SetPlayerParams() calls SetVerboseLogging(true)
	//   UCI mode   → UciHandler::init_ai()   calls SetVerboseLogging(false)
	//   test mode  → test setup              calls SetVerboseLogging(false)
	// Do NOT enable it here — constructors must not have stdout side-effects.
}

// Callers must ensure no search is using _tt. Constructing the replacement
// before assigning it retains the old table if allocation fails.
PlayerAiBase::HashConfigurationResult AIPerplex::SetHash(unsigned mb) noexcept
{
	const unsigned requested = std::clamp(mb, MIN_HASH_MB, MAX_HASH_MB);
	try {
		auto replacement = std::make_unique<TranspositionTable>(requested);
		const HashConfigurationResult result{true, requested, replacement->memory_mb(), replacement->bucket_count()};
		_tt = std::move(replacement);
		return result;
	} catch (const std::bad_alloc&) {
		return {false, requested, 0, 0};
	}
}

// Resets every piece of per-game state so that a persisting AIPerplex is
// equivalent to a freshly-constructed one, without paying for a rebuild.
// What is deliberately NOT reset here:
//   - threads_    : never discarded by anything now that ai_ persists across
//                   games, so there is nothing to restore.
//   - tuning_     : caller configuration, not accumulated search state --
//                   the same reasoning as threads_. Game::SetPlayerParams()
//                   applies game_settings.json's search_tuning overrides and
//                   then unconditionally calls StartNewGame() on the same
//                   object; resetting tuning_ here would silently discard
//                   those overrides before the first move is searched.
//   - the evaluator: EvalManager/EvalComplex are documented stateless and
//                    thread-shared (see the Lazy SMP sharing contract
//                    comment in Eval.h) -- recreating one changes nothing.
//   - max_depth_ / time_limit_ (PlayerAiBase): unconditionally reset by
//     UciHandler::stop_and_join(), called immediately before this in
//     cmd_ucinewgame().
void AIPerplex::StartNewGame()
{
	(void)_tt->clear();
	td_.reset_for_new_game();
	// Lazily resized in GetMove() (`if (helper_tds_.size() < threads - 1)`),
	// so clearing it forces fresh ThreadData construction -- with the
	// correct thread_id reassigned -- on the next search, rather than
	// reusing helpers whose killers/history carry over from the last game.
	helper_tds_.clear();
	last_result_ = SearchResult{};
}

// PVS Iterative transpositional alpha beta search
// Transposition tables
// Resets td_'s per-search state. Move-ordering state (killers, history) is
// handled inside iterative_deepening(); history deliberately survives across
// moves (aged, never cleared).
void AIPerplex::init_search(const GameInfo& info)
{
	td_.board = m_Board; // thread-local copy — the search runs on this
	td_.nodes_searched = 0;
	td_.qnodes_searched = 0;
	td_.pv_table = PVTable{}; // fresh PV, exactly like the former GetMove() local
	td_.info_seq.clear();
	td_.info_seq.emplace_back(info);
}

// Lazy SMP helper thread entry point (plain iterative deepening, no quality
// gates — see the declaration comment in AIPerplex.h). Runs entirely on its
// own ThreadData; touches nothing shared except the TT (already thread-safe)
// and the atomic abort flag it reads via IsAborted(). Calls the same
// search_with_aspiration() aspiration-window logging as the main thread, but
// those log_* calls are gated on `td.thread_id == 0` inside
// search_with_aspiration(), so helper threads never actually log. Never
// reports a move — its result is discarded by design; only the main
// thread's search result is authoritative (the "main-is-authoritative"
// design decision, .claude/plans/lazy-smp.md).
void AIPerplex::helper_loop(ThreadData& td, int max_depth, TranspositionTable& tt)
{
	int seed_score = 0;
	for (int depth = 1; depth <= max_depth; ++depth) {
		if (IsAborted())
			break;
		const int score = search_with_aspiration(td, depth, seed_score, tt);
		if (IsAborted())
			break; // partial iteration — discard score
		seed_score = score;
	}
}

Move AIPerplex::GetMove(GameInfo& info, const SearchLimits& limits)
{
	init_search(info);
	// Snapshot threads_ exactly once: UCI's cmd_setoption (unlike cmd_go/
	// cmd_ucinewgame/cmd_stop) does not call stop_and_join() before writing
	// threads_, so a client can mutate it on the UCI thread while this
	// function runs on the search thread. Re-reading the plain `unsigned`
	// member at multiple points below could observe different values across
	// reads (e.g. the spawn guard sees the old value while the aggregation
	// loop sees a new, larger one after helper_tds_ was never resized this
	// call), which is both a data race and a potential out-of-bounds
	// helper_tds_[] access. Using one local snapshot everywhere in this
	// function closes that window; a setoption arriving mid-search simply
	// takes effect starting with the next GetMove() call.
	const unsigned threads = threads_;
	const unsigned effective_depth = ApplyLimits(limits);

	// Lazy SMP: spawn threads_ - 1 helper threads to warm the shared TT while
	// the main search below runs on td_ (main-is-authoritative, see
	// .claude/plans/lazy-smp.md: helpers never report a move, only
	// their node counts feed back in).
	// threads_ == 1 (the default) leaves this block entirely unreached:
	// `helpers` stays a default-constructed empty vector and helper_tds_ is
	// never touched — byte-identical to the pre-SMP single-threaded code path.
	std::vector<std::jthread> helpers;
	if (threads > 1) {
		if (helper_tds_.size() < threads - 1) {
			const size_t old = helper_tds_.size();
			helper_tds_.resize(threads - 1);
			for (size_t i = old; i < helper_tds_.size(); ++i) {
				helper_tds_[i] = std::make_unique<ThreadData>();
				helper_tds_[i]->thread_id = static_cast<int>(i) + 1;
			}
		}
		helpers.reserve(threads - 1);
		for (size_t i = 0; i < threads - 1; ++i) {
			ThreadData& htd = *helper_tds_[i];
			htd.board = m_Board; // same seed as td_.board
			htd.info_seq.clear();
			htd.info_seq.emplace_back(info); // same root info as init_search gives td_
			htd.clear_killers();
			htd.clear_null_move_flags();
			htd.nodes_searched = 0;
			htd.qnodes_searched = 0;
			helpers.emplace_back([this, &htd, effective_depth, this_tt = _tt.get()] {
				helper_loop(htd, static_cast<int>(effective_depth), *this_tt);
			});
		}
	}

	SearchResult result = iterative_deepening(td_, static_cast<int>(effective_depth), *_tt);
	last_result_ = result; // expose via GetLastResult()

	// Propagate the searched game state (mate/stalemate detected at the root)
	// back to the real game board. This is the only m_Board side effect the
	// search had before it ran on the thread-local copy; same only-if-changed
	// condition as ThreadData::update_game_state().
	const GameStates searched_state = td_.info_seq.at(0).gameState;
	if (searched_state != m_Board.GetGameInfo().gameState)
		m_Board.SetGameState(searched_state);

	// Latch the abort signal so any still-running helpers collapse in O(depth)
	// steps (they only ever poll IsAborted()), then join them — helpers.clear()
	// destroys each std::jthread, which joins automatically. This must happen
	// before every subsequent return in this function, including the
	// empty-move emergency path below, so a helper can never outlive GetMove().
	time_manager_.stop();
	helpers.clear();

	int64_t total_nodes = td_.nodes_searched;
	int64_t total_qnodes = td_.qnodes_searched;
	for (size_t i = 0; i + 1 < static_cast<size_t>(threads); ++i) {
		total_nodes += helper_tds_[i]->nodes_searched;
		total_qnodes += helper_tds_[i]->qnodes_searched;
	}
	last_result_.nodes_searched = total_nodes;
	last_result_.qnodes_searched = total_qnodes;

	// Both trees: an nps computed from the main tree alone charges quiescence work to
	// the clock without ever crediting it to the count.
	auto elapsed = StopTimerAndAdjustVars(static_cast<size_t>(total_nodes + total_qnodes));

	Move bestMove = result.best_move;

	// Defensive check for game-over scenario
	if (bestMove.is_null()) {
		info = m_Board.GetGameInfo();
		//if ( !info.GameEnded())
		/*ensure_logger_initialized();
		if (s_logger) {
			s_logger->error("No move from search - game is over (score={})", result.best_score);
		}*/
		_bestScore = result.best_score;
		return Move::EmptyMove();
	}

	// Success logging
	if (s_logger) {
		s_logger->info("GetMove complete: move={}, score={}, depth={}, time={}ms, nodes={}, stable={}",
		               MoveFormatter::ToCoord(bestMove), result.best_score, result.depth_completed, elapsed.count(),
		               total_nodes, result.search_was_stable ? "yes" : "NO");
	}
	// Equivalent of CheckGameOver(info, false), reading the thread-local
	// info_seq root instead of the base-class m_infoSeq (unused by AIPerplex).
	info = td_.info_seq.at(0);
	if (info.gameState != GameStates::STILL_PLAYING) {
		EGameStateChanged.fire(this, info.gameState);
	}
	info.UpdateBoardInfo(bestMove, m_Board.GetEffectiveMovPiece(bestMove));

	return bestMove;
}

SearchResult AIPerplex::iterative_deepening(ThreadData& td, int max_depth, TranspositionTable& tt)
{
	SearchState state;
	td.nodes_since_check_ = 0;     // reset node counter for this search
	bool extra_depth_used = false; // soft-limit extension granted at most once per search

	td.clear_killers(); // Clear killer moves at the start of the search
	td.clear_null_move_flags();

	for (int depth = 1; depth <= max_depth; ++depth) {

		// BEFORE ITERATION: Prepare for this depth's search
		tt.newSearchIteration();
		td.age_history();
		const int64_t nodes_at_start = td.nodes_searched;

		// EXECUTE SEARCH: This might get interrupted by timeout
		int currentBestScore;
		if (state.depth_completed == 0 || !tuning_.aspiration_enabled) {
			// Depth 1 or kill-switch: always full window (no reliable seed yet)
			currentBestScore = pvs(td, depth, -GameValues::Search_Init, GameValues::Search_Init, 0, true, tt);
		} else {
			currentBestScore = search_with_aspiration(td, depth, state.best_score, tt);
		}

		// Gather metrics
		IterationMetrics metrics;
		metrics.depth = depth;
		metrics.current_move = td.pv_table.get_pv_move(0);
		metrics.current_score = currentBestScore;
		metrics.nodes_searched = td.nodes_searched - nodes_at_start;
		metrics.pv_length = td.pv_table.get_length(0);
		metrics.interrupted = StopRequested(); // clock, node budget or UCI stop
		metrics.move_changed = (metrics.current_move != state.last_iteration_move);
		metrics.score_delta = currentBestScore - state.best_score;
		// node counts never realistically approach 2^53 (int64_t->double precision loss)
		metrics.completion_ratio =
		    (state.nodes_at_completed_depth > 0)
		        ? static_cast<double>(metrics.nodes_searched) / static_cast<double>(state.nodes_at_completed_depth)
		        : 1.0;

		// Debug logging (detailed diagnostics)
		log_iteration_eval(metrics, td.pv_table);

		// Decide what to do
		IterationDecision decision;
		RejectionReason rejection_reason = RejectionReason::NONE;

		if (!metrics.interrupted) {
			decision = IterationDecision::ACCEPT_AND_CONTINUE; // Depth completed
		} else {
			// Interrupted - assess quality
			rejection_reason = assess_iteration_quality(metrics, state);
			decision = (rejection_reason != RejectionReason::NONE) ? IterationDecision::REJECT_AND_STOP
			                                                       : IterationDecision::ACCEPT_AND_STOP;
		}

		// Execute decision
		bool continue_iteration = false;

		switch (decision) {
		case IterationDecision::ACCEPT_AND_CONTINUE:
			state.best_move = metrics.current_move;
			state.best_score = metrics.current_score;
			state.depth_completed = depth;
			state.nodes_at_completed_depth = metrics.nodes_searched;
			state.last_iteration_move = metrics.current_move;
			state.search_was_stable = !metrics.move_changed;

			log_completed_iteration(metrics, td.pv_table);
			emit_iteration_info(td, state.depth_completed, state.best_score);

			// Soft limit gate: stop after this depth if the allocated time budget
			// is consumed.  Exception: if the best move just changed, allow one
			// more depth to verify the new move (the hard limit will cut it off).
			if (time_manager_.should_stop_iteration()) {
				if (!metrics.move_changed || extra_depth_used) {
					continue_iteration = false;
					break;
				}
				extra_depth_used = true; // grant extension exactly once
			}

			continue_iteration = !should_stop_early(depth, metrics.current_score, metrics.pv_length);
			break;

		case IterationDecision::ACCEPT_AND_STOP:
			state.best_move = metrics.current_move;
			state.best_score = metrics.current_score;
			state.depth_completed = depth;
			state.nodes_at_completed_depth = metrics.nodes_searched;
			state.search_was_stable = !metrics.move_changed;

			log_acceptance(metrics);
			emit_iteration_info(td, state.depth_completed, state.best_score);
			continue_iteration = false;
			break;

		case IterationDecision::REJECT_AND_STOP:
			log_rejection(depth, rejection_reason, metrics, state);
			continue_iteration = false;
			break;
		}

		if (!continue_iteration) {
			break; // Exit iteration_deepening
		}
	}

	// Handle empty move emergency
	if (state.best_move.is_null()) {
		if (!handle_empty_move_emergency(td, state)) {
			// Game over - return what we have
			return SearchResult{.best_move = state.best_move,
			                    .best_score = state.best_score,
			                    .depth_completed = state.depth_completed,
			                    .nodes_searched = state.nodes_at_completed_depth,
			                    .search_was_stable = state.search_was_stable};
		}
	}

	// Final logging
	if (state.depth_completed > 0) {
		log_search_complete(state, td.pv_table);
	}
	return SearchResult{.best_move = state.best_move,
	                    .best_score = state.best_score,
	                    .depth_completed = state.depth_completed,
	                    .nodes_searched = state.nodes_at_completed_depth,
	                    .search_was_stable = state.search_was_stable};
}

bool AIPerplex::poll_search_limits(ThreadData& td)
{
	// Helpers never even reach the ++, so their nodes_since_check_ stays at 0 for the whole
	// search. They do still call StopRequested() elsewhere — search_with_aspiration() at
	// every retry boundary, and pvs()'s LMR re-search guard — so this gate bounds how often
	// the clock is read, not which threads read it.
	if (td.thread_id != 0 || (++td.nodes_since_check_ & 1023) != 0)
		return false;

	return StopRequested() || NodeLimitReached(td.nodes_searched + td.qnodes_searched);
}

int AIPerplex::pvs(ThreadData& td, int depth, int alpha, int beta, int ply, bool is_pv_node, TranspositionTable& tt)
{
	// Fast early exit: IsAborted() reads only the latched atomic (no clock call).
	// After the first StopRequested() fires and latches the flag, this collapses
	// the entire call stack in O(depth) steps instead of O(tree_size).
	if (IsAborted())
		return GameValues::Draw;

	if (poll_search_limits(td))
		return GameValues::Draw;

	td.pv_table.clear_ply(ply);

	// We need the info on the current board state
	GameInfo info = td.get_last_info(ply);

	// Test for 50 moves rule and threefold repetition
	if (td.check_draws(info, ply))
		return GameValues::Draw;

	// Er vi naaet til bunden af traeet - evaluering?
	//	See if static eval will cause a cutoff or raise alpha.
	if (depth <= 0)
		return quiescence(td, alpha, beta, QSEARCH_BUDGET, ply, tt);

	auto key = td.board.get_zobrist_hash();
	int original_alpha = alpha;
	Move hash_move;
	Move pv_move;

	// TT probe
	if (auto entry = tt.probe(key, ply)) {
		if (entry->phase == SearchPhase::MAIN) { // Avoid the Quiescence nodes to affect main search - just to make sure
			hash_move = entry->best_move;

			// Critical: Don't use TT cutoffs at PV nodes
			if (!is_pv_node && entry->depth >= depth) {
				if (entry->bound == BoundType::EXACT) {
					return entry->value;
				}
				if (entry->bound == BoundType::LOWER) {
					alpha = std::max(alpha, static_cast<int>(entry->value));
				}
				if (entry->bound == BoundType::UPPER) {
					beta = std::min(beta, static_cast<int>(entry->value));
				}
				if (alpha >= beta) {
					return entry->value;
				}
			}
		}
	}

	// Get PV move from previous iteration
	if (is_pv_node && ply > 0) {
		pv_move = td.pv_table.get_pv_move(ply);
	}

	const bool in_check = td.board.InCheck();

	// Declared before the null-move search so the unwind guard below it has the same value
	// to return as the guard in the move loop: the best score over the children that
	// actually completed, still the -Search_Init sentinel when none did.
	int best_value = -GameValues::Search_Init;

	// Null-move pruning: cheap cutoff attempt before move generation.
	// should_try_null_move() centralises every guard (zugzwang, mate-score,
	// consecutive-null, PV/in-check/depth) so it can be unit tested directly.
	if (should_try_null_move(td, depth, beta, ply, is_pv_node, in_check)) {
		const int R = tuning_.null_move_reduction;
		td.last_move_was_null[ply + 1] = true;
		td.board.DoNullMove();
		td.add_null_move_to_seq(ply);
		int null_score = -pvs(td, depth - 1 - R, -beta, -beta + 1, ply + 1, false, tt);
		td.board.UndoNullMove();
		td.last_move_was_null[ply + 1] = false;

		// Unwind invariant, see the move loop below: null_score comes from a search that was
		// cut off, so the store it would justify is not ours to make.
		if (IsAborted())
			return best_value;

		if (null_score >= beta) {
			tt.store(key, static_cast<int16_t>(null_score), static_cast<int16_t>(depth), static_cast<int16_t>(ply),
			         Move::EmptyMove(), BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::MAIN);
			return null_score;
		}
	}

	MoveList moveList;
	MoveGenerator::ComputeLegalMoves(td.board, info, moveList);

	bool first_child = true;
	Move best_move;

	// Stack-allocated scored index array — zero heap allocation per call.
	std::array<std::pair<int, int>, MoveList::MAX_MOVES> scored_idx;
	const int n = static_cast<int>(moveList.size());
	const eColor side = td.board.GetCurrentColor();

	MoveSorter::ScoreMoves(moveList, n, td.board, side, pv_move, hash_move, td.killers[ply][0], td.killers[ply][1],
	                       td.history, scored_idx);

	bool moveFound = false;

	// Iterate by sorted index — no rebuild of moveList needed
	for (int si = 0; si < n; ++si) {
		const Move& move = moveList[scored_idx[si].second];

		td.nodes_searched++;

		if (td.board.DoMove(move)) {
			// Tilfoejer dette traek til nuvaerende traekfoelge - og opdaterer resten
			td.add_move_to_seq(move, ply);
			int value;

			if (first_child) {
				// Full window search for first move
				value = -pvs(td, depth - 1, -beta, -alpha, ply + 1, is_pv_node, tt);
				first_child = false;
			} else {
				const bool isCapture = MoveHelper::IsCapture(move);
				const bool isPromotion = MoveHelper::IsPromote(move);
				const bool isKiller = (move == td.killers[ply][0] || move == td.killers[ply][1]);

				// Late Move Reductions: reduce quiet, non-killer, non-evasion moves
				// that appear late in the sorted order. Skip conditions are conservative:
				// captures, promotions, killers, evasions (in_check), PV nodes, checking
				// moves, and early moves are always searched at full depth.
				// Future skip candidates: passed pawn pushes.
				//
				// The board still holds the position after DoMove, so InCheck() here asks
				// whether the opponent is in check, i.e. whether this move gives check. It
				// is last in the chain deliberately: InCheck() generates a whole-side attack
				// board, and every earlier term disqualifies far more moves than it admits.
				const bool applyLMR = tuning_.lmr_enabled && !is_pv_node && !in_check && !isCapture && !isPromotion &&
				                      !isKiller && si >= tuning_.lmr_min_move_index && depth >= tuning_.lmr_min_depth &&
				                      !td.board.InCheck();

				if (applyLMR) {
					// sqrt formula: scales naturally with depth and move index.
					// Clamped to [1, depth-1]; when R == depth-1 the recursive call is
					// at depth 0 (falls into quiescence). The re-search below restores
					// full depth if alpha is beaten.
					// clang-format off
					// Hand-wrapped so the sqrt(depth) * sqrt(move index) product and the
					// clamp to [1, depth-1] stay visible as separate steps.
					const int R = std::min(
						std::max(1, static_cast<int>(
							std::sqrt(static_cast<double>(depth - 1)) *
							std::sqrt(static_cast<double>(si - 1)))),
						depth - 1);
					// clang-format on

					// Reduced-depth null-window search
					value = -pvs(td, depth - 1 - R, -alpha - 1, -alpha, ply + 1, false, tt);

					// Re-search at full depth-1 null window if the reduced result beats alpha
					if (value > alpha && !StopRequested())
						value = -pvs(td, depth - 1, -alpha - 1, -alpha, ply + 1, false, tt);
				} else {
					// Normal null-window search (unchanged)
					value = -pvs(td, depth - 1, -alpha - 1, -alpha, ply + 1, false, tt);
				}

				// Re-search with full window at PV node (unchanged from original)
				if (value > alpha && is_pv_node)
					value = -pvs(td, depth - 1, -beta, -alpha, ply + 1, true, tt);
			}

			td.board.UndoMove(move);

			// Unwind invariant: an aborted frame mutates nothing. `value` came from a search
			// that was cut off mid-tree, so everything below — the transposition store, the
			// PV row, the killer and history updates — would record a result no search ever
			// produced, and those writes outlive the search that made them. Returning here,
			// with the board already restored, is what keeps the whole unwinding stack out
			// of them: every parent frame reaches its own copy of this guard immediately
			// after its recursive call returns. best_value is the best score over the
			// children that did complete, which is a valid lower bound for this node and is
			// what the root reports for an interrupted iteration.
			if (IsAborted())
				return best_value;

			moveFound = true;

			if (value > best_value) {
				best_value = value;
				best_move = move;

				if (value > alpha) {
					alpha = value;

					// Update PV when alpha improves
					if (is_pv_node) {
						td.pv_table.update(ply, move);
					}
					// Update history for non-capture moves
					//if (!move.is_capture()) {
					//	data.move_ordering.update_history(move, depth, false);
				}
			}

			if (beta <= alpha) {
				td.store_killer(ply, move);
				td.update_history(side, move, depth);
				break;
			}
		}
	}

	// Terminal node: no legal move could be played, so this position is checkmate or
	// stalemate and best_value is still the -Search_Init sentinel. Resolve the true
	// score before storing — narrowing the sentinel to the TT's int16_t value field
	// would wrap it to +15536 and cache a nonsensical bound for the position.
	// The score is exact and depth-independent, so it is stored as EXACT with an
	// empty move; tt.store() normalises the mate distance by ply.
	//
	// Exempt from the unwind invariant above: !moveFound means no child search ran at all,
	// so this value derives from InCheck() and ply alone and is as true after an abort as
	// before one. The guard in the loop cannot have been passed on the way here.
	if (!moveFound) {
		const int terminal_value = adjustScoreForGameState(td, moveFound, ply, best_value);
		tt.store(key, static_cast<int16_t>(terminal_value), static_cast<int16_t>(depth), static_cast<int16_t>(ply),
		         Move::EmptyMove(), BoundType::EXACT, is_pv_node ? NodeType::PV_NODE : NodeType::ALL_NODE,
		         SearchPhase::MAIN);
		return terminal_value;
	}

	// Classify node and store
	BoundType bound;
	NodeType node_type;

	if (best_value <= original_alpha) {
		bound = BoundType::UPPER;
		node_type = NodeType::ALL_NODE;
	} else if (best_value >= beta) {
		bound = BoundType::LOWER;
		node_type = NodeType::CUT_NODE;
	} else {
		bound = BoundType::EXACT;
		node_type = is_pv_node ? NodeType::PV_NODE : NodeType::ALL_NODE;
	}

	tt.store(key, static_cast<int16_t>(best_value), static_cast<int16_t>(depth), static_cast<int16_t>(ply), best_move,
	         bound, node_type, SearchPhase::MAIN);

	return adjustScoreForGameState(td, moveFound, ply, best_value);
}

int AIPerplex::adjustScoreForGameState(ThreadData& td, bool moveFound, int ply, int score)
{
	// Any legal moves found?
	if (!moveFound) {
		// Nope - so we are either mate or remis here !
		if (td.board.InCheck()) {
			// Oops - we are mate!!
			td.update_game_state(ply,
			                     td.board.GetCurrentColor() == WHITE ? GameStates::BLACK_WON : GameStates::WHITE_WON);
			if (ply == 0) // Are we at root?
			{
				_bestScore = -GameValues::Mate;
			}
			// Checkmate - prefer shorter mates
			return -GameValues::Mate + ply;
		}
		// Else No move and not in check - Pat!
		td.update_game_state(ply, GameStates::DRAW_PAT);
		return GameValues::Draw;
	}

	td.update_game_state(ply, GameStates::STILL_PLAYING);

	return score;
}

int AIPerplex::quiescence(ThreadData& td, int alpha, int beta, int qsearch_budget, int ply, TranspositionTable& tt)
{
	// Fast early exit: IsAborted() reads only the latched atomic (no clock call).
	// Mirrors pvs() — collapses quiescence chains in O(depth) after latch fires.
	if (IsAborted())
		return GameValues::Draw;

	if (poll_search_limits(td))
		return GameValues::Draw;

	// A side to move in check may not decline to move, so this node cannot be settled by a
	// static evaluation: no stand-pat, and every legal evasion must be searched, not just
	// captures. Computed before the depth cutoffs because it decides which one applies.
	const bool in_check = td.board.InCheck();

	// Limit quiescence extension by qsearch. In check the budget is deliberately ignored —
	// returning an evaluation of a position whose side to move is still in check is the
	// defect this path exists to remove. What bounds the chain that bypasses the budget is
	// the draw and backstop checks below, not material: a quiet evasion may itself give
	// check, so two sides can go on checking each other without a capture between them.
	if (qsearch_budget < 0 && !in_check) {
		return Eval->Evaluate(td.board);
	}

	// Repetition and fifty-move draws. pvs() checks these before it hands a node to
	// quiescence, so historically quiescence could reach neither: every move it could make
	// was a capture or a pawn move, which resets the fifty-move counter and makes repetition
	// impossible. Generating quiet evasions breaks both halves of that.
	//
	// Repetition becomes reachable because a quiet evasion may itself give check, so two
	// sides can go on checking each other with no capture between them — material never
	// falls and the position is free to repeat. Left undetected, such a line runs to the
	// absolute backstop below and is settled by a static evaluation of a position that is
	// still in check, which is the defect this whole path exists to remove.
	//
	// The fifty-move counter becomes reachable the same way but is observed one ply later:
	// a quiet evasion can push the count to the limit, and the node that results need not
	// itself be in check. So this is deliberately not gated on in_check — gating it would
	// leave a genuine fifty-move draw scored by material, and would trip the STILL_PLAYING
	// assertion in GameState::UpdateFiftyMovesState on the next move made from that node.
	if (td.check_draws(td.get_last_info(ply), ply))
		return GameValues::Draw;

	// Absolute backstop. With the draw check above, a real search should never reach this —
	// but the recursion must terminate on its own rather than on an argument about what
	// positions can arise, and ply indexes fixed-size per-thread arrays elsewhere in the
	// search. This is the one place a position is evaluated while still in check.
	if (ply >= MAX_PLY - 1) {
		return Eval->Evaluate(td.board);
	}

	int original_alpha = alpha;

	auto key = td.board.get_zobrist_hash();

	// Probe TT for cached info. Both sides of the comparison are remaining budget, so an entry
	// is reusable exactly when it was produced with at least as much search left as this node
	// has — the same invariant the main search relies on.
	if (auto entry = tt.probe(key, ply)) {
		if (entry->phase == SearchPhase::QUIESCENCE && entry->depth >= qsearch_budget) {
			if (entry->bound == BoundType::EXACT) {
				return entry->value;
			}
			if (entry->bound == BoundType::LOWER) {
				alpha = std::max(alpha, static_cast<int>(entry->value));
			}
			if (entry->bound == BoundType::UPPER) {
				beta = std::min(beta, static_cast<int>(entry->value));
			}
			if (alpha >= beta) {
				return entry->value;
			}
		}
	}

	// We need the info on the current board state
	const GameInfo& info = td.get_last_info(ply);

	// Stand-pat: the option of making no move at all. Out of check it is the node's baseline
	// and can cut off on its own. In check it does not exist — the side to move is obliged to
	// leave check — so neither the cutoff nor the baseline applies, and the static evaluation
	// backing them is not computed at all (delta pruning, its only other consumer, is likewise
	// disabled below).
	//
	// The cutoff store below is exempt from the unwind invariant: it is reached before this
	// node searches anything, so its value is a static evaluation of the position in front of
	// it and owes nothing to a child. Whether the search is about to be aborted does not make
	// a static evaluation any less true.
	int best_value = -GameValues::Search_Init;
	int stand_pat = 0;

	if (!in_check) {
		stand_pat = Eval->Evaluate(td.board);
		if (stand_pat >= beta) {
			// Store and cutoff
			tt.store(key, static_cast<int16_t>(beta), static_cast<int16_t>(qsearch_budget), static_cast<int16_t>(ply),
			         Move::EmptyMove(), BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::QUIESCENCE);
			return beta;
		}
		// stand-pat is the baseline (valid option: don't capture)
		best_value = stand_pat;
		if (stand_pat > alpha)
			alpha = stand_pat;
	}

	MoveList moveList;
	if (in_check) {
		// Every evasion counts, including quiet blocks and king walks, so a capture-only
		// generator cannot answer this node. The list is pseudo-legal; DoMove() below rejects
		// the moves that leave the king in check, which is what makes an empty survivor set
		// mean checkmate.
		MoveGenerator::ComputeLegalMoves(td.board, info, moveList);
	} else {
		// Generate only capture moves and promotions
		MoveGenerator::ComputeCaptures(td.board, info, moveList);
	}
	// Sort the found captures
	MoveSorter::SortMovesByValue(moveList, moveList.size(), td.board);

	// Tjek om der er lovlige brugbare traek her
	bool moveFound = false;
	Move best_move = Move::EmptyMove();

	for (const auto& move : moveList) {
		// Promotions stay: their tactical value is not bounded by immediate material gain.
		if (!in_check && !MoveHelper::IsPromote(move) &&
		    stand_pat +
		            MoveHelper::DeltaGain(move, td.board.GetEffectiveMovPiece(move), td.board.GetCapturedPiece(move)) +
		            tuning_.delta_pruning_margin <
		        alpha)
			continue;

		if (!td.board.DoMove(move))
			continue;

		td.add_move_to_seq(move, ply);

		// Per searched edge, matching pvs(): counting entries here instead would re-count
		// every quiescence root, whose incoming move the parent's loop already counted.
		td.qnodes_searched++;

		int score = -quiescence(td, -beta, -alpha, qsearch_budget - 1, ply + 1, tt);
		td.board.UndoMove(move);

		// Unwind invariant, as in pvs(): `score` came from a search that was cut off, so the
		// cutoff store below and the final store after the loop are both unearned. Returning
		// here is also what keeps the beta-cutoff store out of reach — it returns from inside
		// the loop, so a check placed after the loop would never run for it.
		if (IsAborted())
			return best_value;

		moveFound = true;

		if (score >= beta) {
			tt.store(key, static_cast<int16_t>(beta), static_cast<int16_t>(qsearch_budget), static_cast<int16_t>(ply),
			         move, BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::QUIESCENCE);
			return beta;
		}
		if (score > best_value) {
			best_value = score;
			alpha = std::max(alpha, score);
			best_move = move;
		}
	}
	// In check, the move list was every legal evasion, so no survivor means checkmate — the
	// one terminal state quiescence can identify with certainty. Score it by ply so shorter
	// mates are preferred, and store it exact with an empty move: best_value is still the
	// -Search_Init sentinel here, and narrowing that sentinel into the table hands later
	// probes a score no position ever had.
	//
	// Exempt from the unwind invariant for the same reason as pvs()'s terminal store: no
	// child of this node was searched, so the score follows from InCheck() and ply alone.
	// An abort during the loop returns above rather than arriving here with !moveFound.
	if (in_check && !moveFound) {
		const int mate_value = -GameValues::Mate + ply;
		tt.store(key, static_cast<int16_t>(mate_value), static_cast<int16_t>(qsearch_budget), static_cast<int16_t>(ply),
		         Move::EmptyMove(), BoundType::EXACT, NodeType::PV_NODE, SearchPhase::QUIESCENCE);
		return mate_value;
	}

	// Out of check, no move found means no capture was available — not stalemate. Whether the
	// position could have been avoided is invisible from here, so the node keeps its stand-pat
	// evaluation rather than claiming a draw.

	// Classify node type correctly
	NodeType node_type;
	BoundType bound;

	if (!moveFound) {
		// No captures were available to search
		// This is ALL_NODE regardless of whether stand_pat improved alpha
		node_type = NodeType::ALL_NODE;
		bound = (best_value > original_alpha) ? BoundType::EXACT : BoundType::UPPER;
	} else if (best_value <= original_alpha) {
		// Failed to improve alpha (or came close)
		node_type = NodeType::ALL_NODE;
		bound = BoundType::UPPER;
	} else if (best_value >= beta) {
		// Beta cutoff (would have returned earlier, but for completeness)
		node_type = NodeType::CUT_NODE;
		bound = BoundType::LOWER;
	} else {
		// Captures found AND improved alpha, no beta cutoff => PV node
		node_type = NodeType::PV_NODE;
		bound = BoundType::EXACT;
	}

	tt.store(key, static_cast<int16_t>(best_value), static_cast<int16_t>(qsearch_budget), static_cast<int16_t>(ply),
	         best_move, bound, node_type, SearchPhase::QUIESCENCE);
	return best_value;
}

// ============================================================================
// ASPIRATION WINDOW SEARCH
// ============================================================================
int AIPerplex::search_with_aspiration(ThreadData& td, int depth, int seed_score, TranspositionTable& tt)
{
	int delta = tuning_.aspiration_initial_delta;
	int alpha = std::max(seed_score - delta, -GameValues::Search_Init);
	int beta = std::min(seed_score + delta, static_cast<int>(GameValues::Search_Init));
	int score = seed_score; // safe fallback if interrupted before the first pvs() call

	for (int retry = 0;; ++retry) {
		if (StopRequested()) {
			// Interrupt before entering pvs(): clear PV so iterative_deepening sees EmptyMove
			// and triggers INCOMPLETE rejection rather than accepting a stale PV as valid.
			td.pv_table.clear_ply(0);
			return score;
		}

		score = pvs(td, depth, alpha, beta, 0, true, tt);

		if (StopRequested())
			return score;

		// In-window: accept the score
		if (score > alpha && score < beta)
			return score;

		// Safety fallback: open full window after max retries
		if (retry >= tuning_.aspiration_max_retries) {
			if (td.thread_id == 0)
				log_aspiration_full_window(depth, tuning_.aspiration_max_retries);
			if (!StopRequested())
				score = pvs(td, depth, -GameValues::Search_Init, GameValues::Search_Init, 0, true, tt);
			return score;
		}

		// Widen on the failing side; double delta for the next potential miss.
		// Log after updating so the message shows the new window being tried.
		delta *= 2;
		if (score <= alpha) {
			alpha = std::max(seed_score - delta, -static_cast<int>(GameValues::Search_Init));
			if (td.thread_id == 0)
				log_aspiration_retry(depth, retry + 1, score, alpha, beta, true);
		} else {
			beta = std::min(seed_score + delta, static_cast<int>(GameValues::Search_Init));
			if (td.thread_id == 0)
				log_aspiration_retry(depth, retry + 1, score, alpha, beta, false);
		}
	}
}

// ============================================================================
// HELPERS
// ============================================================================
// Killer/history/null-flag maintenance lives on ThreadData (ThreadData.h).

AIPerplex::RejectionReason AIPerplex::assess_iteration_quality(const IterationMetrics& metrics,
                                                               const SearchState& state) const
{
	// CASE 1: Obviously incomplete
	if (metrics.current_move.is_null() || metrics.nodes_searched < tuning_.min_nodes_threshold) {
		return RejectionReason::INCOMPLETE;
	}

	// CASE 2: Too few nodes compared to previous depth
	if (state.depth_completed > 0 && state.nodes_at_completed_depth > 0 &&
	    metrics.completion_ratio < tuning_.min_completion_ratio) {
		return RejectionReason::TOO_FEW_NODES;
	}

	// CASE 3: PV too short
	if (metrics.pv_length < std::max(1, static_cast<int>(metrics.depth * tuning_.min_pv_ratio)) &&
	    state.depth_completed > 0) {
		return RejectionReason::SHORT_PV;
	}

	// CASE 4: Score dropped to 0 suspiciously
	if (metrics.current_score == 0 && state.depth_completed > 0 &&
	    std::abs(state.best_score) > tuning_.score_draw_threshold) {
		return RejectionReason::SCORE_DROP;
	}

	// CASE 5: Move changed on interrupt
	if (metrics.move_changed && state.depth_completed > 0) {
		return RejectionReason::MOVE_CHANGED;
	}

	// All checks passed
	return RejectionReason::NONE;
}

void AIPerplex::log_iteration_eval(const IterationMetrics& metrics, const PVTable& pv_table) const
{
	if (!s_logger)
		return;
	if (!s_logger->should_log(spdlog::level::debug))
		return;

	// Build PV string
	std::string pv_line;
	pv_line.reserve(80);
	const int display_length = std::min(metrics.pv_length, 6);
	for (int i = 0; i < display_length; ++i) {
		if (i)
			pv_line += ", ";
		pv_line += MoveFormatter::ToCoord(pv_table.get_line(0)[i]);
	}
	if (metrics.pv_length > display_length) {
		pv_line += " ...";
	}

	s_logger->debug("D{:>2} EVAL: move={:<8} score={:>6} (Δ{:>+5}) nodes={:>8} ({:>3}%) "
	                "pv={:>2} int={} chg={} PV:[{}]",
	                metrics.depth,
	                metrics.current_move.is_null() ? "EMPTY" : MoveFormatter::ToCoord(metrics.current_move),
	                metrics.current_score, metrics.score_delta, metrics.nodes_searched,
	                static_cast<int>(metrics.completion_ratio * 100), metrics.pv_length,
	                metrics.interrupted ? "Y" : "N", metrics.move_changed ? "Y" : "N", pv_line);
}

void AIPerplex::log_rejection(int depth, RejectionReason reason, const IterationMetrics& metrics,
                              const SearchState& state) const
{
	if (!s_logger)
		return;

	switch (reason) {
	case RejectionReason::INCOMPLETE:
		s_logger->debug("Depth {:>2}: REJECTED[R1:INCOMPLETE] (nodes={}, move={}) - Using depth {}", depth,
		                metrics.nodes_searched, metrics.current_move.is_null() ? "EMPTY" : "ok", state.depth_completed);
		break;

	case RejectionReason::TOO_FEW_NODES:
		s_logger->debug("Depth {:>2}: REJECTED[R2:TOO_FEW_NODES] ({} = {:.0f}% of D{}) - using depth {}", depth,
		                metrics.nodes_searched, metrics.completion_ratio * 100, state.depth_completed,
		                state.depth_completed);
		break;

	case RejectionReason::SHORT_PV:
		s_logger->debug("Depth {:>2}: REJECTED[R3:SHORT_PV] (pv={} vs depth={}) - using depth {}", depth,
		                metrics.pv_length, depth, state.depth_completed);
		break;

	case RejectionReason::SCORE_DROP:
		s_logger->debug("Depth {:>2}: REJECTED[R4:SCORE_DROP] ({} → 0) - Using depth {}", depth, state.best_score,
		                state.depth_completed);
		break;

	case RejectionReason::MOVE_CHANGED:
		s_logger->debug("Depth {:>2}: REJECTED[R5:MOVE_CHANGED] ({} → {}) - Using depth {}", depth,
		                MoveFormatter::ToCoord(state.last_iteration_move), MoveFormatter::ToCoord(metrics.current_move),
		                state.depth_completed);
		break;

	default:
		break;
	}
}

void AIPerplex::log_acceptance(const IterationMetrics& metrics) const
{
	if (!s_logger)
		return;

	// Noteworthy so Info
	s_logger->info("Depth {:>2}: ACCEPTED[INTERRUPTED] (nodes={}, score={}, pv={})", metrics.depth,
	               metrics.nodes_searched, metrics.current_score, metrics.pv_length);
}

bool AIPerplex::should_stop_early(int depth, int score, int pv_length) const
{
	// Mate found
	if (std::abs(score) >= GameValues::Mate_Threshold) {
		if (s_logger) {
			s_logger->info("Mate found at depth {}, stopping iteration", depth);
		}
		return true;
	}

	// Forced line (PV much shorter than depth)
	if (depth > 1 && pv_length > 0 && pv_length < (depth - depth / 2)) {
		if (s_logger) {
			s_logger->info("Short PV ({} vs depth {}) indicates forced line, stopping", pv_length, depth);
		}
		return true;
	}

	return false;
}

bool AIPerplex::should_try_null_move(const ThreadData& td, int depth, int beta, int ply, bool is_pv_node,
                                     bool in_check) const
{
	if (!tuning_.null_move_enabled)
		return false;
	if (is_pv_node || in_check)
		return false;
	if (depth < tuning_.null_move_min_depth)
		return false;
	if (std::abs(beta) >= GameValues::Mate_Threshold)
		return false;
	if (td.last_move_was_null[ply])
		return false;

	// Zugzwang guard: refuse to "pass" for a side with fewer than two
	// non-pawn pieces — the null-move assumption ("a free pass is never
	// better than moving") is false in king+pawn endgames AND in
	// single-piece endgames won by domination/zugzwang (issue #66: KQ vs KR,
	// where the lone rook loses only because its side must move).
	const eColor side = td.board.GetCurrentColor();
	const auto boards = td.board.GetBitBoards();
	const BITBOARD non_pawn_material =
	    boards[static_cast<BITBOARD>(KNIGHT) + side] | boards[static_cast<BITBOARD>(BISHOP) + side] |
	    boards[static_cast<BITBOARD>(ROOK) + side] | boards[static_cast<BITBOARD>(QUEEN) + side];
	return std::popcount(non_pawn_material) >= 2;
}

bool AIPerplex::handle_empty_move_emergency(ThreadData& td, SearchState& state)
{
	auto& log = *spdlog::default_logger();

	// Check if mate/stalemate
	if (std::abs(state.best_score) >= GameValues::Mate_Threshold) {
		log.info("No move needed - mate detected (score={})", state.best_score);
		return false;
	}

	// Check game state
	const GameInfo current_info = td.board.GetGameInfo();
	if (current_info.gameState != GameStates::STILL_PLAYING) {
		log.info("No move needed - game over (state={})", static_cast<int>(current_info.gameState));
		return false;
	}

	// True emergency - generate any legal move
	log.critical("EMERGENCY: No best move found (max_depth={}, last_completed={})", max_depth_, state.depth_completed);

	MoveList emergency_moves;
	MoveGenerator::ComputeLegalMoves(td.board, current_info, emergency_moves);

	if (emergency_moves.empty()) {
		log.critical("No legal moves - game is over");
		return false;
	}

	// Verify first move is legal
	if (!td.board.DoMove(emergency_moves[0])) {
		log.critical("First pseudolegal move {} is illegal!", MoveFormatter::ToCoord(emergency_moves[0]));

		// Try others
		for (const auto& move : emergency_moves) {
			if (td.board.DoMove(move)) {
				td.board.UndoMove(move);
				state.best_move = move;
				state.best_score = 0;
				td.pv_table.update(0, move);

				log.critical("Using legal emergency move: {}", MoveFormatter::ToCoord(move));
				return true;
			}
		}

		log.critical("No legal moves found - ComputeLegalMoves is broken!");
		return false;
	}

	// First move is legal
	td.board.UndoMove(emergency_moves[0]);
	state.best_move = emergency_moves[0];
	state.best_score = 0;
	td.pv_table.update(0, emergency_moves[0]);

	log.critical("Using emergency move: {}", MoveFormatter::ToCoord(emergency_moves[0]));
	return true;
}

void AIPerplex::log_search_complete(const SearchState& state, const PVTable& pv_table) const
{
	if (!s_logger)
		return;

	if (state.best_move.is_null() || pv_table.get_length(0) == 0) {
		return;
	}

	s_logger->info("Search complete: depth={}, score={}, move={}, nodes={}, stable={}", state.depth_completed,
	               state.best_score, MoveFormatter::ToCoord(state.best_move), state.nodes_at_completed_depth,
	               state.search_was_stable ? "yes" : "NO");
}

void AIPerplex::log_completed_iteration(const IterationMetrics& metrics, const PVTable& pv_table) const
{
	if (!s_logger)
		return;
	if (!s_logger->should_log(spdlog::level::info))
		return;

	std::string pv_line;
	pv_line.reserve(128);
	const int display_length = std::min(metrics.pv_length, 10);
	for (int i = 0; i < display_length; ++i) {
		if (i)
			pv_line += ", ";
		pv_line += MoveFormatter::ToCoord(pv_table.get_line(0)[i]);
	}

	s_logger->info("Depth {:>2}: Score: {:>6} Nodes: {:>8} PV[{}]: {} {}", metrics.depth, metrics.current_score,
	               metrics.nodes_searched, metrics.pv_length, pv_line,
	               (metrics.move_changed && metrics.depth > 1) ? "(!)" : "");
}

void AIPerplex::emit_iteration_info(const ThreadData& td, int depth, int score) const
{
	// The PV of an accepted iteration, asserted before anything reads it: a line spliced out
	// of two different subtrees replays illegally at the ply where the positions diverge.
	// td.board is back at the search root by the time an iteration is accepted, on the abort
	// path as much as the completed one. Checked ahead of the observer test because the
	// accepted line is what the search logs and what its first move is played from, whether
	// or not a UCI client is listening. Debug only — truncating a bad line in Release would
	// guarantee no GUI ever complains again while hiding whatever produced it, which is the
	// opposite of what this is for.
	assert(pv_replays_legally(td.board, std::span<const Move>(td.pv_table.get_line(0).data(),
	                                                          static_cast<size_t>(td.pv_table.get_length(0)))));

	if (!iteration_observer_)
		return;

	// The reported figure is the main-search-thread's cumulative count of both trees as
	// of this accepted iteration. It does not generally equal the final info/bestmove
	// line's total (AIPerplex.cpp GetMove(), ~line 256): a rejected trailing
	// iteration (REJECT_AND_STOP, see iterative_deepening()) still adds its
	// nodes to those counters before assess_iteration_quality() throws the
	// iteration away, so the final line's count is typically strictly greater
	// than this figure at Threads=1; under Lazy SMP the final total also sums
	// helper threads' nodes, which are invisible here. Not synchronising with
	// helpers on every accepted depth is intentional: this is a progress
	// indicator, not a decision input.
	const int length = td.pv_table.get_length(0);
	const auto& line = td.pv_table.get_line(0);

	IterationInfo iter;
	iter.depth = depth;
	iter.score = score;
	iter.nodes = td.nodes_searched + td.qnodes_searched;
	iter.pv.assign(line.begin(), line.begin() + length);

	iteration_observer_(iter);
}

void AIPerplex::log_aspiration_retry(int depth, int retry, int score, int alpha, int beta, bool fail_low) const
{
	if (!s_logger)
		return;

	s_logger->debug("D{:>2} ASPIRATION: {} retry {} score={:>6} new window=[{:>7},{:>7}]", depth,
	                fail_low ? "FAIL-LOW " : "FAIL-HIGH", retry, score, alpha, beta);
}

void AIPerplex::log_aspiration_full_window(int depth, int max_retries) const
{
	if (!s_logger)
		return;

	s_logger->debug("D{:>2} ASPIRATION: max retries ({}) reached, opening full window", depth, max_retries);
}
