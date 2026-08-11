// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "AIPerplex.h"
#include "MoveGenerator.h"
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
	if (s_logger) return;
	
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

			s_logger = std::make_shared<spdlog::logger>(		// was spdlog:async_logger
				"AIPerplex",
				spdlog::sinks_init_list{ console_sink, file_sink });
				//tp,
				//spdlog::async_overflow_policy::block);

			spdlog::register_logger(s_logger);
			s_logger->set_level(spdlog::level::debug);
			s_logger->flush_on(spdlog::level::debug);
		}
		catch (...) {
			// best-effort; leave it empty if creation fails
		}
	}
}

void AIPerplex::SetVerboseLogging(bool enabled) noexcept {
	s_verbose_logging = enabled;
	if (enabled) {
		ensure_logger_initialized();
		if (s_logger) s_logger->set_level(spdlog::level::debug);
	} else if (s_logger) {
		s_logger->set_level(spdlog::level::off);
	}
}

AIPerplex::AIPerplex(Board& board, unsigned md)
	: PlayerAiBase(board, md)
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
		const HashConfigurationResult result{
			true,
			requested,
			replacement->memory_mb(),
			replacement->bucket_count()
		};
		_tt = std::move(replacement);
		return result;
	}
	catch (const std::bad_alloc&) {
		return { false, requested, 0, 0 };
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
	td_.board = m_Board;			// thread-local copy — the search runs on this
	td_.nodes_searched = 0;
	td_.pv_table = PVTable{};		// fresh PV, exactly like the former GetMove() local
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
			break;            // partial iteration — discard score
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
			htd.board = m_Board;                       // same seed as td_.board
			htd.info_seq.clear();
			htd.info_seq.emplace_back(info);           // same root info as init_search gives td_
			htd.clear_killers();
			htd.clear_null_move_flags();
			htd.nodes_searched = 0;
			helpers.emplace_back([this, &htd, effective_depth, this_tt = _tt.get()] {
				helper_loop(htd, static_cast<int>(effective_depth), *this_tt);
			});
		}
	}

	SearchResult result = iterative_deepening(td_, static_cast<int>(effective_depth), *_tt);
	last_result_ = result;   // expose via GetLastResult()

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
	for (size_t i = 0; i + 1 < static_cast<size_t>(threads); ++i)
		total_nodes += helper_tds_[i]->nodes_searched;
	last_result_.nodes_searched = total_nodes;

	auto elapsed = StopTimerAndAdjustVars(static_cast<size_t>(total_nodes));

	Move bestMove = result.best_move;

	// Defensive check for game-over scenario
	if ( bestMove.is_null()) {
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
		s_logger->info(
			"GetMove complete: move={}, score={}, depth={}, time={}ms, nodes={}, stable={}",
			MoveFormatter::ToCoord(bestMove),
			result.best_score,
			result.depth_completed,
			elapsed.count(),
			total_nodes,
			result.search_was_stable ? "yes" : "NO");
	}
	// Equivalent of CheckGameOver(info, false), reading the thread-local
	// info_seq root instead of the base-class m_infoSeq (unused by AIPerplex).
	info = td_.info_seq.at(0);
	if (info.gameState != GameStates::STILL_PLAYING)
	{
		EGameStateChanged.fire(this, info.gameState);
	}
	info.UpdateBoardInfo(bestMove, m_Board.GetEffectiveMovPiece(bestMove));

	return bestMove;
}

SearchResult  AIPerplex::iterative_deepening(ThreadData& td, int max_depth, TranspositionTable& tt) {
	SearchState state;
	td.nodes_since_check_ = 0;   // reset node counter for this search
	bool extra_depth_used = false;   // soft-limit extension granted at most once per search

	td.clear_killers();	// Clear killer moves at the start of the search
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
			currentBestScore = pvs(td, depth, -GameValues::Search_Init, GameValues::Search_Init,
				0, true, tt);
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
		metrics.interrupted = ShouldStopSearch();	// Check on time expiry
		metrics.move_changed = (metrics.current_move != state.last_iteration_move);
		metrics.score_delta = currentBestScore - state.best_score;
		metrics.completion_ratio = (state.nodes_at_completed_depth > 0)
			? static_cast<double>(metrics.nodes_searched) / state.nodes_at_completed_depth
			: 1.0;
		
		// Debug logging (detailed diagnostics)
		log_iteration_eval(metrics, td.pv_table);

		// Decide what to do
		IterationDecision decision;
		RejectionReason rejection_reason = RejectionReason::NONE;

		if (!metrics.interrupted) {
			decision = IterationDecision::ACCEPT_AND_CONTINUE;	// Depth completed
				}
		else {
			// Interrupted - assess quality
			rejection_reason = assess_iteration_quality(metrics, state);
			decision = (rejection_reason != RejectionReason::NONE)
				? IterationDecision::REJECT_AND_STOP
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

			// Soft limit gate: stop after this depth if the allocated time budget
			// is consumed.  Exception: if the best move just changed, allow one
			// more depth to verify the new move (the hard limit will cut it off).
			if (time_manager_.should_stop_iteration()) {
				if (!metrics.move_changed || extra_depth_used) {
					continue_iteration = false;
					break;
				}
				extra_depth_used = true;   // grant extension exactly once
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
			continue_iteration = false;
			break;

		case IterationDecision::REJECT_AND_STOP:
			log_rejection(depth, rejection_reason, metrics, state);
			continue_iteration = false;
			break;
		}

		if (!continue_iteration) {
			break;	// Exit iteration_deepening
	}
}

	// Handle empty move emergency
	if (state.best_move.is_null()) {
		if (!handle_empty_move_emergency(td, state)) {
			// Game over - return what we have
			return SearchResult{
				state.best_move,
				state.best_score,
				state.depth_completed,
				state.nodes_at_completed_depth,
				state.search_was_stable
			};
		}
	}

	// Final logging
	if (state.depth_completed > 0) {
		log_search_complete(state, td.pv_table);
	}
	return SearchResult{
		state.best_move,
		state.best_score,
		state.depth_completed,
		state.nodes_at_completed_depth,
		state.search_was_stable
	};
}

int AIPerplex::pvs(ThreadData& td, int depth, int alpha, int beta, int ply, bool is_pv_node, TranspositionTable& tt)
{
	// Fast early exit: IsAborted() reads only the latched atomic (no clock call).
	// After the first ShouldStopSearch() fires and latches the flag, this collapses
	// the entire call stack in O(depth) steps instead of O(tree_size).
	if (IsAborted())
		return GameValues::Draw;

	// Node-based time polling: check every 1024 nodes to amortise chrono::now() cost.
	// On first expiry, ShouldStopSearch() latches should_stop_; all future IsAborted()
	// calls above then return true for free. Only thread 0 ever calls the wall-clock
	// check — helper threads increment their own td.nodes_since_check_ but rely solely
	// on the IsAborted() fast-path above (no chrono::now() calls off the main thread).
	if (td.thread_id == 0 && (++td.nodes_since_check_ & 1023) == 0) {
		if (ShouldStopSearch())
			return GameValues::Draw;
	}

	td.pv_table.clear_ply(ply);

	// We need the info on the current board state
	GameInfo info = td.get_last_info(ply);

	// Test for 50 moves rule and threefold repetition
	if (td.check_draws(info, ply))
		return GameValues::Draw;

	// Er vi naaet til bunden af traeet - evaluering?
	//	See if static eval will cause a cutoff or raise alpha.
	if (depth <= 0)
		return quiescence(td, alpha, beta, 0, ply, tt);

	auto key = td.board.get_zobrist_hash();
	int original_alpha = alpha;
	Move hash_move;
	Move pv_move;

	// TT probe
	if (auto entry = tt.probe(key, ply))
	{
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
		if (null_score >= beta) {
			tt.store(key, static_cast<int16_t>(null_score), static_cast<int16_t>(depth),
				static_cast<int16_t>(ply), Move::EmptyMove(), BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::MAIN);
			return null_score;
		}
	}

	MoveList moveList;
	MoveGenerator::ComputeLegalMoves(td.board, info, moveList);

	bool first_child = true;
	int best_value = -GameValues::Search_Init;
	Move best_move;


	// Stack-allocated scored index array — zero heap allocation per call.
	std::array<std::pair<int, int>, MoveList::MAX_MOVES> scored_idx;
	const int n = static_cast<int>(moveList.size());
	const eColor side = td.board.GetCurrentColor();

	MoveSorter::ScoreMoves(moveList, n, td.board, side,
		pv_move, hash_move,
		td.killers[ply][0], td.killers[ply][1],
		td.history, scored_idx);

	bool moveFound = false;

	// Iterate by sorted index — no rebuild of moveList needed
	for (int si = 0; si < n; ++si) {
		const Move& move = moveList[scored_idx[si].second];

		td.nodes_searched++;

		if (td.board.DoMove(move))
		{
			// Tilfoejer dette traek til nuvaerende traekfoelge - og opdaterer resten
			td.add_move_to_seq(move, ply);
			int value;

			if (first_child) {
				// Full window search for first move
				value = -pvs(td, depth - 1, -beta, -alpha, ply + 1, is_pv_node, tt);
				first_child = false;
			}
			else {
				const bool isCapture   = MoveHelper::IsCapture(move);
				const bool isPromotion = MoveHelper::IsPromote(move);
				const bool isKiller    = (move == td.killers[ply][0] || move == td.killers[ply][1]);

				// Late Move Reductions: reduce quiet, non-killer, non-evasion moves
				// that appear late in the sorted order. Skip conditions are conservative:
				// captures, promotions, killers, evasions (in_check), PV nodes, and early
				// moves are always searched at full depth.
				// Future skip candidates: passed pawn pushes, moves giving check.
				const bool applyLMR = tuning_.lmr_enabled
					&& !is_pv_node
					&& !in_check
					&& !isCapture
					&& !isPromotion
					&& !isKiller
					&& si >= tuning_.lmr_min_move_index
					&& depth >= tuning_.lmr_min_depth;

				if (applyLMR) {
					// sqrt formula: scales naturally with depth and move index.
					// Clamped to [1, depth-1]; when R == depth-1 the recursive call is
					// at depth 0 (falls into quiescence). The re-search below restores
					// full depth if alpha is beaten.
					const int R = std::min(
						std::max(1, static_cast<int>(
							std::sqrt(static_cast<double>(depth - 1)) *
							std::sqrt(static_cast<double>(si - 1)))),
						depth - 1);

					// Reduced-depth null-window search
					value = -pvs(td, depth - 1 - R, -alpha - 1, -alpha, ply + 1, false, tt);

					// Re-search at full depth-1 null window if the reduced result beats alpha
					if (value > alpha && !ShouldStopSearch())
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

			if (beta <= alpha)
			{
				td.store_killer(ply, move);
				td.update_history(side, move, depth);
				break;
		}
	}
	}

	// Classify node and store
	BoundType bound;
	NodeType node_type;

	if (!moveFound) {
		// No captures were available to search
		// This is ALL_NODE regardless of whether stand_pat improved alpha
		node_type = NodeType::ALL_NODE;
		bound = (best_value > original_alpha) ? BoundType::EXACT : BoundType::UPPER;
	}
	else if (best_value <= original_alpha) {
		bound = BoundType::UPPER;
		node_type = NodeType::ALL_NODE;
	}
	else if (best_value >= beta) {
		bound = BoundType::LOWER;
		node_type = NodeType::CUT_NODE;
	}
	else {
		bound = BoundType::EXACT;
		node_type = is_pv_node ? NodeType::PV_NODE : NodeType::ALL_NODE;
	}

	tt.store(key, static_cast<int16_t>(best_value),
		static_cast<int16_t>(depth), static_cast<int16_t>(ply), best_move, bound, node_type, SearchPhase::MAIN);

	return adjustScoreForGameState(td, moveFound, ply, best_value);
}

int AIPerplex::adjustScoreForGameState(ThreadData& td, bool moveFound, int ply, int score)
{
	// Any legal moves found?
	if (!moveFound)
	{
		// Nope - so we are either mate or remis here !
		if (td.board.InCheck())
		{
			// Oops - we are mate!!
			td.update_game_state(ply, td.board.GetCurrentColor() == WHITE ? GameStates::BLACK_WON : GameStates::WHITE_WON);
			if (ply == 0)		// Are we at root?
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

int AIPerplex::quiescence(ThreadData& td, int alpha, int beta, int qsearch_depth, int ply, TranspositionTable& tt)
{
	// Fast early exit: IsAborted() reads only the latched atomic (no clock call).
	// Mirrors pvs() — collapses quiescence chains in O(depth) after latch fires.
	if (IsAborted())
		return GameValues::Draw;

	// Mirrors pvs(): only thread 0 calls the wall-clock check; helpers rely on
	// the IsAborted() fast-path above.
	if (td.thread_id == 0 && (++td.nodes_since_check_ & 1023) == 0) {
		if (ShouldStopSearch())
			return GameValues::Draw;
	}

	constexpr int MAX_QSEARCH_DEPTH = 15;  // Lets keep it real

	// Limit quiescence extension by qsearch
	if (qsearch_depth > MAX_QSEARCH_DEPTH)
	{
		return Eval->Evaluate(td.board);
	}

	int original_alpha = alpha;

	auto key = td.board.get_zobrist_hash();

	// Probe TT for cached info
	if (auto entry = tt.probe(key, ply)) {
		// Only use TT cutoff if QUIESCENCE entry AND depth stored is sufficient
		if (entry->phase == SearchPhase::QUIESCENCE && entry->depth >= qsearch_depth) {
			// Scale quiescence depth for comparison
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

	// Stand pat evaluation first
	const int stand_pat = Eval->Evaluate(td.board);
	if (stand_pat >= beta)
	{
		// Store and cutoff
		tt.store(key, static_cast<int16_t>(beta), static_cast<int16_t>(qsearch_depth), static_cast<int16_t>(ply),
			Move::EmptyMove(), BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::QUIESCENCE);
		return beta;
	}
	// stand-pat is the baseline (valid option: don't capture)
	int best_value = stand_pat;
	if (stand_pat > alpha)
		alpha = stand_pat;

	MoveList moveList;
	// Generate only capture moves and promotions
	MoveGenerator::ComputeCaptures(td.board, info, moveList);
	// Sort the found captures
	MoveSorter::SortMovesByValue(moveList, moveList.size(), td.board);

	// Tjek om der er lovlige brugbare traek her
	bool moveFound = false;
	Move best_move = Move::EmptyMove();

	// Compute once: delta pruning must not fire when in check (all evasions must be searched)
	const bool in_check = td.board.InCheck();

	for (const auto& move : moveList)
	{
		// Delta pruning: skip captures whose best-case material gain cannot raise alpha.
		// stand_pat is already the alpha lower-bound; MoveHelper::Value() gives the MVV-LVA
		// score which is conservatively ≤ the raw captured-piece value, so pruning is safe.
		// Promotions always score ≥ 800 (queen − pawn gain) and are never pruned.
		if (!in_check && stand_pat + MoveHelper::Value(move, td.board.GetEffectiveMovPiece(move), td.board.GetCapturedPiece(move)) + tuning_.delta_pruning_margin < alpha)
			continue;

		if (!td.board.DoMove(move))
			continue;

		td.add_move_to_seq(move, ply);

		int score = -quiescence(td, -beta, -alpha, qsearch_depth + 1, ply + 1, tt);
		td.board.UndoMove(move);

		moveFound = true;

		if (score >= beta) {
			tt.store(key, static_cast<int16_t>(beta), static_cast<int16_t>(qsearch_depth), static_cast<int16_t>(ply),
				move, BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::QUIESCENCE);
			return beta;
		}
		if (score > best_value) {
			best_value = score;
			alpha = std::max(alpha, score);
			best_move = move;
		}
	}
	// Found no legal moves here
	// We don't check for mate / stalemate here, because without generating all
	// of the moves leading up to it, we don't know if the position could have
	// been avoided by one side or not.  So simply return our evaluation score 
	/*if (!moveFound)
	{
		return stand_pat;
	}*/

	// Classify node type correctly
	NodeType node_type;
	BoundType bound;

	if (!moveFound) {
		// No captures were available to search
		// This is ALL_NODE regardless of whether stand_pat improved alpha
		node_type = NodeType::ALL_NODE;
		bound = (best_value > original_alpha) ? BoundType::EXACT : BoundType::UPPER;
	}
	else if (best_value <= original_alpha) {
		// Failed to improve alpha (or came close)
		node_type = NodeType::ALL_NODE;
		bound = BoundType::UPPER;
	}
	else if (best_value >= beta) {
		// Beta cutoff (would have returned earlier, but for completeness)
		node_type = NodeType::CUT_NODE;
		bound = BoundType::LOWER;
	}
	else {
		// Captures found AND improved alpha, no beta cutoff => PV node
		node_type = NodeType::PV_NODE;
		bound = BoundType::EXACT;
	}

	tt.store(key, static_cast<int16_t>(best_value), static_cast<int16_t>(qsearch_depth), static_cast<int16_t>(ply),
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
	int beta  = std::min(seed_score + delta, static_cast<int>(GameValues::Search_Init));
	int score = seed_score;  // safe fallback if interrupted before the first pvs() call

	for (int retry = 0; ; ++retry) {
		if (ShouldStopSearch()) {
			// Interrupt before entering pvs(): clear PV so iterative_deepening sees EmptyMove
			// and triggers INCOMPLETE rejection rather than accepting a stale PV as valid.
			td.pv_table.clear_ply(0);
			return score;
		}

		score = pvs(td, depth, alpha, beta, 0, true, tt);

		if (ShouldStopSearch())
			return score;

		// In-window: accept the score
		if (score > alpha && score < beta)
			return score;

		// Safety fallback: open full window after max retries
		if (retry >= tuning_.aspiration_max_retries) {
			if (td.thread_id == 0) log_aspiration_full_window(depth, tuning_.aspiration_max_retries);
			if (!ShouldStopSearch())
				score = pvs(td, depth, -GameValues::Search_Init, GameValues::Search_Init, 0, true, tt);
			return score;
		}

		// Widen on the failing side; double delta for the next potential miss.
		// Log after updating so the message shows the new window being tried.
		delta *= 2;
		if (score <= alpha) {
			alpha = std::max(seed_score - delta, -static_cast<int>(GameValues::Search_Init));
			if (td.thread_id == 0) log_aspiration_retry(depth, retry + 1, score, alpha, beta, true);
		} else {
			beta  = std::min(seed_score + delta,  static_cast<int>(GameValues::Search_Init));
			if (td.thread_id == 0) log_aspiration_retry(depth, retry + 1, score, alpha, beta, false);
		}
	}
}

// ============================================================================
// HELPERS
// ============================================================================
// Killer/history/null-flag maintenance lives on ThreadData (ThreadData.h).

AIPerplex::RejectionReason AIPerplex::assess_iteration_quality(
	const IterationMetrics& metrics,
	const SearchState& state) const
{
	// CASE 1: Obviously incomplete
	if (metrics.current_move.is_null() || metrics.nodes_searched < tuning_.min_nodes_threshold) {
		return RejectionReason::INCOMPLETE;
	}

	// CASE 2: Too few nodes compared to previous depth
	if (state.depth_completed > 0 &&
		state.nodes_at_completed_depth > 0 &&
		metrics.completion_ratio < tuning_.min_completion_ratio) {
		return RejectionReason::TOO_FEW_NODES;
	}

	// CASE 3: PV too short
	if (metrics.pv_length < std::max(1, static_cast<int>(metrics.depth * tuning_.min_pv_ratio)) &&
		state.depth_completed > 0) {
		return RejectionReason::SHORT_PV;
	}

	// CASE 4: Score dropped to 0 suspiciously
	if (metrics.current_score == 0 &&
		state.depth_completed > 0 &&
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

void AIPerplex::log_iteration_eval(
	const IterationMetrics& metrics,
	const PVTable& pv_table) const
{
	if (!s_logger) return;
	if (!s_logger->should_log(spdlog::level::debug)) return;

	// Build PV string
	std::string pv_line;
	pv_line.reserve(80);
	const int display_length = std::min(metrics.pv_length, 6);
	for (int i = 0; i < display_length; ++i) {
		if (i) pv_line += ", ";
		pv_line += MoveFormatter::ToCoord(pv_table.get_line(0)[i]);
	}
	if (metrics.pv_length > display_length) {
		pv_line += " ...";
	}

	s_logger->debug(
		"D{:>2} EVAL: move={:<8} score={:>6} (Δ{:>+5}) nodes={:>8} ({:>3}%) "
		"pv={:>2} int={} chg={} PV:[{}]",
		metrics.depth,
		metrics.current_move.is_null() ? "EMPTY" : MoveFormatter::ToCoord(metrics.current_move),
		metrics.current_score,
		metrics.score_delta,
		metrics.nodes_searched,
		static_cast<int>(metrics.completion_ratio * 100),
		metrics.pv_length,
		metrics.interrupted ? "Y" : "N",
		metrics.move_changed ? "Y" : "N",
		pv_line
	);
}

void AIPerplex::log_rejection(
	int depth,
	RejectionReason reason,
	const IterationMetrics& metrics,
	const SearchState& state) const
{
	if (!s_logger) return;

	switch (reason) {
	case RejectionReason::INCOMPLETE:
		s_logger->debug(
			"Depth {:>2}: REJECTED[R1:INCOMPLETE] (nodes={}, move={}) - Using depth {}",
			depth, metrics.nodes_searched,
			metrics.current_move.is_null() ? "EMPTY" : "ok",
			state.depth_completed);
		break;

	case RejectionReason::TOO_FEW_NODES:
		s_logger->debug(
			"Depth {:>2}: REJECTED[R2:TOO_FEW_NODES] ({} = {:.0f}% of D{}) - using depth {}",
			depth, metrics.nodes_searched,
			metrics.completion_ratio * 100,
			state.depth_completed,
			state.depth_completed);
		break;

	case RejectionReason::SHORT_PV:
		s_logger->debug(
			"Depth {:>2}: REJECTED[R3:SHORT_PV] (pv={} vs depth={}) - using depth {}",
			depth, metrics.pv_length, depth, state.depth_completed);
		break;

	case RejectionReason::SCORE_DROP:
		s_logger->debug(
			"Depth {:>2}: REJECTED[R4:SCORE_DROP] ({} → 0) - Using depth {}",
			depth, state.best_score, state.depth_completed);
		break;

	case RejectionReason::MOVE_CHANGED:
		s_logger->debug(
			"Depth {:>2}: REJECTED[R5:MOVE_CHANGED] ({} → {}) - Using depth {}",
			depth, MoveFormatter::ToCoord(state.last_iteration_move),
			MoveFormatter::ToCoord(metrics.current_move), state.depth_completed);
		break;

	default:
		break;
	}
}

void AIPerplex::log_acceptance(const IterationMetrics& metrics) const {
	if (!s_logger) return;

	// Noteworthy so Info
	s_logger->info(
		"Depth {:>2}: ACCEPTED[INTERRUPTED] (nodes={}, score={}, pv={})",
		metrics.depth, metrics.nodes_searched,
		metrics.current_score, metrics.pv_length);
}

bool AIPerplex::should_stop_early(int depth, int score, int pv_length) const {
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
			s_logger->info(
				"Short PV ({} vs depth {}) indicates forced line, stopping",
				pv_length, depth);
		}
		return true;
	}

	return false;
}

bool AIPerplex::should_try_null_move(const ThreadData& td, int depth, int beta, int ply, bool is_pv_node, bool in_check) const
{
	if (!tuning_.null_move_enabled) return false;
	if (is_pv_node || in_check) return false;
	if (depth < tuning_.null_move_min_depth) return false;
	if (std::abs(beta) >= GameValues::Mate_Threshold) return false;
	if (td.last_move_was_null[ply]) return false;

	// Zugzwang guard: refuse to "pass" for a side with fewer than two
	// non-pawn pieces — the null-move assumption ("a free pass is never
	// better than moving") is false in king+pawn endgames AND in
	// single-piece endgames won by domination/zugzwang (issue #66: KQ vs KR,
	// where the lone rook loses only because its side must move).
	const eColor side = td.board.GetCurrentColor();
	const auto boards = td.board.GetBitBoards();
	const BITBOARD non_pawn_material =
		boards[static_cast<BITBOARD>(KNIGHT) + side] | boards[static_cast<BITBOARD>(BISHOP) + side]
		| boards[static_cast<BITBOARD>(ROOK) + side] | boards[static_cast<BITBOARD>(QUEEN) + side];
	return std::popcount(non_pawn_material) >= 2;
}

bool AIPerplex::handle_empty_move_emergency(
	ThreadData& td,
	SearchState& state)
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
		log.info("No move needed - game over (state={})",
			static_cast<int>(current_info.gameState));
		return false;
	}

	// True emergency - generate any legal move
	log.critical("EMERGENCY: No best move found (max_depth={}, last_completed={})",
		max_depth_, state.depth_completed);

	MoveList emergency_moves;
	MoveGenerator::ComputeLegalMoves(td.board, current_info, emergency_moves);

	if (emergency_moves.empty()) {
		log.critical("No legal moves - game is over");
		return false;
	}

	// Verify first move is legal
	if (!td.board.DoMove(emergency_moves[0])) {
		log.critical("First pseudolegal move {} is illegal!",
			MoveFormatter::ToCoord(emergency_moves[0]));

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

void AIPerplex::log_search_complete(
	const SearchState& state,
	const PVTable& pv_table) const
{
	if (!s_logger) return;

	if (state.best_move.is_null() || pv_table.get_length(0) == 0) {
		return;
	}

	s_logger->info(
		"Search complete: depth={}, score={}, move={}, nodes={}, stable={}",
		state.depth_completed,
		state.best_score,
		MoveFormatter::ToCoord(state.best_move),
		state.nodes_at_completed_depth,
		state.search_was_stable ? "yes" : "NO");
}

void AIPerplex::log_completed_iteration(
	const IterationMetrics& metrics,
	const PVTable& pv_table) const
{
	if (!s_logger) return;
	if (!s_logger->should_log(spdlog::level::info)) return;

	std::string pv_line;
	pv_line.reserve(128);
	const int display_length = std::min(metrics.pv_length, 10);
	for (int i = 0; i < display_length; ++i) {
		if (i) pv_line += ", ";
		pv_line += MoveFormatter::ToCoord(pv_table.get_line(0)[i]);
	}

	s_logger->info(
		"Depth {:>2}: Score: {:>6} Nodes: {:>8} PV[{}]: {} {}",
		metrics.depth,
		metrics.current_score,
		metrics.nodes_searched,
		metrics.pv_length,
		pv_line,
		(metrics.move_changed && metrics.depth > 1) ? "(!)" : "");
}

void AIPerplex::log_aspiration_retry(int depth, int retry, int score, int alpha, int beta, bool fail_low) const
{
	if (!s_logger) return;

	s_logger->debug(
		"D{:>2} ASPIRATION: {} retry {} score={:>6} new window=[{:>7},{:>7}]",
		depth,
		fail_low ? "FAIL-LOW " : "FAIL-HIGH",
		retry,
		score,
		alpha,
		beta);
}

void AIPerplex::log_aspiration_full_window(int depth, int max_retries) const
{
	if (!s_logger) return;

	s_logger->debug(
		"D{:>2} ASPIRATION: max retries ({}) reached, opening full window",
		depth, max_retries);
}
