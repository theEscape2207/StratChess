// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "Common.h"
#include "AIPerplex.h"
#include "SearchTuningSchema.h"
#include <new>

// AIPerplex construction, the go/stop launch handshake, configuration, per-game reset and the
// Lazy SMP helper entry point.

AIPerplex::AIPerplex(AIPerplexConfig config)
    : control_(config.default_depth, config.default_time), tuning_(config.tuning),
      threads_(std::clamp(config.threads, 1u, MAX_THREADS)), verbose_logging_(config.verbose_logging)
{
	if (const auto error = SearchTuningSchema::Validate(tuning_))
		throw std::invalid_argument("SearchTuning." + error->field + ": " + error->message);
	tt_ = std::make_unique<TranspositionTable>(std::clamp(config.hash_mb, MIN_HASH_MB, MAX_HASH_MB));
	init_logger();
}

AIPerplex::~AIPerplex() { StopAndWait(); }

void AIPerplex::arm_search_launch() noexcept
{
	const std::lock_guard<std::mutex> lock(stop_mutex_);
	stop_pending_ = false;
	search_launch_active_ = true;
}

void AIPerplex::Stop() noexcept
{
	{
		const std::lock_guard<std::mutex> lock(stop_mutex_);
		if (search_launch_active_)
			stop_pending_ = true;
	}
	control_.Stop();
}

void AIPerplex::finish_search_launch() noexcept
{
	const std::lock_guard<std::mutex> lock(stop_mutex_);
	search_launch_active_ = false;
	stop_pending_ = false;
}

bool AIPerplex::IsSearching() const noexcept
{
	const std::lock_guard<std::mutex> lock(stop_mutex_);
	return search_launch_active_;
}

// Join before arm: the handshake flags are not per launch, so a previous Search() still unwinding
// would clear a fresh arm through its launch_guard, losing a stop and misreporting IsSearching().
void AIPerplex::StartAsync(const Board& root, const SearchLimits& limits, IterationObserver observer,
                           CompletionHandler on_done)
{
	StopAndWait();
	arm_search_launch();
	try {
		launch_thread_ = std::jthread(
		    [this, root_copy = root, limits, observer = std::move(observer), on_done = std::move(on_done)]() mutable {
#ifdef STRAT_ENABLE_TEST_ACCESS
			    if (launch_barrier_)
				    launch_barrier_();
#endif
			    // No catch: an exception escaping Search() terminates, as a lost bestmove would be worse.
			    const SearchResult result = Search(root_copy, limits, std::move(observer));
			    // Search() has returned, so IsSearching() is already false here: a client reading the
			    // bestmove that on_done sends can issue its next position at once.
			    if (on_done) {
				    in_completion_handler_ = true;
				    on_done(result);
				    in_completion_handler_ = false;
			    }
		    });
	} catch (...) {
		// Otherwise the engine would refuse commands for the rest of the session.
		finish_search_launch();
		throw;
	}
}

void AIPerplex::StopAndWait()
{
	Stop();
	Wait();
}

void AIPerplex::Wait()
{
	assert_not_in_completion_handler();
	if (launch_thread_.joinable())
		launch_thread_.join();
}

// Callers must ensure no search is using tt_. Constructing the replacement
// before assigning it retains the old table if allocation fails.
AIPerplex::HashConfigurationResult AIPerplex::SetHash(unsigned mb) noexcept
{
	assert_not_in_completion_handler();
	const unsigned requested = std::clamp(mb, MIN_HASH_MB, MAX_HASH_MB);
	try {
		auto replacement = std::make_unique<TranspositionTable>(requested);
		const HashConfigurationResult result{true, requested, replacement->memory_mb(), replacement->bucket_count()};
		tt_ = std::move(replacement);
		return result;
	} catch (const std::bad_alloc&) {
		return {false, requested, 0, 0};
	}
}

std::optional<SearchTuningSchema::TuningError> AIPerplex::SetTuning(const SearchTuning& tuning)
{
	assert_not_in_completion_handler();
	if (auto error = SearchTuningSchema::Validate(tuning))
		return error;
	if (tuning != tuning_) {
		tuning_ = tuning;
		(void)tt_->clear();
		// The table is empty, so the contempt context describing its contents is stale. Without
		// this the next Search() would compare against a pair for entries that no longer exist and
		// clear an already-empty table. This is also why a contempt MAGNITUDE change cannot reach
		// Search() with a populated table by any supported route — the guard there covers it as
		// defence in depth, not as the live path.
		tt_contempt_context_.reset();
	}
	return std::nullopt;
}

// Resets every piece of per-game state so that a persisting AIPerplex is
// equivalent to a freshly-constructed one, without paying for a rebuild.
// What is deliberately NOT reset here:
//   - threads_    : caller configuration, not per-game state.
//   - tuning_     : caller configuration, not accumulated search state --
//                   CreatePlayer() applies game_settings.json's search_tuning
//                   while constructing this service, then starts its first
//                   game. Resetting tuning_ here would silently discard those
//                   configured overrides.
//   - the evaluator: its one member is the drawn score pair, which Search()
//                    rewrites before every search, so nothing accumulates
//                    across games (see the sharing contract in Eval.h).
//   - each thread's pawn_cache: every entry is a pure function of its key, so
//                    nothing in it can be stale across games (PawnCache.h).
void AIPerplex::StartNewGame()
{
	assert_not_in_completion_handler();
	(void)tt_->clear();
	td_.reset_for_new_game();
	// Lazily resized in Search() (`if (helper_tds_.size() < threads - 1)`),
	// so clearing it forces fresh ThreadData construction -- with the
	// correct thread_id reassigned -- on the next search, rather than
	// reusing helpers whose killers/history carry over from the last game.
	helper_tds_.clear();
	// The table just went away, so the contempt context it was filled under has to go with it —
	// otherwise the next search matches against a pair describing entries that no longer exist.
	tt_contempt_context_.reset();
	++game_generation_;
}

// Resets td_'s per-search state. Move-ordering state (killers, history) is
// handled inside iterative_deepening(); history deliberately survives across
// moves (aged, never cleared).
void AIPerplex::init_search(const Board& root)
{
	td_.board = root; // thread-local copy — the search runs on this
	// After the copy, which carries root's target. Search() resets it after the helpers join.
	td_.board.SetPrefetchTarget(tt_->prefetch_target());
	td_.nodes_searched = 0;
	td_.qnodes_searched = 0;
	// Per-call like the node counters: the reported trigger rate belongs to this search.
	td_.telemetry.reset();
	td_.pv_table = PVTable{}; // fresh PV for this call
	td_.root_game_state = GameStates::STILL_PLAYING;
}

// Every board the search bound goes back to the dummy target. Skips a helper slot a failed
// allocation left empty.
void AIPerplex::reset_prefetch_targets() noexcept
{
	td_.board.SetPrefetchTarget({});
	for (const auto& htd : helper_tds_)
		if (htd)
			htd->board.SetPrefetchTarget({});
}

// Lazy SMP helper thread entry point (plain iterative deepening, no quality
// gates — see the declaration comment in AIPerplex.h). Runs entirely on its
// own ThreadData; touches nothing shared except the TT (already thread-safe)
// and the atomic abort flag it reads via IsAborted(). Calls the same
// search_with_aspiration() aspiration-window logging as the main thread, but
// those log_* calls are gated on `td.thread_id == 0` inside
// search_with_aspiration(), so helper threads never actually log. Never
// reports a move — its result is discarded by design; only the main
// thread's search result is authoritative.
void AIPerplex::helper_loop(ThreadData& td, int max_depth, TranspositionTable& tt)
{
	// Helpers run plain N = 1 searches; only the main thread ranks root moves.
	assert(td.root_excluded_count == 0);
	// On the helper's own thread, so the helpers age their continuation tables in parallel.
	td.begin_search(tuning_.continuation_history_plies > 0);
	int seed_score = 0;
	for (int depth = 1; depth <= max_depth; ++depth) {
		if (control_.IsAborted())
			break;
		const int score = search_with_aspiration(td, depth, seed_score, tt);
		if (control_.IsAborted())
			break; // partial iteration — discard score
		seed_score = score;
	}
}
