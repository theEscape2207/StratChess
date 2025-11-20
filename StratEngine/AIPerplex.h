#pragma once
#include <map>

#include "PlayerAiIterBase.h"

#include "TranspositionTable.h"

class Move;

class AIPerplex final
	: public PlayerAiIterBase
{
public:
	// Implementation/overrides of the IPlayer interface
	Move GetMove(_Inout_ GameInfo& info) override;
	const char* GetType() const noexcept override
	{
		return "Perplexity Transpositional AlphaBeta";
	}

	// Note: NOT to be called directly - only through Factory method (needed to be public due to usage of make_unique)
	explicit AIPerplex(_In_ unsigned md) : PlayerAiIterBase(md), _searchCount(0) {}
	~AIPerplex() = default;

	// Force use of factory by preventing constructor, copy-construction & operator=
	AIPerplex(const AIPerplex&) = delete;
	AIPerplex& operator=(const AIPerplex&) = delete;
	AIPerplex(AIPerplex&&) = delete;
	AIPerplex& operator=(AIPerplex&&) = delete;
private:
	// Perplex specific helper
	int iterative_deepening(int max_depth, TranspositionTable& tt, PVTable& pv_table);
	int pvs(int depth, int alpha, int beta, int ply, bool is_pv_node, TranspositionTable& tt, PVTable& pv_table);
	int adjustScoreForGameState(bool moveFound, int ply, int best_value);
	int quiescence(int alpha, int beta, int depth_q, int ply, TranspositionTable& tt);


	// logging control: enable detailed logging when needed (default: false)
	static inline bool s_verbose_logging = false;

public:
	// Configure logger verbosity at runtime (call before heavy runs if needed)
	static void SetVerboseLogging(bool enabled) noexcept { s_verbose_logging = enabled; }
	static bool IsVerboseLoggingEnabled() noexcept { return s_verbose_logging; }

	// for debugging
	void debug_tt_cache_misses(unsigned int key, int ply);
	void verify_tt_store(const TranspositionTable& tt, std::uint64_t key, int16_t ply,
		int16_t value, int16_t depth, Move best_move,
		BoundType bound, NodeType node_type, SearchPhase phase);
	std::multimap<std::uint64_t, int> tt_misses;
};

