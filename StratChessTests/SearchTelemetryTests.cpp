// SearchTelemetryTests.cpp — Catch2 tests for what a search reports rather than what it finds:
// the SetThreads clamp and the post-join aggregate, the terminal verdict a root with no move
// hands back, the per-call reset of that verdict carrier, the main/quiescence node counters and
// the fixed_nodes poll gate.

#include "SearchTestFixture.h"
#include <catch2/catch_test_macros.hpp>
#include "AIAgent.h"
#include "MoveFormatter.h"
#include "defines.h"
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// SetThreads clamp tests
// ============================================================================
// Verifies the [1, 32] clamp on threads_ in isolation.

TEST_CASE("SMP - SetThreads(0) clamps to 1", "[smp]")
{
	AIPerlexTestFixture fix;
	fix.ai->SetThreads(0);
	REQUIRE(fix.threads() == 1u);
}

TEST_CASE("SMP - SetThreads(64) clamps to 32", "[smp]")
{
	AIPerlexTestFixture fix;
	fix.ai->SetThreads(64);
	REQUIRE(fix.threads() == 32u);
}

TEST_CASE("SMP - SetThreads(4) passes through unchanged", "[smp]")
{
	AIPerlexTestFixture fix;
	fix.ai->SetThreads(4);
	REQUIRE(fix.threads() == 4u);
}

TEST_CASE("SMP - SetThreads default is 1", "[smp]")
{
	AIPerlexTestFixture fix;
	REQUIRE(fix.threads() == 1u);
}

// Search() returns the result AFTER helper threads are joined and their node counts folded in.
TEST_CASE("SMP - Search returns post-join aggregate telemetry at Threads > 1", "[smp]")
{
	AIPerlexTestFixture fix("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 6);

	const SearchResult returned = fix.get_move_at_threads(4, 6);

	REQUIRE_FALSE(returned.best_move.is_null());

	// Stated as the exact sum rather than an inequality:
	// a helper whose thread starts after the main search has already finished contributes a
	// legitimate zero, so "greater than the main thread's count" is not guaranteed on a loaded
	// or single-core machine. Without this, the equality checks above would pass on a pre-join
	// result.
	CHECK(returned.nodes_searched == fix.mainnodes() + fix.helper_nodes());
	CHECK(returned.qnodes_searched == fix.qnodes() + fix.helper_qnodes());
}

// Summed like the node counters: exactly the main thread plus every helper this search ran.
TEST_CASE("SMP - Search returns post-join aggregate trigger counters at Threads > 1", "[smp][telemetry]")
{
	AIPerlexTestFixture fix("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 6);

	const SearchResult returned = fix.get_move_at_threads(3, 6);
	REQUIRE_FALSE(returned.best_move.is_null());
	REQUIRE(fix.helper_count() == 2);

	const SearchTelemetry helpers = fix.all_helper_telemetry();
	REQUIRE(fix.frontier_skips() > 0);
	REQUIRE(fix.lmp_skips() > 0);
	CHECK(returned.telemetry.frontier.skips == fix.frontier_skips() + helpers.frontier.skips);
	CHECK(returned.telemetry.lmp.skips == fix.lmp_skips() + helpers.lmp.skips);
}

// helper_tds_ is not shrunk when Threads drops, so idle helpers still hold an earlier search's
// counters. The sum must stop at the current Threads, not at every allocated helper.
TEST_CASE("SMP - trigger counters exclude helpers the current Threads leaves idle", "[smp][telemetry]")
{
	AIPerlexTestFixture fix("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 6);

	SearchTelemetry stale;
	stale.frontier.skips = 1'000'000'000;
	stale.lmp.skips = 1'000'000'000;
	fix.add_stale_helper(stale);
	fix.add_stale_helper(stale);

	const SearchResult single = fix.get_move_at_threads(1, 6);
	REQUIRE_FALSE(single.best_move.is_null());

	CHECK(single.telemetry.frontier.skips == fix.frontier_skips());
	CHECK(single.telemetry.lmp.skips == fix.lmp_skips());
}

// The wording is parsed by Run-Bench.ps1, measure_tt_capacity.py and the strength lab.
TEST_CASE("SearchTelemetry - info string payloads keep their parsed wording and order", "[search][telemetry]")
{
	// The test binary compiles both gated features in, so every payload is reachable here.
	REQUIRE(kSingularExtensionsCompiled);
	REQUIRE(kTTStatsCompiled);

	const auto payloads_of = [](const SearchTelemetry& telemetry) {
		std::vector<std::string> payloads;
		telemetry.append_info([&payloads](const std::string& payload) { payloads.push_back(payload); });
		return payloads;
	};

	SearchTelemetry fired;
	fired.singular = {.eligible = 1, .verifications = 2, .extensions = 3, .verification_nodes = 4};
	fired.frontier.skips = 5;
	fired.lmp.skips = 6;
	fired.tt = {.main_probes = 1,
	            .main_hits = 2,
	            .main_cutoffs = 3,
	            .qs_probes = 4,
	            .qs_hits = 5,
	            .qs_cutoffs = 6,
	            .stores_declined = 7,
	            .stores_filled = 8,
	            .stores_refreshed = 9,
	            .evicted_stale = 10,
	            .evicted_current = 11};

	CHECK(payloads_of(fired) ==
	      std::vector<std::string>{"singular eligible 1 verified 2 extended 3 verifynodes 4", "frontier skips 5",
	                               "lmp skips 6",
	                               "ttstats mainprobes 1 mainhits 2 maincutoffs 3 qsprobes 4 qshits 5 qscutoffs 6 "
	                               "stores 45 declined 7 filled 8 refreshed 9 evictstale 10 evictcurrent 11"});

	// Singular, frontier and lmp stay silent when they did not fire; ttstats prints whenever compiled.
	CHECK(payloads_of(SearchTelemetry{}) ==
	      std::vector<std::string>{"ttstats mainprobes 0 mainhits 0 maincutoffs 0 qsprobes 0 qshits 0 qscutoffs 0 "
	                               "stores 0 declined 0 filled 0 refreshed 0 evictstale 0 evictcurrent 0"});
}

// Replacement charges a penalty per TT generation, so a per-depth advance would make earlier depths
// of the same search look stale and wrap the 8-bit age within a game.
TEST_CASE("AIPerplex - one Search() advances the TT age by exactly one generation", "[search][tt][smp]")
{
	AIPerlexTestFixture fix("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 64);

	SearchResult result;
	const auto advance_of = [&fix, &result](unsigned threads, const SearchLimits& limits) {
		fix.ai->SetThreads(threads);
		const uint8_t before = fix.tt_age();
		result = fix.ai->Search(fix.board_, limits);
		REQUIRE_FALSE(result.best_move.is_null());
		return static_cast<uint8_t>(fix.tt_age() - before);
	};

	CHECK(advance_of(1, SearchLimits::fixed_depth(6)) == 1);
	CHECK(advance_of(4, SearchLimits::fixed_depth(6)) == 1);
	// Stopped by the node poll partway through an iteration, after several completed depths.
	CHECK(advance_of(1, SearchLimits::fixed_nodes(20'000)) == 1);
	CHECK(result.nodes_searched + result.qnodes_searched >= 20'000);
	CHECK(result.depth_completed > 1);
}

TEST_CASE("AIPerplex - a completed result reports non-negative elapsed time", "[search]")
{
	// Catches GetMove() dropping the elapsed value from its completed SearchControl session.
	AIPerlexTestFixture fix;

	const SearchResult result = fix.get_move_at_threads(1, 1);

	CHECK(result.elapsed >= std::chrono::milliseconds::zero());
}

TEST_CASE("AIAgent - a completed result reports its unsplit legacy node count", "[search]")
{
	// Catches MakeResult() leaving the legacy count at the SearchResult default, or incorrectly
	// claiming that legacy nodes belong to the separately reported quiescence tree.
	LegacyAiTestFixture fix("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

	const SearchResult result = fix.get_move(3);

	REQUIRE(fix.search_count() > 0);
	CHECK(result.nodes_searched == fix.search_count());
	CHECK(result.qnodes_searched == 0);
}

// ============================================================================
// Terminal results at the root
// ============================================================================
// The producer half of the contract GameLoopTests.cpp exercises from the consumer side: asked
// for a move in a position that has none, a real player returns a null move and names the
// outcome in game_state. That is the only channel left — the state-changed event is gone — so
// nothing else reports the end of a game, and scripted players cannot prove a real one supplies
// it.

TEST_CASE("AIPerplex - a mated root returns no move and names the winner", "[search]")
{
	// Black to move and mated: Ra8 covers the back rank, f7/g7/h7 block every escape.
	AIPerlexTestFixture fix("R5k1/5ppp/8/8/8/8/8/6K1 b - - 0 1");

	const SearchResult result = fix.get_move_at_threads(1, 3);

	CHECK(result.best_move.is_null());
	CHECK(result.game_state == GameStates::WHITE_WON);
}

TEST_CASE("AIPerplex - a stalemated root returns no move and DRAW_PAT", "[search]")
{
	// Black to move, not in check, and every king move is covered: Qf7 takes g8, g7 and h7,
	// with Kg6 covering the last two a second time.
	AIPerlexTestFixture fix("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
	REQUIRE(fix.count_legal_moves() == 0);

	const SearchResult result = fix.get_move_at_threads(1, 3);

	CHECK(result.best_move.is_null());
	CHECK(result.game_state == GameStates::DRAW_PAT);
}

TEST_CASE("AIPerplex - a position with a move reports STILL_PLAYING", "[search]")
{
	// The control for both cases above: without it, a GetMove() that reported a terminal state
	// unconditionally would pass them.
	AIPerlexTestFixture fix("4k3/8/8/8/8/8/1R6/4K3 w - - 5 60");

	const SearchResult result = fix.get_move_at_threads(1, 3);

	CHECK_FALSE(result.best_move.is_null());
	CHECK(result.game_state == GameStates::STILL_PLAYING);
}

// The root verdict carrier is reset per GetMove() call, and an aborted search never writes it.
// Both halves matter: any search that completes a root frame overwrites the carrier anyway, so an
// abort is the only way a previous call's terminal verdict reaches the caller -- and a terminal
// verdict makes handle_empty_move_emergency() return no move at all.

// What one complete iteration at `depth` costs the poll gate on `fen`, with SEE pruning off for
// the reason given at BUSY_FEN.
static int64_t poll_ticks_at_depth(const std::string& fen, int depth)
{
	AIPerlexTestFixture probe(fen, static_cast<unsigned>(depth));
	probe.set_see_pruning(false);
	probe.search_to_depth(depth);
	return probe.poll_ticks();
}

// Kiwipete: 48 root moves, and enough hanging material that quiescence keeps going under most of
// them. Chosen because its depth-1 iteration costs more than one poll interval -- the only shape of
// position where a node limit can abort before any root frame has adjudicated.
//
// The tests below pin see_pruning_enabled off, because with it on this iteration costs 1008 ticks
// and no legal position in the repository's corpora is dearer. The flag is irrelevant to what they
// assert -- they are about the verdict carrier, not about pruning -- so pinning it stops a search
// that gets cheaper from silently converting these into tests of something else.
static constexpr const char* BUSY_FEN = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";
static constexpr int64_t POLL_INTERVAL = 1024; // poll_search_limits(): (++nodes_since_check_ & 1023)

TEST_CASE("AIPerplex - init_search resets the root verdict before any node runs", "[search]")
{
	AIPerlexTestFixture fix("4k3/8/8/8/8/8/1R6/4K3 w - - 5 60");

	fix.set_root_game_state(GameStates::BLACK_WON);
	fix.call_init_search();

	CHECK(fix.root_game_state() == GameStates::STILL_PLAYING);
}

TEST_CASE("AIPerplex - a search aborted inside its first root frame drops the previous verdict", "[search]")
{
	// Black to move and mated: this call leaves WHITE_WON in the carrier.
	AIPerlexTestFixture fix("R5k1/5ppp/8/8/8/8/8/6K1 b - - 0 1", /*max_depth=*/8);
	REQUIRE(fix.get_move_at_threads(1, 3).game_state == GameStates::WHITE_WON);

	// Asserted, not assumed: if depth 1 ever gets cheap enough to finish inside one poll interval,
	// the abort below would land after a root frame adjudicated and this test would pass for the
	// wrong reason. It goes red here instead.
	REQUIRE(poll_ticks_at_depth(BUSY_FEN, 1) > POLL_INTERVAL);

	REQUIRE(fix.board_.SetupFromFEN(BUSY_FEN));
	fix.set_see_pruning(false);
	const SearchResult aborted = fix.result_with_nodes(1);

	CHECK(aborted.game_state == GameStates::STILL_PLAYING);
	// The behaviour that depends on it: handle_empty_move_emergency() refuses to supply a move
	// when the carrier names a finished game, so a stale WHITE_WON here costs the engine its move.
	CHECK_FALSE(aborted.best_move.is_null());
}

TEST_CASE("AIAgent - ApplyLimits resets the root verdict before any node runs", "[search]")
{
	LegacyAiTestFixture fix("4k3/8/8/8/8/8/1R6/4K3 w - - 5 60");

	fix.set_root_game_state(GameStates::BLACK_WON);
	fix.call_apply_limits();

	CHECK(fix.root_game_state() == GameStates::STILL_PLAYING);
}

TEST_CASE("AIAgent - StopSearch latches the composed control for the legacy search guard", "[search_control]")
{
	LegacyAiTestFixture fix("4k3/8/8/8/8/8/1R6/4K3 w - - 5 60");

	fix.call_apply_limits();
	fix.stop_search();

	CHECK(fix.search_is_aborted());
}

TEST_CASE("AIAgent - a search aborted before its first root frame drops the previous verdict", "[search]")
{
	// One real search first, so m_Line is populated: GetBestMove() asserts on an empty line unless
	// the root was adjudicated, and the aborted call below never reaches Search().
	LegacyAiTestFixture fix("4k3/8/8/8/8/8/1R6/4K3 w - - 5 60");
	REQUIRE(fix.get_move(4).game_state == GameStates::STILL_PLAYING);

	// What a terminal call would have left behind.
	fix.set_root_game_state(GameStates::WHITE_WON);

	// Only game_state is checked: the leftover line makes the returned move two ply stale, which is
	// a legacy quirk of seeding each search from the last one, not what is under test.
	CHECK(fix.get_move_with_spent_clock().game_state == GameStates::STILL_PLAYING);
}

// ============================================================================
// Quiescence node accounting
// ============================================================================
// Both counters count legal move edges actually searched: nodes_searched in pvs(), qnodes_searched
// in quiescence(). These pin the split and the unit.

TEST_CASE("Search - quiescence nodes are counted separately from main-tree nodes", "[search][nodes]")
{
	AIPerlexTestFixture fix("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

	const SearchResult result = fix.result_to_depth(6);
	REQUIRE_FALSE(result.best_move.is_null());

	// A zero on either side means a counter stopped being incremented — silent
	// in every other test.
	CHECK(fix.mainnodes() > 0);
	CHECK(fix.qnodes() > 0);

	// At Threads=1 there are no helper counts to add, so the result's fields are exactly
	// the main thread's, and the two are reported separately rather than pre-summed.
	CHECK(result.nodes_searched == fix.mainnodes());
	CHECK(result.qnodes_searched == fix.qnodes());
}

TEST_CASE("Search - node counters reset between searches", "[search][nodes]")
{
	AIPerlexTestFixture fix("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

	REQUIRE_FALSE(fix.search_to_depth(5).is_null());
	const int64_t first_main = fix.mainnodes();
	const int64_t first_q = fix.qnodes();
	REQUIRE(first_main > 0);
	REQUIRE(first_q > 0);

	// A second search on the SAME fixture reuses a warm TT, so its count is not comparable
	// with the first. The sentinel makes the reset provable anyway: a counter that
	// accumulated instead of resetting can only report back at least this much.
	constexpr int64_t sentinel = 1'000'000'000;
	fix.poison_node_counters(sentinel);

	REQUIRE_FALSE(fix.search_to_depth(5).is_null());
	CHECK(fix.mainnodes() < sentinel);
	CHECK(fix.qnodes() < sentinel);
	CHECK(fix.mainnodes() > 0);

	// No qnodes lower bound here, deliberately. quiescence() may take a bound from a MAIN entry,
	// so against a warm table every quiescence call can be served at the probe and the second
	// search legitimately searches zero quiescence edges. The fresh fixture below still pins a
	// non-zero count, on a cold table where the work has to happen.

	// The exact-equality form of the same property, with the table taken out of it:
	// an independent fixture is a fresh AI and a fresh TT, so a correctly reset
	// counter must reproduce the first search's count exactly.
	AIPerlexTestFixture fresh("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
	REQUIRE_FALSE(fresh.search_to_depth(5).is_null());
	CHECK(fresh.mainnodes() == first_main);
	CHECK(fresh.qnodes() == first_q);
}

// A depth-1 node's children are quiescence roots, which add no main node of their own, and a
// window no move can fail high against searches every move. So its main count is its edge count.
// Both cases assume nothing returns before the move loop at such a node; a depth-1 node-level
// pruning step would break that assumption, not the counter.
TEST_CASE("Search - a main node is a legal move, not a pseudo-legal one DoMove rejects", "[search][nodes]")
{
	// The e7 rook pins the e2 knight: six knight moves the generator emits and DoMove rejects,
	// four legal king moves. Verified against python-chess.
	AIPerlexTestFixture fix("7k/4r3/8/8/8/8/4N3/4K3 w - - 0 1");
	fix.set_frontier_futility(false);
	fix.arm_clock();
	REQUIRE(fix.count_legal_moves() == 4);

	constexpr int kHighAlpha = 5000;
	fix.search_node(/*depth=*/1, /*ply=*/1, kHighAlpha, kHighAlpha + 1, /*is_pv_node=*/false);

	REQUIRE(fix.frontier_skips() == 0);
	CHECK(fix.mainnodes() == 4);
}

TEST_CASE("Search - a move frontier futility skips is not a main node", "[search][nodes][futility]")
{
	// Two captures, four promotions, two quiet checks and 21 plain quiet moves; the guard skips
	// the 21 at this window.
	AIPerlexTestFixture fix("7k/1P6/8/3p4/8/2N5/8/3QK3 w - - 0 1");
	fix.set_frontier_futility(true);
	fix.arm_clock();

	constexpr int kHighAlpha = 5000;
	fix.search_node(/*depth=*/1, /*ply=*/1, kHighAlpha, kHighAlpha + 1, /*is_pv_node=*/false);

	REQUIRE(fix.frontier_skips() > 0);
	CHECK(fix.mainnodes() == fix.count_legal_moves() - fix.frontier_skips());
}

// ============================================================================
// go nodes — node-limit poll (SearchLimits::fixed_nodes)
// ============================================================================
// The poll in pvs()/quiescence() fires only every 1024 nodes on thread 0, so
// the search cannot stop exactly at the budget — these pin the observable
// contract instead: it stops within one poll interval, and it does so the
// same way every time.

TEST_CASE("Search - fixed_nodes stops within one poll interval past the budget", "[search][nodes][search_control]")
{
	// After 1.e4 e5 2.Nf3, black to move. max_depth=8 is chosen so that this test
	// fails rather than hangs if the poll ever stops firing: a full depth-8 search
	// here costs far more than the budget, so a broken limit overshoots the upper
	// bound below and goes red in about a second — while with a working limit the
	// search still stops on nodes, mid-iteration, well before depth 8.
	AIPerlexTestFixture fix("rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2", /*max_depth=*/8);

	constexpr int64_t node_budget = 20000;
	const Move move = fix.search_with_nodes(node_budget);
	REQUIRE_FALSE(move.is_null());

	const int64_t total_nodes = fix.mainnodes() + fix.qnodes();
	CHECK(total_nodes >= node_budget);
	CHECK(fix.search_is_aborted());
	// Loose on purpose: the limit is observed only at multiples of 1024, and during
	// the abort collapse the parent pvs()/quiescence() frames each add their own
	// nodes before unwinding. Do not tighten without re-deriving the bound.
	CHECK(total_nodes <= node_budget + 8192);
}

TEST_CASE("Search - fixed_nodes is deterministic across repeated searches", "[search][nodes]")
{
	// This is the property the abort-collapse strategy depends on: a fresh search
	// hitting the same node budget from the same position must behave identically
	// every time, not just land in a range.
	const std::string fen = "rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2";
	constexpr int64_t node_budget = 20000;

	AIPerlexTestFixture first(fen, /*max_depth=*/8);
	const Move first_move = first.search_with_nodes(node_budget);
	REQUIRE_FALSE(first_move.is_null());
	const int64_t first_total = first.mainnodes() + first.qnodes();

	AIPerlexTestFixture second(fen, /*max_depth=*/8);
	const Move second_move = second.search_with_nodes(node_budget);
	REQUIRE_FALSE(second_move.is_null());
	const int64_t second_total = second.mainnodes() + second.qnodes();

	CHECK(MoveFormatter::ToUCI(first_move) == MoveFormatter::ToUCI(second_move));
	CHECK(first_total == second_total);
}

TEST_CASE("Search - a pvs frame that aborts at entry leaves an empty pv row", "[search][pv]")
{
	// The one abort the unwind guard cannot cover, because it happens before the frame searches
	// anything: the entry exit returns a fabricated GameValues::Draw. A parent frame discards
	// that at its own guard, but the root's caller is search_with_aspiration(), which hands it to
	// iterative_deepening() as this iteration's score. Row 0 is what decides whether that score
	// is believed — a populated row (here, a completed aspiration retry's line) makes
	// metrics.current_move plausible and lets `score cp 0` through CASE 4 in a balanced position.
	// Clearing the row before the exit is what turns it into the INCOMPLETE rejection instead.
	AIPerlexTestFixture fix;
	fix.seed_pv_row(0, AnyLegalMove());
	REQUIRE(fix.pv_length(0) == 1); // the earlier retry's line, still standing

	fix.ai->Stop(); // latch the abort flag, as UCI 'stop' or an expired clock would

	const int score = fix.search_node(/*depth=*/4, /*ply=*/0);

	REQUIRE(score == GameValues::Draw); // fabricated: nothing was searched
	REQUIRE(fix.pv_length(0) == 0);     // ... and nothing is published alongside it
	REQUIRE(fix.pv_move(0).is_null());
}

TEST_CASE("Search - TT stats are internally consistent", "[search][telemetry][tt]")
{
	// The test binary compiles the TT stats in, so a search here does the counting.
	REQUIRE(kTTStatsCompiled);
	AIPerlexTestFixture fix;
	const SearchResult result = fix.result_to_depth(6);
	REQUIRE_FALSE(result.best_move.is_null());
	const TTStats& tt = result.telemetry.tt;

	CHECK(tt.main_probes > 0);
	CHECK(tt.main_hits > 0);
	CHECK(tt.main_hits <= tt.main_probes);
	CHECK(tt.main_cutoffs <= tt.main_hits);
	CHECK(tt.qs_probes > 0);
	CHECK(tt.qs_hits <= tt.qs_probes);
	CHECK(tt.qs_cutoffs <= tt.qs_hits);
	CHECK(tt.stores_filled > 0);
	// Every search node probes before it can store, so stores cannot outnumber probes.
	CHECK(tt.stores() <= tt.main_probes + tt.qs_probes);

	// Per search, not per game: a depth-1 search on the same service reports only its own few probes.
	const SearchResult shallow = fix.result_to_depth(1);
	CHECK(shallow.telemetry.tt.main_probes > 0);
	CHECK(shallow.telemetry.tt.main_probes < tt.main_probes);
}
