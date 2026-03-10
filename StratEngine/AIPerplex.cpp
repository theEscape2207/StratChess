// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "AIPerplex.h"
#include "MoveGenerator.h"
#include "Sort.h"
#include "Utils/Logger.h"
#include "defines.h"
#include "MoveHelper.h"
#include <cstring>
#include <iterator>
#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

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

AIPerplex::AIPerplex(_In_ unsigned md)
	: PlayerAiBase(md)
{
	// allocate TT once per AIPerplex instance; size can be tuned or read from config
	_tt = std::make_unique<TranspositionTable>(256);

	clear_killers();
	clear_history();
	SetVerboseLogging(true);
}

// PVS Iterative transpositional alpha beta search
// Transposition tables
Move AIPerplex::GetMove(_Inout_ GameInfo& info)
{
	InitMoveVariables(info);
	// Only clear TT if new game (preserve across moves for better performance)
	if (info.fullMoveCount == 1) {
	_tt->clear();
	}
	PVTable pv_table;
	StartTimer();
	
	SearchResult result = iterative_deepening(static_cast<int>(max_depth_), *_tt, pv_table);

	auto elapsed = StopTimerAndAdjustVars();

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
			bestMove.Output(),
			result.best_score,
			result.depth_completed,
			elapsed.count(),
			m_SearchCount,
			result.search_was_stable ? "yes" : "NO");
	}
	info = m_Board.GetGameInfo();
	CheckGameOver(info, false);
	info.UpdateBoardInfo(bestMove, m_Board.GetEffectiveMovPiece(bestMove));

	return bestMove;
}

SearchResult  AIPerplex::iterative_deepening(int max_depth, TranspositionTable& tt, PVTable& pv_table) {
	SearchState state;

	clear_killers();	// Clear killer moves at the start of the search

	for (int depth = 1; depth <= max_depth; ++depth) {

		// BEFORE ITERATION: Prepare for this depth's search
		tt.newSearchIteration();
		age_history();
		const int64_t nodes_at_start = m_SearchCount;

		// EXECUTE SEARCH: This might get interrupted by timeout
		int currentBestScore;
		if (state.depth_completed == 0 || !tuning_.aspiration_enabled) {
			// Depth 1 or kill-switch: always full window (no reliable seed yet)
			currentBestScore = pvs(depth, -GameValues::Search_Init, GameValues::Search_Init,
				0, true, tt, pv_table);
		} else {
			currentBestScore = search_with_aspiration(depth, state.best_score, tt, pv_table);
		}

		// Gather metrics
		IterationMetrics metrics;
		metrics.depth = depth;
		metrics.current_move = pv_table.get_pv_move(0);
		metrics.current_score = currentBestScore;
		metrics.nodes_searched = m_SearchCount - nodes_at_start;
		metrics.pv_length = pv_table.get_length(0);
		metrics.interrupted = ShouldStopSearch();	// Check on time expiry
		metrics.move_changed = (metrics.current_move != state.last_iteration_move);
		metrics.score_delta = currentBestScore - state.best_score;
		metrics.completion_ratio = (state.nodes_at_completed_depth > 0)
			? static_cast<double>(metrics.nodes_searched) / state.nodes_at_completed_depth
			: 1.0;
		
		// Debug logging (detailed diagnostics)
		log_iteration_eval(metrics, pv_table);

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

			log_completed_iteration(metrics, pv_table);

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
		if (!handle_empty_move_emergency(state, pv_table)) {
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
		log_search_complete(state, pv_table);
	}
	return SearchResult{
		state.best_move,
		state.best_score,
		state.depth_completed,
		state.nodes_at_completed_depth,
		state.search_was_stable
	};
}

int AIPerplex::pvs(int depth, int alpha, int beta, int ply, bool is_pv_node, TranspositionTable& tt, PVTable& pv_table)
{
	// Check time and stop signal
	if (ShouldStopSearch()) {
		return GameValues::Draw;
	}

	pv_table.clear_ply(ply);

	// We need the info on the current board state
	const GameInfo& info = GetLastBoardInfo(ply);

	// Test for 50 moves rule and threefold repetition
	if (checkDraws(info, ply))
		return GameValues::Draw;

	// Er vi naaet til bunden af traeet - evaluering?
	//	See if static eval will cause a cutoff or raise alpha.
	if (depth <= 0)
		return quiescence(alpha, beta, 0, ply, tt);

	auto key = m_Board.get_zobrist_hash();
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
	/*else {
		debug_tt_cache_misses(key, ply);
	}*/

	// Get PV move from previous iteration
	if (is_pv_node && ply > 0) {
		pv_move = pv_table.get_pv_move(ply);
	}

	MoveList moveList;
	MoveGenerator::ComputeLegalMoves(info, moveList);
	
	bool first_child = true;
	int best_value = -GameValues::Search_Init;
	Move best_move;


	// Stack-allocated scored index array — zero heap allocation per call.
	std::array<std::pair<int, int>, MoveList::MAX_MOVES> scored_idx;
	const int n = static_cast<int>(moveList.size());
	const eColor side = m_Board.GetCurrentColor();

	MoveSorter::ScoreMoves(moveList, n, m_Board, side,
		pv_move, hash_move,
		killers_[ply][0], killers_[ply][1],
		history_, scored_idx);

	const bool in_check = m_Board.InCheck();
	bool moveFound = false;

	// Iterate by sorted index — no rebuild of moveList needed
	for (int si = 0; si < n; ++si) {
		const Move& move = moveList[scored_idx[si].second];

		m_SearchCount++;

		if (m_Board.DoMove(move))
		{
			// Tilfoejer dette traek til nuvaerende traekfoelge - og opdaterer resten
			AddMoveToSeq(move, ply);
			int value;

			if (first_child) {
				// Full window search for first move
				value = -pvs(depth - 1, -beta, -alpha, ply + 1, is_pv_node, tt, pv_table);
				first_child = false;
			}
			else {
				const bool isCapture   = MoveHelper::IsCapture(move);
				const bool isPromotion = MoveHelper::IsPromote(move);
				const bool isKiller    = (move == killers_[ply][0] || move == killers_[ply][1]);

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
					value = -pvs(depth - 1 - R, -alpha - 1, -alpha, ply + 1, false, tt, pv_table);

					// Re-search at full depth-1 null window if the reduced result beats alpha
					if (value > alpha && !ShouldStopSearch())
						value = -pvs(depth - 1, -alpha - 1, -alpha, ply + 1, false, tt, pv_table);
				} else {
					// Normal null-window search (unchanged)
					value = -pvs(depth - 1, -alpha - 1, -alpha, ply + 1, false, tt, pv_table);
				}

				// Re-search with full window at PV node (unchanged from original)
				if (value > alpha && is_pv_node)
					value = -pvs(depth - 1, -beta, -alpha, ply + 1, true, tt, pv_table);
			}

			m_Board.UndoMove(move);
			moveFound = true;

			if (value > best_value) {
				best_value = value;
				best_move = move;

				if (value > alpha) {
					alpha = value;

					// Update PV when alpha improves
					if (is_pv_node) {
						pv_table.update(ply, move);
					}
					// Update history for non-capture moves
					//if (!move.is_capture()) {
					//	data.move_ordering.update_history(move, depth, false);
				}
			}

			if (beta <= alpha)
			{
				store_killer(ply, move);
				update_history(side, move, depth);
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

	// Verify immediately
	//assert_tt_store(tt, key, ply, best_value, depth, best_move, bound, node_type, SearchPhase::MAIN);

	return adjustScoreForGameState(moveFound, ply, best_value);
}

int AIPerplex::adjustScoreForGameState(bool moveFound, int ply, int score)
{
	// Any legal moves found?
	if (!moveFound)
	{
		// Nope - so we are either mate or remis here !
		if (m_Board.InCheck())
		{
			// Oops - we are mate!!
			UpdateGameState(ply, m_Board.GetCurrentColor() == WHITE ? GameStates::BLACK_WON : GameStates::WHITE_WON);
			if (ply == 0)		// Are we at root?
			{
				_bestScore = -GameValues::Mate;
			}
			// Checkmate - prefer shorter mates
			return -GameValues::Mate + ply;
		}
		// Else No move and not in check - Pat!
		UpdateGameState(ply, GameStates::DRAW_PAT);
		return GameValues::Draw;
	}

	UpdateGameState(ply, GameStates::STILL_PLAYING);

	return score;
}

int AIPerplex::quiescence(int alpha, int beta, int qsearch_depth, int ply, TranspositionTable& tt)
{
	constexpr int MAX_QSEARCH_DEPTH = 15;  // Lets keep it real

	// Limit quiescence extension by qsearch
	if (qsearch_depth > MAX_QSEARCH_DEPTH)
	{
		return Eval->Evaluate();
	}

	int original_alpha = alpha;

	auto key = m_Board.get_zobrist_hash();

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
	const GameInfo& info = GetLastBoardInfo(ply);

	// Stand pat evaluation first
	const int stand_pat = Eval->Evaluate();
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
	MoveGenerator::ComputeCaptures(info, moveList);
	// Sort the found captures
	MoveSorter::SortMovesByValue(moveList, moveList.size(), m_Board);

	// Tjek om der er lovlige brugbare traek her
	bool moveFound = false;
	Move best_move = Move::EmptyMove();

	// Compute once: delta pruning must not fire when in check (all evasions must be searched)
	const bool in_check = m_Board.InCheck();

	for (const auto& move : moveList)
	{
		// Delta pruning: skip captures whose best-case material gain cannot raise alpha.
		// stand_pat is already the alpha lower-bound; MoveHelper::Value() gives the MVV-LVA
		// score which is conservatively ≤ the raw captured-piece value, so pruning is safe.
		// Promotions always score ≥ 800 (queen − pawn gain) and are never pruned.
		if (!in_check && stand_pat + MoveHelper::Value(move, m_Board.GetEffectiveMovPiece(move), m_Board.GetCapturedPiece(move)) + tuning_.delta_pruning_margin < alpha)
			continue;

		if (!m_Board.DoMove(move))
			continue;

		AddMoveToSeq(move, ply);

		int score = -quiescence(-beta, -alpha, qsearch_depth + 1, ply + 1, tt);
		m_Board.UndoMove(move);

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
int AIPerplex::search_with_aspiration(int depth, int seed_score, TranspositionTable& tt, PVTable& pv_table)
{
	int delta = tuning_.aspiration_initial_delta;
	int alpha = std::max(seed_score - delta, -GameValues::Search_Init);
	int beta  = std::min(seed_score + delta, static_cast<int>(GameValues::Search_Init));
	int score = seed_score;  // safe fallback if interrupted before the first pvs() call

	for (int retry = 0; ; ++retry) {
		if (ShouldStopSearch()) {
			// Interrupt before entering pvs(): clear PV so iterative_deepening sees EmptyMove
			// and triggers INCOMPLETE rejection rather than accepting a stale PV as valid.
			pv_table.clear_ply(0);
			return score;
		}

		score = pvs(depth, alpha, beta, 0, true, tt, pv_table);

		if (ShouldStopSearch())
			return score;

		// In-window: accept the score
		if (score > alpha && score < beta)
			return score;

		// Safety fallback: open full window after max retries
		if (retry >= tuning_.aspiration_max_retries) {
			log_aspiration_full_window(depth, tuning_.aspiration_max_retries);
			if (!ShouldStopSearch())
				score = pvs(depth, -GameValues::Search_Init, GameValues::Search_Init, 0, true, tt, pv_table);
			return score;
		}

		// Widen on the failing side; double delta for the next potential miss.
		// Log after updating so the message shows the new window being tried.
		delta *= 2;
		if (score <= alpha) {
			alpha = std::max(seed_score - delta, -static_cast<int>(GameValues::Search_Init));
			log_aspiration_retry(depth, retry + 1, score, alpha, beta, true);
		} else {
			beta  = std::min(seed_score + delta,  static_cast<int>(GameValues::Search_Init));
			log_aspiration_retry(depth, retry + 1, score, alpha, beta, false);
		}
	}
}

// ============================================================================
// HELPERS
// ============================================================================
void AIPerplex::clear_killers() noexcept
{
	for (auto& ply_killers : killers_)
		for (auto& k : ply_killers)
			k = Move::EmptyMove();
}

void AIPerplex::store_killer(int ply, const Move& move) noexcept
{
	// Only quiet moves are stored as killers
	if (MoveHelper::IsCapture(move))
		return;
	// Avoid storing the same move twice in slot 0
	if (killers_[ply][0] == move)
		return;
	// Shift slot 0 to slot 1, then store new killer in slot 0
	killers_[ply][1] = killers_[ply][0];
	killers_[ply][0] = move;
}

void AIPerplex::clear_history() noexcept
{
	std::memset(history_, 0, sizeof(history_));
}

void AIPerplex::age_history() noexcept
{
	// Halve all scores between iterative-deepening depths so that older
	// cutoff information fades rather than being discarded entirely.
	// Scores from deeper searches stay proportionally larger.
	for (auto& side : history_)
		for (auto& from : side)
			for (auto& score : from)
				score >>= 1;
}

void AIPerplex::update_history(eColor side, const Move& move, int depth) noexcept
{
	// Only quiet moves contribute to the history table
	if (MoveHelper::IsCapture(move))
		return;
	// Bonus scales with depth^2 so deep cutoffs outweigh shallow ones
	int32_t& entry = history_[side][move.from()][move.to()];
	entry += depth * depth;
	// Cap to avoid int32 overflow after many iterations
	if (entry > HISTORY_MAX)
		entry = HISTORY_MAX;
}



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
		pv_line += pv_table.get_line(0)[i].Output();
	}
	if (metrics.pv_length > display_length) {
		pv_line += " ...";
	}

	s_logger->debug(
		"D{:>2} EVAL: move={:<8} score={:>6} (Δ{:>+5}) nodes={:>8} ({:>3}%) "
		"pv={:>2} int={} chg={} PV:[{}]",
		metrics.depth,
		metrics.current_move.is_null() ? "EMPTY" : metrics.current_move.Output(),
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
			depth, state.last_iteration_move.Output(),
			metrics.current_move.Output(), state.depth_completed);
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

bool AIPerplex::handle_empty_move_emergency(
	SearchState& state,
	PVTable& pv_table)
{
	auto& log = *spdlog::default_logger();

	// Check if mate/stalemate
	if (std::abs(state.best_score) >= GameValues::Mate_Threshold) {
		log.info("No move needed - mate detected (score={})", state.best_score);
		return false;
	}

	// Check game state
	const GameInfo& current_info = m_Board.GetGameInfo();
	if (current_info.gameState != GameStates::STILL_PLAYING) {
		log.info("No move needed - game over (state={})",
			static_cast<int>(current_info.gameState));
		return false;
	}

	// True emergency - generate any legal move
	log.critical("EMERGENCY: No best move found (max_depth={}, last_completed={})",
		max_depth_, state.depth_completed);

	MoveList emergency_moves;
	MoveGenerator::ComputeLegalMoves(current_info, emergency_moves);

	if (emergency_moves.empty()) {
		log.critical("No legal moves - game is over");
		return false;
	}

	// Verify first move is legal
	if (!m_Board.DoMove(emergency_moves[0])) {
		log.critical("First pseudolegal move {} is illegal!",
			emergency_moves[0].Output());

		// Try others
		for (const auto& move : emergency_moves) {
			if (m_Board.DoMove(move)) {
				m_Board.UndoMove(move);
				state.best_move = move;
				state.best_score = 0;
				pv_table.update(0, move);

				log.critical("Using legal emergency move: {}", move.Output());
				return true;
			}
		}

		log.critical("No legal moves found - ComputeLegalMoves is broken!");
		return false;
	}

	// First move is legal
	m_Board.UndoMove(emergency_moves[0]);
	state.best_move = emergency_moves[0];
	state.best_score = 0;
	pv_table.update(0, emergency_moves[0]);

	log.critical("Using emergency move: {}", emergency_moves[0].Output());
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
		state.best_move.Output(),
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
		pv_line += pv_table.get_line(0)[i].Output();
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

void AIPerplex::debug_tt_cache_misses(unsigned int key, int ply)
{
	tt_misses.emplace(key, ply);
	if (tt_misses.size() % 2000 == 0)
	{
		ensure_logger_initialized();
		if (!s_logger) return;

		s_logger->info("Total inserts: {}", tt_misses.size());
		s_logger->info("Zobrist hash collisions (count > 1):");
		s_logger->info("===================================");

		int duplicateKeyCount = 0;
		size_t totalDuplicateEntries = 0;

		for (auto it = tt_misses.begin(); it != tt_misses.end(); ) {
			int64_t miss_key = it->first;
			size_t count = tt_misses.count(miss_key);

			if (count > 1) {
				duplicateKeyCount++;
				totalDuplicateEntries += count;

				s_logger->info("Hash: {} (collisions: {})", miss_key, count);
			}

			// Skip to next unique key
			it = tt_misses.upper_bound(miss_key);
		}

		s_logger->info("\nAnalysis Summary:");
		s_logger->info("  Total cache misses: {}", tt_misses.size());
		s_logger->info("  Unique zobrist hashes: {}", std::distance(tt_misses.begin(), tt_misses.end()));
		s_logger->info("  Collision groups (count > 1): {}", duplicateKeyCount);
		s_logger->info("  Total entries in collisions: {}", totalDuplicateEntries);
		double rate = 0.0;
		if (!tt_misses.empty()) rate = (totalDuplicateEntries * 100.0 / tt_misses.size());
		s_logger->info("  Collision rate: {}%", rate);
	}
}

void AIPerplex::assert_tt_store(const TranspositionTable& tt, std::uint64_t key, int16_t ply,
	[[maybe_unused]] int16_t value, [[maybe_unused]] int16_t depth, Move /*best_move*/,
	[[maybe_unused]] BoundType bound, NodeType /*node_type*/, [[maybe_unused]] SearchPhase phase)
{
	auto verify = tt.probe(key, ply);

	assert(verify && "Probe failed after store");
	assert(verify->key == key && "Key mismatch");
	assert(verify->depth == depth && "Depth mismatch");
	assert(verify->bound == bound && "Bound mismatch");
	assert(verify->phase == phase && "Phase mismatch");
	assert(verify->value == value && "Value mismatch");
}