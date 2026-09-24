#pragma once
#include "TTStats.h"
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

// PROBE (move-ordering quality, throwaway): where in the legal move order a pvs() fail-high came from.
struct OrderingProbeStats {
	int64_t cuts = 0;          // fail-high nodes, ply > 0
	int64_t idx[5] = {};       // cutoff legal index 0, 1, 2, 3-5, 6+
	int64_t idx_sum = 0;
	int64_t first_type[4] = {}; // index-0 cut by: hash, capture, killer, other quiet
	int64_t late_type[4] = {};  // index>0 cut by: hash, capture, killer, other quiet
	int64_t band_cuts[3] = {};  // depth 1-2, 3-6, 7+
	int64_t band_first[3] = {};
	int64_t hash_nodes = 0;     // fail-high nodes that had a hash move
	int64_t hash_first = 0;     // ... where the hash move made the cut
	int64_t fail_low = 0;       // nodes that searched moves and did not cut
	int64_t waste = 0;          // nodes spent before the cutting move at late-cut nodes, nesting-exclusive
	int64_t waste_band[3] = {}; // ... by the cut node's depth band
	int64_t lmr = 0;            // reduced searches
	int64_t lmr_nodes = 0;      // nodes inside reduced searches, outermost only
	int64_t lmr_research = 0;   // reduced searches that beat alpha and were re-searched
	int64_t lmr_confirmed = 0;  // ... whose full-depth re-search still beat alpha
	int64_t research_nodes = 0; // nodes inside those re-searches, outermost only
	int lmr_nesting = 0;        // not summed
	int research_nesting = 0;   // not summed

	void add(const OrderingProbeStats& o) noexcept
	{
		waste += o.waste;
		for (int i = 0; i < 3; ++i)
			waste_band[i] += o.waste_band[i];
		lmr += o.lmr;
		lmr_nodes += o.lmr_nodes;
		lmr_research += o.lmr_research;
		lmr_confirmed += o.lmr_confirmed;
		research_nodes += o.research_nodes;
		cuts += o.cuts;
		idx_sum += o.idx_sum;
		hash_nodes += o.hash_nodes;
		hash_first += o.hash_first;
		fail_low += o.fail_low;
		for (int i = 0; i < 5; ++i)
			idx[i] += o.idx[i];
		for (int i = 0; i < 4; ++i) {
			first_type[i] += o.first_type[i];
			late_type[i] += o.late_type[i];
		}
		for (int i = 0; i < 3; ++i) {
			band_cuts[i] += o.band_cuts[i];
			band_first[i] += o.band_first[i];
		}
	}

	template <class Sink> void append_info(Sink&& sink) const
	{
		auto j = [](const int64_t* a, int n) {
			std::string s;
			for (int i = 0; i < n; ++i)
				s += (i ? "/" : "") + std::to_string(a[i]);
			return s;
		};
		sink("ordprobe cuts " + std::to_string(cuts) + " idx " + j(idx, 5) + " idxsum " + std::to_string(idx_sum) +
		     " first " + j(first_type, 4) + " late " + j(late_type, 4) + " bandcuts " + j(band_cuts, 3) +
		     " bandfirst " + j(band_first, 3) + " hashnodes " + std::to_string(hash_nodes) + " hashfirst " +
		     std::to_string(hash_first) + " faillow " + std::to_string(fail_low) + " waste " + std::to_string(waste) +
		     " wasteband " + j(waste_band, 3) + " lmr " + std::to_string(lmr) + " lmrnodes " +
		     std::to_string(lmr_nodes) + " research " + std::to_string(lmr_research) + " confirmed " +
		     std::to_string(lmr_confirmed) + " researchnodes " + std::to_string(research_nodes));
	}
};

struct SearchTelemetry {
	// Member order is a layout requirement: with singular first, the two counters live in the
	// shipping build keep the offsets in ThreadData they had as loose members.
	SingularStats singular{};
	FrontierFutilityStats frontier{};
	LateMovePruningStats lmp{};
	TTStats tt{};
	OrderingProbeStats ord{};

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
		ord = OrderingProbeStats{};
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
		ord.add(other.ord);
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
		ord.append_info(sink);
	}
};
