#pragma once
#include "TTStats.h"
#include "defines.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// Singular extensions are compiled out unless a build asks for them. The end state is
// unconditional-on or deleted, so the shipping engine must not carry the cost of carrying them
// disabled: a runtime gate alone costs measurable nps for code that never executes.
//
// Set by CMake: -DSTRAT_SINGULAR_EXTENSIONS=ON for an experimental engine build. The test target
// always defines it, because the tests are what exercise the feature.
//
// Every use is `if constexpr` or the first term of a conjunction, never #ifdef. The discarded
// branch of an `if constexpr` in a non-template context is still parsed and type-checked, so the
// disabled code cannot rot -- which is the usual objection to preprocessor branches in a hot path.
#ifndef STRAT_SINGULAR_EXTENSIONS
#	define STRAT_SINGULAR_EXTENSIONS 0
#endif
inline constexpr bool kSingularExtensionsCompiled = STRAT_SINGULAR_EXTENSIONS != 0;

// Per-node search profile counters: move ordering, LMR, node types, null move, pruning, quiescence.
// MEASUREMENT ONLY, so a profile build stays node-identical to the shipping one, and
// Compare-SearchEquivalence.ps1 compares their lines only when both builds print them.
//
// Set by CMake: -DSTRAT_SEARCH_PROFILE=1. The test target always defines it. At 0 the engine runs no
// counting code; the members remain as cold bytes at the tail of ThreadData, because every write
// site is `if constexpr`, never #ifdef, and a discarded branch is still type-checked.
#ifndef STRAT_SEARCH_PROFILE
#	define STRAT_SEARCH_PROFILE 0
#endif
inline constexpr bool kSearchProfileCompiled = STRAT_SEARCH_PROFILE != 0;

// Search telemetry: how often each heuristic fired, per thread, reset per search and summed across
// threads by AIPerplex::Search(). Not the node counters: nodes_searched/qnodes_searched are the
// measurement contract and steer the search, while these change no decision.
//
// Each feature struct carries `compiled`, so SearchTelemetry's reset, sum and formatting skip a
// compiled-out feature in one place. The write sites in pvs()/quiescence() keep their own gates.
//
// Each struct formats its own `info string` payload (the text after the prefix), emitted only when
// it has something to say. Scripts and tests match the wording exactly: never reword one.

struct SingularStats {
	static constexpr bool compiled = kSingularExtensionsCompiled;

	int64_t eligible = 0;      // nodes passing the eligibility gate
	int64_t verifications = 0; // verification searches actually run
	int64_t extensions = 0;    // verifications that granted the extra ply

	// Node edges consumed INSIDE verification searches, measured across each verification call
	// rather than inferred. Without it, "verification is what costs" can only be argued by
	// dividing the total node growth by the verification count and calling the quotient a
	// per-verification cost -- which is an identity, not evidence, and cannot separate
	// verification from the deeper subtrees the extensions themselves produce.
	int64_t verification_nodes = 0;

	void add(const SingularStats& other) noexcept
	{
		eligible += other.eligible;
		verifications += other.verifications;
		extensions += other.extensions;
		verification_nodes += other.verification_nodes;
	}

	template <class Sink> void append_info(Sink&& sink) const
	{
		if (eligible != 0)
			sink("singular eligible " + std::to_string(eligible) + " verified " + std::to_string(verifications) +
			     " extended " + std::to_string(extensions) + " verifynodes " + std::to_string(verification_nodes));
	}
};

// Moves frontier futility skipped. A work counter like nodes_searched, so it survives an abort.
// Neither a skipped move nor the quiescence entry it avoided is in either node count, so this is
// the only number that shows how often the guard fired. Run-Bench.ps1 parses `frontier skips`.
struct FrontierFutilityStats {
	static constexpr bool compiled = true;

	int64_t skips = 0;

	void add(const FrontierFutilityStats& other) noexcept { skips += other.skips; }

	template <class Sink> void append_info(Sink&& sink) const
	{
		if (skips != 0)
			sink("frontier skips " + std::to_string(skips));
	}
};

// Moves late move pruning skipped; the same kind of work counter, equally absent from the node
// counts.
struct LateMovePruningStats {
	static constexpr bool compiled = true;

	int64_t skips = 0;

	void add(const LateMovePruningStats& other) noexcept { skips += other.skips; }

	template <class Sink> void append_info(Sink&& sink) const
	{
		if (skips != 0)
			sink("lmp skips " + std::to_string(skips));
	}
};

// Aspiration windows. Written once per window rather than per node, so always compiled. A fail is a
// pvs() call that completed with a score outside its window; failnodes are the nodes, both trees,
// spent in those calls. The full-window fallback's own nodes are not in it.
struct AspirationStats {
	static constexpr bool compiled = true;

	int64_t iterations = 0; // iterations entered with an aspiration window, aborted ones included
	int64_t fail_lows = 0;
	int64_t fail_highs = 0;
	int64_t full_windows = 0; // retries exhausted, and the full-window search started
	int64_t fail_nodes = 0;

	void add(const AspirationStats& other) noexcept
	{
		iterations += other.iterations;
		fail_lows += other.fail_lows;
		fail_highs += other.fail_highs;
		full_windows += other.full_windows;
		fail_nodes += other.fail_nodes;
	}

	template <class Sink> void append_info(Sink&& sink) const
	{
		if (iterations != 0)
			sink("aspiration iterations " + std::to_string(iterations) + " faillow " + std::to_string(fail_lows) +
			     " failhigh " + std::to_string(fail_highs) + " fullwindow " + std::to_string(full_windows) +
			     " failnodes " + std::to_string(fail_nodes));
	}
};

// One slash-separated histogram token, "a/b/c". The bin count is part of each key's contract.
template <size_t N> std::string join_bins(const std::array<int64_t, N>& bins)
{
	std::string s = std::to_string(bins[0]);
	for (size_t i = 1; i < N; ++i)
		s += "/" + std::to_string(bins[i]);
	return s;
}

// Adds a histogram bin by bin.
template <size_t N> void add_bins(std::array<int64_t, N>& bins, const std::array<int64_t, N>& other) noexcept
{
	for (size_t i = 0; i < N; ++i)
		bins[i] += other[i];
}

// Depth bands shared by every banded profile field: depth 1-2, 3-6, 7+.
inline constexpr size_t kDepthBands = 3;
constexpr size_t depth_band(int depth) noexcept { return depth <= 2 ? 0 : depth <= 6 ? 1 : 2; }

// Where in the legal move order a pvs() fail-high came from. A cut is a fail-high node at ply > 0,
// singular verification frames excluded.
struct OrderingStats {
	static constexpr bool compiled = kSearchProfileCompiled;

	// What made a late cut (index > 0), tested in this order.
	enum CutMove : uint8_t { HashMove, CaptureOrPromotion, Killer, Quiet };

	int64_t cuts = 0;
	std::array<int64_t, 5> index{};    // legal index of the cutting move: 0, 1, 2, 3-5, 6+
	std::array<int64_t, 4> late_cut{}; // by CutMove
	int64_t hash_nodes = 0;            // cut nodes that had a hash move
	int64_t hash_cuts = 0;             // ... where the hash move made the cut

	// Nodes (both trees) spent on the moves searched before a late cut. Nesting-exclusive: a late-cut
	// node inside another's earlier moves is counted once, by the outer one.
	int64_t late_nodes = 0;
	std::array<int64_t, kDepthBands> late_bands{}; // late_nodes by the cut node's depth_band()

	void record_cut(int move_number, CutMove type, int depth, bool had_hash_move, int64_t spent_before) noexcept
	{
		cuts++;
		index[move_number <= 2 ? static_cast<size_t>(move_number) : move_number <= 5 ? 3 : 4]++;
		if (had_hash_move) {
			hash_nodes++;
			if (type == HashMove)
				hash_cuts++;
		}
		if (move_number > 0) {
			late_cut[type]++;
			late_nodes += spent_before;
			late_bands[depth_band(depth)] += spent_before;
		}
	}

	void add(const OrderingStats& other) noexcept
	{
		cuts += other.cuts;
		add_bins(index, other.index);
		add_bins(late_cut, other.late_cut);
		hash_nodes += other.hash_nodes;
		hash_cuts += other.hash_cuts;
		late_nodes += other.late_nodes;
		add_bins(late_bands, other.late_bands);
	}

	template <class Sink> void append_info(Sink&& sink) const
	{
		if (cuts != 0)
			sink("ordering cuts " + std::to_string(cuts) + " index " + join_bins(index) + " latecut " +
			     join_bins(late_cut) + " hashnodes " + std::to_string(hash_nodes) + " hashcuts " +
			     std::to_string(hash_cuts) + " latenodes " + std::to_string(late_nodes) + " latebands " +
			     join_bins(late_bands));
	}
};

// LMR-reduced searches and their full-depth re-searches. Node totals (both trees) count only the
// outermost search of each kind, so each is exclusive within its kind. A reduced search inside a
// re-search counts in both: never sum reducednodes and researchnodes.
struct LmrStats {
	static constexpr bool compiled = kSearchProfileCompiled;

	int64_t reduced = 0;
	int64_t reduced_nodes = 0;
	int64_t researched = 0; // reduced searches that beat alpha and ran again at full depth
	int64_t confirmed = 0;  // ... whose completed re-search still beat alpha
	int64_t research_nodes = 0;

	// Live nesting depth of each kind; per thread, never summed.
	int reduced_nesting = 0;
	int research_nesting = 0;

	void add(const LmrStats& other) noexcept
	{
		reduced += other.reduced;
		reduced_nodes += other.reduced_nodes;
		researched += other.researched;
		confirmed += other.confirmed;
		research_nodes += other.research_nodes;
	}

	template <class Sink> void append_info(Sink&& sink) const
	{
		if (reduced != 0)
			sink("lmr reduced " + std::to_string(reduced) + " reducednodes " + std::to_string(reduced_nodes) +
			     " researched " + std::to_string(researched) + " confirmed " + std::to_string(confirmed) +
			     " researchnodes " + std::to_string(research_nodes));
	}
};

// pvs() frames past the quiescence hand-off, by expected Knuth-Moore type and depth_band().
// A frame's type is PV when it is searched as one, else what its parent expected (see pvs()).
struct NodeTypeStats {
	static constexpr bool compiled = kSearchProfileCompiled;

	enum Expected : uint8_t { Pv, Cut, All };

	std::array<std::array<int64_t, kDepthBands>, 3> frames{}; // by Expected, then band
	std::array<int64_t, kDepthBands> cut_fail_low{};          // expected-cut frames that searched moves and failed low

	// Per thread, never summed: the type each ply's parent expects of it. Only non-PV frames read it.
	std::array<Expected, MAX_PLY> expected{};

	Expected type_of(int ply, bool is_pv_node) const noexcept
	{
		return is_pv_node ? Pv : expected[static_cast<size_t>(ply)];
	}

	void record_frame(int ply, bool is_pv_node, int depth) noexcept
	{
		frames[type_of(ply, is_pv_node)][depth_band(depth)]++;
	}

	void record_fail_low(int ply, bool is_pv_node, int depth) noexcept
	{
		if (type_of(ply, is_pv_node) == Cut)
			cut_fail_low[depth_band(depth)]++;
	}

	// Knuth-Moore, for a move searched with a null window: a cut node's first move is expected to
	// fail low, and every other move to cut. A child searched as PV ignores its slot.
	void expect_move_child(int ply, bool is_pv_node, int move_number) noexcept
	{
		expected[static_cast<size_t>(ply) + 1] = move_number == 0 && type_of(ply, is_pv_node) == Cut ? All : Cut;
	}

	// The pass's child is expected to fail low.
	void expect_null_child(int ply) noexcept { expected[static_cast<size_t>(ply) + 1] = All; }

	void add(const NodeTypeStats& other) noexcept
	{
		for (size_t type = 0; type < frames.size(); ++type)
			add_bins(frames[type], other.frames[type]);
		add_bins(cut_fail_low, other.cut_fail_low);
	}

	template <class Sink> void append_info(Sink&& sink) const
	{
		if (frames[Pv] != std::array<int64_t, kDepthBands>{})
			sink("nodetypes pv " + join_bins(frames[Pv]) + " cut " + join_bins(frames[Cut]) + " all " +
			     join_bins(frames[All]) + " cutfaillow " + join_bins(cut_fail_low));
	}
};

// Null-move attempts. An aborted attempt is in tried only, so cutoffs + failed <= tried. failnodes
// (both trees) is nesting-exclusive: a failed attempt inside another's subtree is counted once.
struct NullMoveStats {
	static constexpr bool compiled = kSearchProfileCompiled;

	int64_t tried = 0;
	int64_t cutoffs = 0;
	int64_t failed = 0; // completed below beta
	int64_t fail_nodes = 0;

	// spent is the attempt's node span; fail_nodes_before, fail_nodes when it began. Nested failures
	// already added their own nodes, so only the rest of the span is new.
	void record_failed(int64_t spent, int64_t fail_nodes_before) noexcept
	{
		failed++;
		fail_nodes += spent - (fail_nodes - fail_nodes_before);
	}

	void add(const NullMoveStats& other) noexcept
	{
		tried += other.tried;
		cutoffs += other.cutoffs;
		failed += other.failed;
		fail_nodes += other.fail_nodes;
	}

	template <class Sink> void append_info(Sink&& sink) const
	{
		if (tried != 0)
			sink("nullmove tried " + std::to_string(tried) + " cutoffs " + std::to_string(cutoffs) + " failed " +
			     std::to_string(failed) + " failnodes " + std::to_string(fail_nodes));
	}
};

// Reverse-futility cutoffs by the node's depth, 1 to 5 and 6+ (the depth limit is tunable), and
// frontier fail-low floors that raised a searched node's best value. Prints when either pruner fired:
// a node without two non-pawn pieces is frontier-pruned but never reverse-futility pruned.
struct PruningStats {
	static constexpr bool compiled = kSearchProfileCompiled;

	std::array<int64_t, 6> rfp{};
	int64_t floor_binds = 0;

	void record_rfp(int depth) noexcept { rfp[static_cast<size_t>(std::min(depth, 6) - 1)]++; }

	void add(const PruningStats& other) noexcept
	{
		add_bins(rfp, other.rfp);
		floor_binds += other.floor_binds;
	}

	template <class Sink> void append_info(Sink&& sink) const
	{
		if (rfp != std::array<int64_t, 6>{} || floor_binds != 0)
			sink("pruning rfp " + join_bins(rfp) + " floorbinds " + std::to_string(floor_binds));
	}
};

// Quiescence: roots are pvs() calls handed over at depth <= 0; delta and see are pseudo-legal moves
// each pruner skipped; maxdepth is the deepest quiescence frame entered. The frame one past the budget
// only evaluates, so maxdepth exceeds QSEARCH_BUDGET + 1 only through in-check chains. maxdepth
// combines across threads by max, not sum.
struct QSearchStats {
	static constexpr bool compiled = kSearchProfileCompiled;

	int64_t roots = 0;
	int64_t delta = 0;
	int64_t see = 0;
	int64_t max_depth = 0;

	void add(const QSearchStats& other) noexcept
	{
		roots += other.roots;
		delta += other.delta;
		see += other.see;
		max_depth = std::max(max_depth, other.max_depth);
	}

	template <class Sink> void append_info(Sink&& sink) const
	{
		if (roots != 0)
			sink("qsearch roots " + std::to_string(roots) + " delta " + std::to_string(delta) + " see " +
			     std::to_string(see) + " maxdepth " + std::to_string(max_depth));
	}
};

struct SearchTelemetry {
	// Member order is a layout requirement: with singular first, the two counters live in the
	// shipping build keep the offsets in ThreadData they had as loose members. New members go last.
	SingularStats singular{};
	FrontierFutilityStats frontier{};
	LateMovePruningStats lmp{};
	TTStats tt{};
	AspirationStats aspiration{};
	OrderingStats ordering{};
	LmrStats lmr{};
	NodeTypeStats nodetypes{};
	NullMoveStats nullmove{};
	PruningStats pruning{};
	QSearchStats qsearch{};

	void reset() noexcept
	{
		if constexpr (SingularStats::compiled)
			singular = SingularStats{};
		if constexpr (FrontierFutilityStats::compiled)
			frontier = FrontierFutilityStats{};
		if constexpr (LateMovePruningStats::compiled)
			lmp = LateMovePruningStats{};
		if constexpr (TTStats::compiled)
			tt = TTStats{};
		if constexpr (AspirationStats::compiled)
			aspiration = AspirationStats{};
		if constexpr (OrderingStats::compiled)
			ordering = OrderingStats{};
		if constexpr (LmrStats::compiled)
			lmr = LmrStats{};
		if constexpr (NodeTypeStats::compiled)
			nodetypes = NodeTypeStats{};
		if constexpr (NullMoveStats::compiled)
			nullmove = NullMoveStats{};
		if constexpr (PruningStats::compiled)
			pruning = PruningStats{};
		if constexpr (QSearchStats::compiled)
			qsearch = QSearchStats{};
	}

	void add(const SearchTelemetry& other) noexcept
	{
		if constexpr (SingularStats::compiled)
			singular.add(other.singular);
		if constexpr (FrontierFutilityStats::compiled)
			frontier.add(other.frontier);
		if constexpr (LateMovePruningStats::compiled)
			lmp.add(other.lmp);
		if constexpr (TTStats::compiled)
			tt.add(other.tt);
		if constexpr (AspirationStats::compiled)
			aspiration.add(other.aspiration);
		if constexpr (OrderingStats::compiled)
			ordering.add(other.ordering);
		if constexpr (LmrStats::compiled)
			lmr.add(other.lmr);
		if constexpr (NodeTypeStats::compiled)
			nodetypes.add(other.nodetypes);
		if constexpr (NullMoveStats::compiled)
			nullmove.add(other.nullmove);
		if constexpr (PruningStats::compiled)
			pruning.add(other.pruning);
		if constexpr (QSearchStats::compiled)
			qsearch.add(other.qsearch);
	}

	// Calls sink(std::string) once per payload, in the order UCI reports them.
	template <class Sink> void append_info(Sink&& sink) const
	{
		if constexpr (SingularStats::compiled)
			singular.append_info(sink);
		if constexpr (FrontierFutilityStats::compiled)
			frontier.append_info(sink);
		if constexpr (LateMovePruningStats::compiled)
			lmp.append_info(sink);
		if constexpr (TTStats::compiled)
			tt.append_info(sink);
		if constexpr (AspirationStats::compiled)
			aspiration.append_info(sink);
		if constexpr (OrderingStats::compiled)
			ordering.append_info(sink);
		if constexpr (LmrStats::compiled)
			lmr.append_info(sink);
		if constexpr (NodeTypeStats::compiled)
			nodetypes.append_info(sink);
		if constexpr (NullMoveStats::compiled)
			nullmove.append_info(sink);
		if constexpr (PruningStats::compiled)
			pruning.append_info(sink);
		if constexpr (QSearchStats::compiled)
			qsearch.append_info(sink);
	}
};
