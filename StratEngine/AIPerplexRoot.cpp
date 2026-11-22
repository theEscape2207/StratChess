// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "Common.h"
#include "AIPerplex.h"
#include "MoveFormatter.h"
#include "MoveGenerator.h"
#include "PVIntegrity.h"
#include <spdlog/common.h>
#include <spdlog/logger.h>

// The AIPerplex root driver: Search() and its Lazy SMP helper spawn, iterative deepening, the
// multi-PV root set, aspiration windows and the empty-move emergency. Node search (pvs(),
// quiescence()) and its per-node limit polling stay in AIPerplex.cpp.

namespace {
	template <typename Function> class ScopeExit final {
	  public:
		explicit ScopeExit(Function function) noexcept : function_(std::move(function)) {}
		~ScopeExit() noexcept { function_(); }

		ScopeExit(const ScopeExit&) = delete;
		ScopeExit& operator=(const ScopeExit&) = delete;

	  private:
		Function function_;
	};
} // namespace

SearchResult AIPerplex::Search(const Board& root, const SearchLimits& limits, IterationObserver observer)
{
	const IterationObserver search_observer = std::move(observer);
	{
		const std::lock_guard<std::mutex> lock(stop_mutex_);
		// Direct callers have no launch phase. They begin a fresh search here;
		// StartAsync() has already armed the same handshake before starting its thread.
		if (!search_launch_active_) {
			stop_pending_ = false;
			search_launch_active_ = true;
		}
	}
	// Destruction order is intentional on every exceptional path: stop_guard
	// latches the shared control first, helper jthreads then join, the boards'
	// prefetch targets are reset, and only then launch_guard clears the
	// immediate-stop handshake for the next call.
	const auto launch_guard = ScopeExit([this]() noexcept { finish_search_launch(); });
	// After the joins, so no board still searching can lose its target, and on every exit, so no
	// board outlives the search naming its table. A stale prefetch never faults, so nothing else
	// would notice: SetHash() may free the table once Search() returns.
	const auto prefetch_guard = ScopeExit([this]() noexcept { reset_prefetch_targets(); });
	std::vector<std::jthread> helpers;
	const auto stop_guard = ScopeExit([this]() noexcept { control_.Stop(); });

	init_search(root);

	// Establish this search's contempt context before anything can probe or store under it, and
	// before helpers exist to read root_color_. A draw score signed for one root colour, or scaled
	// by one contempt value, is not a valid bound for a search under another, and the Zobrist key
	// records neither — so entries produced under a different pair are dropped rather than trusted.
	//
	// Both halves of the second test are load-bearing. Only a search with non-zero contempt can
	// TINT an entry, and only one with non-zero contempt can MISREAD an untinted entry as if it
	// were tinted, so a differing pair matters only when at least one side of the change is
	// non-zero. Without that guard a process that never leaves the shipped default would clear
	// the table on an ordinary colour change, discarding entries no contempt could have tinted.
	//
	// The COLOUR half is the production-reachable one: SetTuning() already clears the table and the
	// context on any tuning change, so a contempt magnitude can only change through a path that has
	// just emptied the table. The pair carries the magnitude anyway, as defence in depth against a
	// future path that mutates tuning without going through SetTuning().
	root_color_ = root.GetCurrentColor();

	// A position the evaluator settles as drawn without computing a term is a draw the side to
	// move is choosing, exactly as a repetition is, so it carries the same contempt. Tinting only
	// the search-detected half would make the engine prefer a dead ending to a repetition — a
	// draw it can never come back from — which is a gradient pointing the wrong way rather than
	// merely an inconsistent score.
	//
	// Here, and not per evaluation, for two reasons: it is the last point at which root_color_ is
	// known and no helper thread exists yet, and a guard on the per-evaluation path costs nps
	// at the shipped default. At contempt 0 both values are GameValues::Draw.
	publish_draw_scores();

	// Snapshot threads_ exactly once so helper allocation, spawning and
	// aggregation use one internally consistent value. This is not race
	// synchronization: callers must obey the documented precondition that
	// configuration and StartNewGame() do not overlap Search(); UCI enforces it
	// by refusing setoption while a search is active.
	const unsigned threads = threads_;
	control_.ApplyLimits(limits);
	{
		const std::lock_guard<std::mutex> lock(stop_mutex_);
		if (stop_pending_)
			control_.Stop();
	}

	// AFTER ApplyLimits, so the memset a large table costs is charged to this move's budget rather
	// than spent before the clock starts. It only has to happen before anything probes or stores,
	// and nothing between here and iterative_deepening() does.
	const auto contempt_context = std::make_pair(root_color_, tuning_.contempt);
	if (tt_contempt_context_ && *tt_contempt_context_ != contempt_context &&
	    (tt_contempt_context_->second != 0 || tuning_.contempt != 0)) {
		(void)tt_->clear();
	}
	tt_contempt_context_ = contempt_context;
	const unsigned effective_depth = control_.EffectiveDepth();
	const uint8_t search_start_age = tt_->currentAge();
	// Establish this search's age before helpers can store, so every entry produced
	// by this search is newer than the snapshot hashfull uses to reject stale content.
	tt_->newSearch();
	if constexpr (kTTStatsCompiled)
		tt_->setStatsSearchStartAge(search_start_age);

	// Everything the helpers read unsynchronized must already be written HERE: root_color_, the
	// tuning_ block and the evaluator's drawn scores. publish_draw_scores() above is the one that
	// is easy to move by accident, and moving it below this block is a data race, not a stale read.
	//
	// Lazy SMP: spawn threads_ - 1 helper threads to warm the shared TT while
	// the main search below runs on td_ (main-is-authoritative: helpers never
	// report a move, only their node counts feed back in).
	// threads_ == 1 (the default) leaves this block entirely unreached:
	// `helpers` stays a default-constructed empty vector and helper_tds_ is
	// neither allocated nor seeded.
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
			htd.board = root; // same seed as td_.board
			htd.board.SetPrefetchTarget(tt_->prefetch_target());
			htd.nodes_searched = 0;
			htd.qnodes_searched = 0;
			htd.telemetry.reset();
			htd.root_game_state = GameStates::STILL_PLAYING;
			helpers.emplace_back([this, &htd, effective_depth, this_tt = tt_.get()] {
				helper_loop(htd, static_cast<int>(effective_depth), *this_tt);
			});
		}
	}

	SearchResult result =
	    iterative_deepening(td_, static_cast<int>(effective_depth), *tt_, search_start_age, search_observer);
	result.game_state = td_.root_game_state;

	// Latch the abort signal so any still-running helpers collapse in O(depth)
	// steps (they only ever poll IsAborted()), then join them — helpers.clear()
	// destroys each std::jthread, which joins automatically. This must happen
	// before every subsequent return in this function, including the
	// empty-move emergency path below, so a helper can never outlive Search().
	control_.Stop();
	helpers.clear();

	int64_t total_nodes = td_.nodes_searched;
	int64_t total_qnodes = td_.qnodes_searched;
	// Telemetry is summed the same way as the node counters, and for the same reason: a helper
	// thread's triggers are work this search did, whether or not its result was the one reported.
	// Bounded by the threads snapshot, never helper_tds_.size(): helper_tds_ is not shrunk when
	// Threads drops, and a stale helper still holds its last search's counters.
	result.telemetry.add(td_.telemetry);
	for (size_t i = 0; i + 1 < static_cast<size_t>(threads); ++i) {
		total_nodes += helper_tds_[i]->nodes_searched;
		total_qnodes += helper_tds_[i]->qnodes_searched;
		result.telemetry.add(helper_tds_[i]->telemetry);
	}
	result.nodes_searched = total_nodes;
	result.qnodes_searched = total_qnodes;
	result.hashfull = tt_->hashfull(search_start_age);

	result.elapsed = control_.Elapsed();
	// Game over at the root: no move to play, and result.game_state carries why.
	if (result.best_move.is_null())
		return result;

	log_search_result(result);
	return result;
}

SearchResult AIPerplex::iterative_deepening(ThreadData& td, int max_depth, TranspositionTable& tt,
                                            uint8_t search_start_age, const IterationObserver& observer)
{
	Engine::IterationState state;
	const Engine::IterationThresholds thresholds{tuning_.min_nodes_threshold, tuning_.min_completion_ratio,
	                                             tuning_.min_pv_ratio};
	td.nodes_since_check = 0; // reset node counter for this search

	td.begin_search(tuning_.continuation_history_plies > 0);

	// Multi-PV: the root moves each depth ranks. 1 is the plain search, unchanged.
	const int rank_count = root_rank_count(td);
	// The last completed set, sorted best first: rank k's aspiration seed and ordering hint.
	std::vector<RootLine> previous_set;
	std::vector<RootLine> current_set;

	// Copied on acceptance: a later rejected iteration rewrites row 0 but not best_move.
	// So is seldepth, which the next iteration resets.
	std::vector<Move> accepted_pv;
	std::vector<RootLine> accepted_alternatives;
	int accepted_seldepth = 0;
	const auto accept_root_pv = [&td, &accepted_pv, &accepted_alternatives,
	                             &accepted_seldepth](std::span<const RootLine> alternatives) {
		const auto& line = td.pv_table.get_line(0);
		accepted_pv.assign(line.begin(), line.begin() + td.pv_table.get_length(0));
		accepted_alternatives.assign(alternatives.begin(), alternatives.end());
		accepted_seldepth = td.seldepth;
	};

	for (int depth = 1; depth <= max_depth; ++depth) {

		// BEFORE ITERATION: Prepare for this depth's search. The TT age belongs to
		// the whole search and was set by Search().
		td.age_history();
		const int64_t nodes_at_start = td.nodes_searched;
		td.seldepth = 0;

		// EXECUTE SEARCH: This might get interrupted by timeout
		Engine::IterationSample sample{};
		std::span<const RootLine> alternatives;
		if (rank_count == 1) {
			int current_best_score;
			if (state.depth_completed == 0 || !tuning_.aspiration_enabled) {
				// Depth 1 or kill-switch: always full window (no reliable seed yet)
				current_best_score = pvs(td, depth, -GameValues::Search_Init, GameValues::Search_Init, 0, true, tt);
			} else {
				current_best_score = search_with_aspiration(td, depth, state.best_score, tt);
			}
			sample = {depth,
			          td.pv_table.get_pv_move(0),
			          current_best_score,
			          td.nodes_searched - nodes_at_start,
			          td.pv_table.get_length(0),
			          control_.StopRequested()};
		} else {
			// A depth counts only when every rank completed: its sorted rank 1 then drives the policy
			// exactly as a plain search's best move does, and the set is never interrupted, because
			// ranks from two depths would be sorted against each other. An incomplete set goes in with
			// a null move, the INCOMPLETE rejection, so the previous set stands.
			//
			// The one exception is the first-depth fallback: no set has completed, but rank 1 of depth 1
			// has. That is the plain search's own depth-1 result, so it is assessed as a completed
			// depth with no alternatives, rather than sent through the interrupted gates, whose node
			// floor a depth-1 set can never reach. Without it a tight limit would trade a searched
			// move for the emergency path's first legal one.
			const bool complete = search_root_set(td, depth, rank_count, previous_set, current_set, tt);
			const bool fallback = !complete && state.depth_completed == 0 && !current_set.empty();
			if (complete)
				alternatives = std::span<const RootLine>(current_set).subspan(1);
			else if (fallback)
				td.pv_table.set_line(0, current_set.front().pv);
			const bool usable = complete || fallback;
			sample = {depth,
			          usable ? current_set.front().move : Move::EmptyMove(),
			          usable ? current_set.front().score : 0,
			          td.nodes_searched - nodes_at_start,
			          usable ? static_cast<int>(current_set.front().pv.size()) : 0,
			          !usable};
		}

		const auto assessment = Engine::assess_iteration(sample, state, thresholds);
		const auto& metrics = assessment.metrics;
		log_iteration_eval(metrics, td.pv_table);

		bool continue_search = false;
		switch (assessment.decision) {
		case Engine::IterationDisposition::COMPLETED: {
			state = assessment.next_state;
			log_completed_iteration(metrics, td.pv_table);
			emit_iteration_info(td, state.depth_completed, state.best_score, search_start_age, alternatives, observer);
			accept_root_pv(alternatives);
			if (!alternatives.empty())
				previous_set.swap(current_set);

			// The observer can consume time or request Stop(); sample the soft limit after it.
			const int soft_percent = Engine::soft_limit_percent(state.move_run, tuning_.stability_time_floor);
			const auto continuation =
			    Engine::continue_iteration(assessment, control_.ShouldStopIteration(soft_percent));
			state = continuation.next_state;
			continue_search = continuation.stop_reason == Engine::IterationStopReason::NONE;
			if (continuation.stop_reason == Engine::IterationStopReason::MATE)
				log_mate_stop(depth);
			break;
		}
		case Engine::IterationDisposition::ACCEPTED_INTERRUPTED:
			assert(rank_count == 1 && "a multi-PV set is never accepted interrupted");
			state = assessment.next_state;
			log_acceptance(metrics);
			emit_iteration_info(td, state.depth_completed, state.best_score, search_start_age, {}, observer);
			accept_root_pv({});
			break;
		case Engine::IterationDisposition::REJECTED:
			log_rejection(depth, assessment.reason, metrics, state);
			break;
		}

		if (!continue_search)
			break;
	}

	// No accepted move: either the game is over at the root, or an emergency move is found.
	const bool has_move = !state.best_move.is_null() || handle_empty_move_emergency(td, state);
	if (has_move && accepted_pv.empty())
		accept_root_pv({}); // the emergency move: its row 0 is that one move
	assert(accepted_pv.empty() ? state.best_move.is_null() : accepted_pv.front() == state.best_move);
	assert(pv_replays_legally(td.board, accepted_pv));
	if (has_move && state.depth_completed > 0)
		log_search_complete(state, td.pv_table);
	return SearchResult{.best_move = state.best_move,
	                    .best_score = state.best_score,
	                    .depth_completed = state.depth_completed,
	                    .nodes_searched = state.nodes_at_completed_depth,
	                    .search_was_stable = state.search_was_stable,
	                    .pv = std::move(accepted_pv),
	                    .seldepth = accepted_seldepth,
	                    .alternatives = std::move(accepted_alternatives)};
}

int AIPerplex::root_rank_count(ThreadData& td) const
{
	if (multipv_ == 1)
		return 1;
	MoveList moves;
	MoveGenerator::ComputeLegalMoves(td.board, moves);
	int legal = 0;
	for (const auto& move : moves) {
		if (td.board.DoMove(move)) {
			td.board.UndoMove(move);
			++legal;
		}
	}
	return std::max(1, std::min(static_cast<int>(multipv_), legal));
}

// The multi-PV loop. Each rank is a full root search through the plain search's own window
// logic, excluding the moves already ranked; see pvs()'s root exclusion frame. Rank k is seeded from
// the previous set's k-th score, the closest prior estimate for "the best move outside k - 1 better
// ones". Depth 1 and a mate-score seed search full window: a window around a mate score keeps failing
// until the retry cap opens it anyway.
//
// The completed set is sorted, and its rank 1 can differ from the move rank 1's search chose: search
// instability can score a move higher in its own full PV search, at rank k, than the null window rank
// 1 gave it. An unsorted set would give a pick policy a negative gap.
bool AIPerplex::search_root_set(ThreadData& td, int depth, int rank_count, const std::vector<RootLine>& previous_set,
                                std::vector<RootLine>& set, TranspositionTable& tt)
{
	set.clear();
	RootExclusionGuard exclusion(td);
	for (size_t rank = 0; rank < static_cast<size_t>(rank_count); ++rank) {
		const RootLine* const previous = rank < previous_set.size() ? &previous_set[rank] : nullptr;
		td.root_rank_hint = previous != nullptr ? previous->move : Move::EmptyMove();
		const bool full_window = previous == nullptr || !tuning_.aspiration_enabled ||
		                         std::abs(previous->score) >= GameValues::Mate_Threshold;
		const int score = full_window ? pvs(td, depth, -GameValues::Search_Init, GameValues::Search_Init, 0, true, tt)
		                              : search_with_aspiration(td, depth, previous->score, tt);
		// The latched abort, so a stop that landed after an out-of-window pvs() counts too.
		const int length = td.pv_table.get_length(0);
		// A rank that ran to the end has a line: its first move beat the full or final window.
		assert(control_.IsAborted() || length > 0);
		if (control_.IsAborted() || length == 0)
			return false;
		const auto& line = td.pv_table.get_line(0);
		set.push_back({.move = line[0], .score = score, .pv = std::vector<Move>(line.begin(), line.begin() + length)});
		exclusion.add(line[0]);
	}

	std::stable_sort(set.begin(), set.end(), [](const RootLine& a, const RootLine& b) { return a.score > b.score; });
#ifdef STRAT_ENABLE_TEST_ACCESS
	if (set.front().move != td.root_excluded[0])
		++multipv_rank1_resorted_;
#endif
	td.pv_table.set_line(0, set.front().pv);
	return true;
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
	AspirationStats& stats = td.telemetry.aspiration;
	++stats.iterations;

	for (int retry = 0;; ++retry) {
		if (control_.StopRequested()) {
			// Interrupt before entering pvs(): clear PV so iterative_deepening sees EmptyMove
			// and triggers INCOMPLETE rejection rather than accepting a stale PV as valid.
			td.pv_table.clear_ply(0);
			return score;
		}

		const int64_t nodes_before = td.nodes_searched + td.qnodes_searched;
		score = pvs(td, depth, alpha, beta, 0, true, tt);

		if (control_.StopRequested())
			return score;

		// In-window: accept the score
		if (score > alpha && score < beta)
			return score;

		const bool fail_low = score <= alpha;
		++(fail_low ? stats.fail_lows : stats.fail_highs);
		stats.fail_nodes += td.nodes_searched + td.qnodes_searched - nodes_before;

		// Safety fallback: open full window after max retries
		if (retry >= tuning_.aspiration_max_retries) {
			if (td.thread_id == 0)
				log_aspiration_full_window(depth, tuning_.aspiration_max_retries);
			if (!control_.StopRequested()) {
				++stats.full_windows;
				score = pvs(td, depth, -GameValues::Search_Init, GameValues::Search_Init, 0, true, tt);
			}
			return score;
		}

		// Widen on the failing side; double delta for the next potential miss.
		// Log after updating so the message shows the new window being tried.
		delta *= 2;
		if (fail_low) {
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

bool AIPerplex::handle_empty_move_emergency(ThreadData& td, Engine::IterationState& state)
{
	auto& log = *spdlog::default_logger();

	// Check if mate/stalemate
	if (std::abs(state.best_score) >= GameValues::Mate_Threshold) {
		log.info("No move needed - mate detected (score={})", state.best_score);
		return false;
	}

	// Check game state
	if (td.root_game_state != GameStates::STILL_PLAYING) {
		log.info("No move needed - game over (state={})", static_cast<int>(td.root_game_state));
		return false;
	}

	// True emergency - generate any legal move
	log.critical("EMERGENCY: No best move found (max_depth={}, last_completed={})", control_.EffectiveDepth(),
	             state.depth_completed);

	// PVTable::update copies row ply + 1 onto the end of row ply. Row 1 here holds whatever the
	// last subtree to reach ply 1 left behind — a different position entirely — so without this
	// the emergency move would be published with a tail spliced onto it, the same defect the
	// unwind guard removes from the search itself.
	td.pv_table.clear_ply(1);

	// The list is pseudo-legal, so DoMove() decides which move is playable.
	MoveList emergency_moves;
	MoveGenerator::ComputeLegalMoves(td.board, emergency_moves);
	for (const auto& move : emergency_moves) {
		if (!td.board.DoMove(move))
			continue;
		td.board.UndoMove(move);
		state.best_move = move;
		state.best_score = 0;
		td.pv_table.update(0, move);

		log.critical("Using emergency move: {}", MoveFormatter::ToCoord(move));
		return true;
	}

	log.critical("No legal moves - game is over");
	return false;
}
