// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "Common.h"
#include "AIPerplex.h"
#include "MoveFormatter.h"
#include "PVIntegrity.h"
#include "Utils/Logger.h"
#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

// AIPerplex diagnostics: the verbose logger and the IterationInfo observer feed. Nothing here
// decides a move; every log_* call is a no-op unless the service was built with verbose_logging.

namespace {
	// Lazy SMP thread-safety: s_logger's sinks
	// (stdout_color_sink_mt, basic_file_sink_mt — see ensure_logger_initialized()
	// below, and the equivalent _mt sinks in Utils/Logger.cpp's default/perf
	// loggers) are all spdlog "_mt" thread-safe variants, so concurrent log
	// calls from multiple threads would be safe at the sink level. That said,
	// s_logger itself and ensure_logger_initialized()'s lazy-init (a plain
	// `if (s_logger) return;` check, not a magic static) are NOT safe to race:
	// initialization only ever happens from init_logger(), called by the constructor during
	// single-threaded AIPerplex setup before any search or
	// helper thread starts. Helper threads run the same search_with_aspiration()
	// code path as the main thread and reach the same log_* call sites, but
	// those calls are gated on `td.thread_id == 0` (see search_with_aspiration()
	// in AIPerplexRoot.cpp), so only the main thread ever actually logs; s_logger remains
	// read-only (already-initialized-or-null) from every thread's perspective.
	// If a future revision lets helper threads log too, ensure_logger_initialized()
	// must be made safe to call concurrently (e.g. via std::call_once) first.
	std::shared_ptr<spdlog::logger> s_logger = nullptr;

	void ensure_logger_initialized()
	{
		// ensure the general default logger is initialized first (no-op if already)
		Engine::Logger::InitDefault();
		if (s_logger)
			return;

		// create AIPerplex specific logger if desired
		// Use spdlog directly via Engine::Logger utilities
		if (!Engine::Logger::GetLogger("AIPerplex")) {
			try {
				// add both console and file sinks (file sink keeps a record for diagnostics)
				const auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
				console_sink->set_level(spdlog::level::info);
				console_sink->set_pattern("%T.%e %^%l%$: %v");
				const auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/aiperplex.log", true);
				file_sink->set_level(spdlog::level::debug);
				file_sink->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

				s_logger =
				    std::make_shared<spdlog::logger>("AIPerplex", spdlog::sinks_init_list{console_sink, file_sink});

				spdlog::register_logger(s_logger);
				s_logger->set_level(spdlog::level::debug);
				s_logger->flush_on(spdlog::level::debug);
			} catch (...) { // NOLINT(bugprone-empty-catch) - best-effort logger init
			}
		}
	}

} // namespace

void AIPerplex::init_logger() noexcept
{
	try {
		if (verbose_logging_) {
			ensure_logger_initialized();
			if (s_logger)
				s_logger->set_level(spdlog::level::debug);
		}
	} catch (...) { // NOLINT(bugprone-empty-catch) - best-effort logger setup
	}
}

void AIPerplex::log_iteration_eval(const Engine::IterationMetrics& metrics, const PVTable& pv_table) const
{
	if (!verbose_logging_ || !s_logger)
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

void AIPerplex::log_rejection(int depth, Engine::RejectionReason reason, const Engine::IterationMetrics& metrics,
                              const Engine::IterationState& state) const
{
	if (!verbose_logging_ || !s_logger)
		return;

	switch (reason) {
	case Engine::RejectionReason::INCOMPLETE:
		s_logger->debug("Depth {:>2}: REJECTED[R1:INCOMPLETE] (nodes={}, move={}) - Using depth {}", depth,
		                metrics.nodes_searched, metrics.current_move.is_null() ? "EMPTY" : "ok", state.depth_completed);
		break;

	case Engine::RejectionReason::TOO_FEW_NODES:
		s_logger->debug("Depth {:>2}: REJECTED[R2:TOO_FEW_NODES] ({} = {:.0f}% of D{}) - using depth {}", depth,
		                metrics.nodes_searched, metrics.completion_ratio * 100, state.depth_completed,
		                state.depth_completed);
		break;

	case Engine::RejectionReason::SHORT_PV:
		s_logger->debug("Depth {:>2}: REJECTED[R3:SHORT_PV] (pv={} vs depth={}) - using depth {}", depth,
		                metrics.pv_length, depth, state.depth_completed);
		break;

	case Engine::RejectionReason::MOVE_CHANGED:
		s_logger->debug("Depth {:>2}: REJECTED[R4:MOVE_CHANGED] ({} → {}) - Using depth {}", depth,
		                MoveFormatter::ToCoord(state.last_iteration_move), MoveFormatter::ToCoord(metrics.current_move),
		                state.depth_completed);
		break;

	default:
		break;
	}
}

void AIPerplex::log_acceptance(const Engine::IterationMetrics& metrics) const
{
	if (!verbose_logging_ || !s_logger)
		return;

	// Noteworthy so Info
	s_logger->info("Depth {:>2}: ACCEPTED[INTERRUPTED] (nodes={}, score={}, pv={})", metrics.depth,
	               metrics.nodes_searched, metrics.current_score, metrics.pv_length);
}

void AIPerplex::log_search_complete(const Engine::IterationState& state, const PVTable& pv_table) const
{
	if (!verbose_logging_ || !s_logger)
		return;

	if (state.best_move.is_null() || pv_table.get_length(0) == 0) {
		return;
	}

	s_logger->info("Search complete: depth={}, score={}, move={}, nodes={}, stable={}", state.depth_completed,
	               state.best_score, MoveFormatter::ToCoord(state.best_move), state.nodes_at_completed_depth,
	               state.search_was_stable ? "yes" : "NO");
}

void AIPerplex::log_completed_iteration(const Engine::IterationMetrics& metrics, const PVTable& pv_table) const
{
	if (!verbose_logging_ || !s_logger)
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

void AIPerplex::emit_iteration_info(const ThreadData& td, int depth, int score, uint8_t search_start_age,
                                    std::span<const RootLine> alternatives, const IterationObserver& observer) const
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

	if (!observer)
		return;

	// Main-thread cumulative nodes; see IterationInfo for why this need not equal Search()'s
	// final total. A progress indicator, not a decision input.
	const int length = td.pv_table.get_length(0);
	const auto& line = td.pv_table.get_line(0);

	IterationInfo iter;
	iter.depth = depth;
	iter.seldepth = td.seldepth;
	iter.score = score;
	iter.nodes = td.nodes_searched + td.qnodes_searched;
	iter.hashfull = tt_->hashfull(search_start_age);
	iter.elapsed = control_.Elapsed();
	iter.pv.assign(line.begin(), line.begin() + length);
	iter.alternatives.assign(alternatives.begin(), alternatives.end());

	observer(iter);
}

void AIPerplex::log_aspiration_retry(int depth, int retry, int score, int alpha, int beta, bool fail_low) const
{
	if (!verbose_logging_ || !s_logger)
		return;

	s_logger->debug("D{:>2} ASPIRATION: {} retry {} score={:>6} new window=[{:>7},{:>7}]", depth,
	                fail_low ? "FAIL-LOW " : "FAIL-HIGH", retry, score, alpha, beta);
}

void AIPerplex::log_aspiration_full_window(int depth, int max_retries) const
{
	if (!verbose_logging_ || !s_logger)
		return;

	s_logger->debug("D{:>2} ASPIRATION: max retries ({}) reached, opening full window", depth, max_retries);
}

void AIPerplex::log_search_result(const SearchResult& result) const
{
	if (!verbose_logging_ || !s_logger)
		return;

	s_logger->info("GetMove complete: move={}, score={}, depth={}, time={}ms, nodes={}, stable={}",
	               MoveFormatter::ToCoord(result.best_move), result.best_score, result.depth_completed,
	               result.elapsed.count(), result.nodes_searched, result.search_was_stable ? "yes" : "NO");
}

void AIPerplex::log_mate_stop(int depth) const
{
	if (!verbose_logging_ || !s_logger)
		return;

	s_logger->info("Mate found at depth {}, stopping iteration", depth);
}
