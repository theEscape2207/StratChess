#pragma once
#include "Board.h"
#include "PVTable.h"
#include "MoveHelper.h"
#include "SearchTelemetry.h"
#include "Sort.h"
#include <algorithm>
#include <array>
#include <bitset>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>

// Per-thread search state for AIPerplex: each Lazy SMP helper thread owns one and runs the same
// search functions on it.
//
// Deliberately NOT in here:
//   - TranspositionTable  — shared across threads by design; passed explicitly
//   - SearchTuning        — read-only configuration
//   - time control        — SearchControl owned by AIPerplex
struct ThreadData {
	static constexpr int MAX_KILLERS = 2;
	static constexpr int32_t HISTORY_MAX = 16'384;
	// A quiet's score sums its history entry and two continuation entries.
	static_assert(3 * HISTORY_MAX < MoveSorter::kLosingCaptureTier, "quiet scores must stay below every other tier");
	static_assert(HISTORY_MAX <= INT16_MAX, "continuation entries are int16_t");

	static constexpr uint16_t kNoContinuation = 0xFFFF;
	using ContinuationHistory = std::array<std::array<int16_t, kPieceSquares>, kPieceSquares>;

	// Bit i: the move at sorted index i of a pvs() frame completed its child search.
	using SearchedMoves = std::bitset<MoveList::MAX_MOVES>;

	// Thread-local board copy — the search runs on this, not on the game board.
	// Copy-assigned from the Search() root at the start of every search.
	Board board;

	// Game outcome adjudicated at this thread's root (ply 0). Thread-local rather than a
	// single AIPerplex-level member because adjustScoreForGameState() runs on every Lazy
	// SMP helper thread, each writing its own root at ply 0 concurrently — a shared member
	// would be a data race.
	GameStates root_game_state = GameStates::STILL_PLAYING;

	// Thread-local main-tree node counter owned by AIPerplex. Counts legal pvs() move edges searched.
	// Keeping quiescence out is deliberate:
	// assess_iteration_quality() and completion_ratio are calibrated against main-tree size,
	// so folding it in here would change search behaviour, not just reporting.
	int64_t nodes_searched = 0;

	// Thread-local quiescence counter, same unit as nodes_searched: one per move edge
	// searched. The two sum without overlap — a quiescence root's incoming edge belongs to
	// the main tree — and are reported both together (UCI 'nodes') and apart (Run-Bench.ps1).
	int64_t qnodes_searched = 0;

	// Node-based SearchControl polling counter. Reset alongside nodes_searched. Only thread 0 counts
	// and calls the wall-clock check (poll_search_limits(), gated on thread_id == 0); helper threads
	// rely solely on the cheap atomic IsAborted() read instead.
	int64_t nodes_since_check_ = 0;

	// Thread-local principal variation.
	PVTable pv_table;

	// 0 for the main thread; only it polls the clock and logs.
	int thread_id = 0;

	// Killer move heuristic: two non-capture moves per ply that caused a beta cutoff.
	Move killers[MAX_PLY][MAX_KILLERS];

	// Null-move consecutive-pass guard: last_move_was_null[ply] is true when
	// the move that led to this ply was itself a null move. Indexed the same
	// way as killers; cleared at search start and reset immediately after
	// each null-move attempt completes (see AIPerplex::pvs()).
	bool last_move_was_null[MAX_PLY]{};

	// History heuristic for quiet moves, indexed by [side-to-move][from-square][to-square]: raised
	// for a quiet move that cut, lowered for the quiets searched before it. Can be negative, and
	// |entry| <= HISTORY_MAX always holds (see apply_history()).
	int32_t history[2][64][64];

	// cont_key[p] is continuation_index() of the move that led to ply p, or kNoContinuation after a
	// null move and at the root, whose game move's piece is not recorded. Written by pvs() only for a
	// move it actually searches, so a node at ply reads the opponent's last move at cont_key[ply] and
	// its own previous one at cont_key[ply - 1].
	uint16_t cont_key[MAX_PLY];

	// Continuation history: [row: a previous move][column: a quiet], same update rule and bound as
	// history. On the heap because 1.18 MB inline would overflow the stack of an AIPerplex
	// constructed as a local, as tests do. make_unique value-initialises it to zero.
	std::unique_ptr<ContinuationHistory> cont_history = std::make_unique<ContinuationHistory>();

	// --- Cold tail: singular exclusion and telemetry ---
	// Deliberately LAST. Everything above is touched on the hot path; these are not, and
	// inserting them higher shifted the offsets of the members that are.

	// excluded_move[ply] is the move a verification search at this ply must pretend does not
	// exist. Empty for every ordinary node, which is what every guard in pvs() tests against.
	// Indexed like killers.
	//
	// A verification search re-enters pvs() at the SAME ply as the frame that launched it,
	// so this is the only thing distinguishing the two — an exclusion frame must not probe
	// or store the transposition table, clear the PV row, try a null move, or adjudicate a
	// moveless position as mate. Set and restored by ExcludedMoveGuard, never by hand.
	//
	// Cold by construction: pvs() tests the enable flag before indexing this, so a build with
	// singular extensions off never touches the array.
	Move excluded_move[MAX_PLY];

	// Trigger counters, per thread; see SearchTelemetry.h. Reset per search, and they survive an
	// abort like the node counters.
	SearchTelemetry telemetry{};

	ThreadData()
	{
		clear_killers();
		clear_excluded_moves();
		clear_history();
		clear_continuation_keys();
	}

	void clear_killers() noexcept
	{
		for (auto& ply_killers : killers)
			for (auto& k : ply_killers)
				k = Move::EmptyMove();
	}

	void clear_null_move_flags() noexcept { std::memset(last_move_was_null, 0, sizeof(last_move_was_null)); }

	void clear_excluded_moves() noexcept
	{
		for (auto& m : excluded_move)
			m = Move::EmptyMove();
	}

	// Resets everything that must not leak into a new game. History is
	// deliberately aged, never cleared, WITHIN a game (see age_history())
	// -- this is what draws that line at the game boundary instead.
	// Killers and null-move flags are already cleared at the start of every
	// search by begin_search(), so clearing them again here is only for
	// the (harmless) case of something reading them before the new game's
	// first search runs. `board` is reset too even though every Search()
	// copy-assigns it fresh from the supplied root before searching: it
	// costs nothing and avoids a stale position sitting in thread-local
	// state between games.
	void reset_for_new_game()
	{
		board = Board();
		nodes_searched = 0;
		qnodes_searched = 0;
		telemetry.reset();
		nodes_since_check_ = 0;
		pv_table = PVTable();
		clear_killers();
		clear_null_move_flags();
		clear_excluded_moves();
		clear_history();
		clear_continuation_history();
		clear_continuation_keys();
	}

	void store_killer(int ply, const Move& move) noexcept
	{
		// Captures are not stored as killers
		if (MoveHelper::IsCapture(move))
			return;
		// Avoid storing the same move twice in slot 0
		if (killers[ply][0] == move)
			return;
		// Shift slot 0 to slot 1, then store new killer in slot 0
		killers[ply][1] = killers[ply][0];
		killers[ply][0] = move;
	}

	void clear_history() noexcept { std::memset(history, 0, sizeof(history)); }

	void age_history() noexcept
	{
		// Halve all scores between iterative-deepening depths so that older
		// cutoff information fades rather than being discarded entirely.
		// Scores from deeper searches stay proportionally larger. Division, not a shift: >> rounds
		// toward minus infinity, so a -1 would never decay.
		for (auto& side : history)
			for (auto& from : side)
				for (auto& score : from)
					score /= 2;
	}

	// Per-search reset of the ply-indexed state, with the continuation table's once-per-search ageing.
	void begin_search(bool age_continuation) noexcept
	{
		clear_killers();
		clear_null_move_flags();
		clear_continuation_keys();
		if (age_continuation)
			age_continuation_history();
	}

	void clear_continuation_keys() noexcept { std::fill(std::begin(cont_key), std::end(cont_key), kNoContinuation); }

	void clear_continuation_history() noexcept
	{
		for (auto& row : *cont_history)
			row.fill(0);
	}

	// Halved once per search, not before every iteration like history: a pass over 1.18 MB per
	// iteration is judged too costly. So within one search the sum weights this table more heavily
	// as depth rises, a tradeoff accepted rather than designed for.
	void age_continuation_history() noexcept
	{
		for (auto& row : *cont_history)
			for (auto& score : row)
				score = static_cast<int16_t>(score / 2);
	}

	// The rows for a node at ply, reading `plies` of them (0..2).
	ContinuationRows continuation_rows(int ply, int plies) noexcept
	{
		assert(ply >= 0 && ply < MAX_PLY && plies >= 0 && plies <= 2);
		ContinuationRows rows;
		if (plies >= 1 && cont_key[ply] != kNoContinuation)
			rows.one_ply = continuation_row(cont_key[ply]);
		if (plies >= 2 && ply >= 1 && cont_key[ply - 1] != kNoContinuation)
			rows.two_ply = continuation_row(cont_key[ply - 1]);
		return rows;
	}

	static bool is_quiet(const Move& move) noexcept
	{
		return !MoveHelper::IsCapture(move) && !MoveHelper::IsPromote(move);
	}

	// Bonus for a quiet move that caused a beta cutoff; other moves are ignored. board must hold the
	// position the move is played from whenever a continuation row is given.
	void update_history(eColor side, const Move& move, int depth, ContinuationRows rows = {}) noexcept
	{
		if (is_quiet(move))
			apply_quiet_delta(side, move, history_bonus(depth), rows);
	}

	// Malus for a quiet move that was searched and failed to cut; other moves are ignored.
	void penalize_history(eColor side, const Move& move, int depth, ContinuationRows rows = {}) noexcept
	{
		if (is_quiet(move))
			apply_quiet_delta(side, move, -history_bonus(depth), rows);
	}

	// At a cutoff by the quiet move at sorted index cut_index, penalizes every quiet move sorted
	// before it that completed its child search. A move skipped by pruning, rejected as illegal
	// or excluded has no bit set: it was judged, not tried.
	void penalize_searched_quiets(eColor side, const MoveList& moveList,
	                              const std::array<std::pair<int, int>, MoveList::MAX_MOVES>& scored_idx,
	                              const SearchedMoves& searched, int cut_index, int depth,
	                              ContinuationRows rows = {}) noexcept
	{
		assert(cut_index >= 0 && cut_index < static_cast<int>(MoveList::MAX_MOVES));
		for (int i = 0; i < cut_index; ++i)
			if (searched[static_cast<size_t>(i)])
				penalize_history(side, moveList[scored_idx[static_cast<size_t>(i)].second], depth, rows);
	}

	// Threefold repetition and the fifty-move rule (thread-local board). Neither applies at
	// the root: the caller asked for a move, not an adjudication, and a draw returned there
	// leaves the search with nothing to report but the emergency move.
	bool check_draws(int ply) const noexcept
	{
		if (ply == 0)
			return false;
		return board.is_repetition(ply) || board.halfmove_clock() >= HALFMOVE_CLOCK_LIMIT;
	}

	// Updates the game state adjudicated at the root of the search tree.
	void update_game_state(size_t ply, GameStates newState)
	{
		if (ply == 0)
			root_game_state = newState;
	}

  private:
	// depth^2, so deep cutoffs outweigh shallow ones. Clamped so that |delta| <= HISTORY_MAX, which
	// apply_history()'s bound rests on, whatever depth search reaches.
	static int32_t history_bonus(int depth) noexcept { return std::min(depth * depth, HISTORY_MAX); }

	// Gravity update: an entry near the bound moves less, so the table keeps ranking good moves
	// instead of saturating at a cap. With |delta| and |entry| <= HISTORY_MAX the result stays in
	// [-HISTORY_MAX, HISTORY_MAX], and entry * |delta| <= 2^28 cannot overflow in int32_t, whatever
	// the entry's own type.
	template <typename Entry> static void apply_history(Entry& entry, int32_t delta) noexcept
	{
		assert(delta >= -HISTORY_MAX && delta <= HISTORY_MAX);
		const int32_t value = entry;
		entry = static_cast<Entry>(value + delta - value * (delta < 0 ? -delta : delta) / HISTORY_MAX);
	}

	int16_t* continuation_row(uint16_t key) noexcept
	{
		assert(key < kPieceSquares);
		return (*cont_history)[key].data();
	}

	void apply_quiet_delta(eColor side, const Move& move, int32_t delta, ContinuationRows rows) noexcept
	{
		apply_history(history[side][move.from()][move.to()], delta);
		if (rows.empty())
			return;
		const int col = MoveSorter::QuietContinuationColumn(board, move);
		if (rows.one_ply != nullptr)
			apply_history(rows.one_ply[col], delta);
		if (rows.two_ply != nullptr)
			apply_history(rows.two_ply[col], delta);
	}
};

// Sets td.excluded_move[ply] for the duration of a singular verification search and restores
// it on every exit. RAII rather than a set/call/clear sequence because the verification can
// return through the abort path: a slot left populated would silently disable the
// transposition table and null-move pruning for every later node at that ply, with no
// symptom beyond a slower search.
class ExcludedMoveGuard {
  public:
	ExcludedMoveGuard(ThreadData& td, int ply, const Move& move) noexcept
	    : td_(td), ply_(ply), previous_(td.excluded_move[ply])
	{
		assert(ply >= 0 && ply < MAX_PLY);
		assert(td.excluded_move[ply] == Move::EmptyMove() && "nested verification at one ply");
		td_.excluded_move[ply_] = move;
	}
	// Restores the PREVIOUS value, not Empty. The two coincide while nesting at one ply is
	// unreachable, and the assert above catches nesting in Debug -- but in Release an inner guard
	// clearing an outer one's slot would leave the outer frame searching the excluded move while
	// no longer recognising itself as an exclusion frame, and it would then store that partial
	// search to the transposition table under the position's own key. One Move member closes
	// that structurally instead of by argument.
	~ExcludedMoveGuard() noexcept { td_.excluded_move[ply_] = previous_; }

	ExcludedMoveGuard(const ExcludedMoveGuard&) = delete;
	ExcludedMoveGuard& operator=(const ExcludedMoveGuard&) = delete;
	ExcludedMoveGuard(ExcludedMoveGuard&&) = delete;
	ExcludedMoveGuard& operator=(ExcludedMoveGuard&&) = delete;

  private:
	ThreadData& td_;
	int ply_;
	Move previous_;
};

// Marks a singular verification search as an expected all-node for the search profile, and restores
// the parent's type on every exit: the verification re-enters pvs() at its parent's ply, so it shares
// the parent's slot. Does nothing unless the profile is compiled.
class VerificationNodeTypeGuard {
  public:
	VerificationNodeTypeGuard(ThreadData& td, int ply) noexcept : td_(td), ply_(static_cast<size_t>(ply))
	{
		assert(ply >= 0 && ply < MAX_PLY);
		if constexpr (kSearchProfileCompiled) {
			previous_ = td_.telemetry.nodetypes.expected[ply_];
			td_.telemetry.nodetypes.expected[ply_] = NodeTypeStats::All;
		}
	}
	~VerificationNodeTypeGuard() noexcept
	{
		if constexpr (kSearchProfileCompiled)
			td_.telemetry.nodetypes.expected[ply_] = previous_;
	}

	VerificationNodeTypeGuard(const VerificationNodeTypeGuard&) = delete;
	VerificationNodeTypeGuard& operator=(const VerificationNodeTypeGuard&) = delete;
	VerificationNodeTypeGuard(VerificationNodeTypeGuard&&) = delete;
	VerificationNodeTypeGuard& operator=(VerificationNodeTypeGuard&&) = delete;

  private:
	ThreadData& td_;
	size_t ply_;
	NodeTypeStats::Expected previous_ = NodeTypeStats::Pv;
};
