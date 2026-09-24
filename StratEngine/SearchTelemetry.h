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

// PROBE (#634 step 0, throwaway): moves a depth-2 frontier guard at 300 cp would skip. Counting only;
// the search is unchanged.
struct ExtFutProbeStats {
	static constexpr bool compiled = true;

	int64_t d2_nodes = 0;      // depth-2 nodes passing the node-level guards
	int64_t cand = 0;          // candidates LMP does not cover, i.e. actually searched
	int64_t cand_idx[3] = {};  // ... by legal index 1-3, 4-7, 8-11
	int64_t lmp_cov = 0;       // candidates LMP skips anyway
	int64_t beat_alpha = 0;    // searched candidates whose value exceeded alpha
	int64_t subtree = 0;       // main + qs nodes spent under searched candidates, outermost only
	int nesting = 0;           // not summed

	void add(const ExtFutProbeStats& o) noexcept
	{
		d2_nodes += o.d2_nodes;
		cand += o.cand;
		for (int i = 0; i < 3; ++i)
			cand_idx[i] += o.cand_idx[i];
		lmp_cov += o.lmp_cov;
		beat_alpha += o.beat_alpha;
		subtree += o.subtree;
	}

	template <class Sink> void append_info(Sink&& sink) const
	{
		sink("xfut d2nodes " + std::to_string(d2_nodes) + " cand " + std::to_string(cand) + " idx " +
		     std::to_string(cand_idx[0]) + "/" + std::to_string(cand_idx[1]) + "/" + std::to_string(cand_idx[2]) +
		     " lmpcov " + std::to_string(lmp_cov) + " beat " + std::to_string(beat_alpha) + " subtree " +
		     std::to_string(subtree));
	}
};

struct SearchTelemetry {
	// Member order is a layout requirement: with singular first, the two counters live in the
	// shipping build keep the offsets in ThreadData they had as loose members.
	SingularStats singular{};
	FrontierFutilityStats frontier{};
	LateMovePruningStats lmp{};
	TTStats tt{};
	ExtFutProbeStats xfut{};

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
		xfut = ExtFutProbeStats{};
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
		xfut.add(other.xfut);
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
		xfut.append_info(sink);
	}
};
