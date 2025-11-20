// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "AIPerplex.h"
#include "Sort.h"
#include "MoveGenerator.h"

#include "TranspositionTable.h"
#include "Utils/Logger.h"
#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

extern std::ofstream outLegalMoves;

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
			spdlog::init_thread_pool(8192, 1);
			auto tp = spdlog::thread_pool();

			// add both console and file sinks (file sink keeps a record for diagnostics)
			auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
			console_sink->set_level(spdlog::level::info);
			console_sink->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
			auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("aiperplex.log", true);
			file_sink->set_level(spdlog::level::debug);

			s_logger = std::make_shared<spdlog::async_logger>(
				"AIPerplex",
				spdlog::sinks_init_list{ console_sink, file_sink },
				tp,
				spdlog::async_overflow_policy::block);

			spdlog::register_logger(s_logger);
			s_logger->set_level(spdlog::level::info);
			s_logger->flush_on(spdlog::level::info);
		}
		catch (...) {
			// best-effort; leave it empty if creation fails
		}
	}
}
// PVS Iterative transpositional alpha beta search
// Transposition tables
Move AIPerplex::GetMove(_Inout_ GameInfo& info)
{
	InitMoveVariables(info);
	_searchCount = 0;

	// Adding new Transposition table and PV Table
	TranspositionTable tt(256);
	PVTable pv_table;

	StartTimer();
	int score = iterative_deepening(m_MaxDepth, tt, pv_table);

	auto elapsed = StopTimerAndAdjustVars();
	if (IsVerboseLoggingEnabled()) {
		ensure_logger_initialized();
		if (s_logger) {
			s_logger->info("Final score: {}", score);
			s_logger->info("Elapsed time (ms): {}", elapsed.count());
		}
	}
	CheckGameOver(info);

	Move bestMove = pv_table.get_pv_move(0);
	UpdateBoardInfo(bestMove, info);

	return bestMove;
}

// - ABIterTrans doesn't assert, but does not generate the same moves as the non-trans search algos
//int AIPerplex::Search(_In_ size_t ply, _In_ int alpha, _In_ int beta, _Inout_ PVLine& pline)
//{
	// Increment counter - we are not returning early
//	m_SearchCount++;
//
//	// Sorterer traekkene
//	MoveSorter::SortMovesIter(moveList, GetParentMove(ply), // Last move
//		GetIterMove(ply));
//
//	Move goodMove;
//	size_t counter = 0;
//
//	for (const auto& curMove : moveList)
//	{
//		counter++;
//
//#ifdef PRINT_MOVES
//			// Udskriver traekkene til fil
//			if (ply == 0 && m_Depth == m_MaxDepth)	// for iterativ udskriv kun i roden af traeet
//				PrintMovesAndScore(outLegalMoves, counter, moveList.size(), curMove, value);
//		{
//			// Udskriver traekkene til fil
//			if (ply == 0 && m_Depth == m_MaxDepth)	// for iterativ kun udskriv i roden
//				// Ulovligt traek!!														// Magic value: illegal move
//				PrintMovesAndScore(outLegalMoves, counter, moveList.size(), curMove, -GameValues::Search_Init - 1);
//		}
//#endif // PRINT_MOVES
//	}
//}

int AIPerplex::iterative_deepening(int max_depth, TranspositionTable& tt, PVTable& pv_table) {
	int best_value = 0;

	for (int depth = 1; depth <= max_depth; ++depth) {
		tt.newSearchIteration();
		pv_table.clear_ply(0);

		best_value = pvs(
			depth,
			-GameValues::Search_Init,
			GameValues::Search_Init,
			0,
			true,
			tt,
			pv_table);

		// We've gotten a new move in the PVLine - send it to listeners
		//ENewPVLineMove.fire(this, m_Line);
		// FIXME - re-migrate to Subscriber pattern
		
		// Verbose per-depth output is expensive; gate it behind the runtime flag
		if (IsVerboseLoggingEnabled())
		{
			ensure_logger_initialized();
			if (s_logger) {
				// Build PV string efficiently
				std::string pv_line;
				pv_line.reserve(64);
				int len = std::min(pv_table.get_length(0), 10);
				for (int i = 0; i < len; ++i) {
					if (i) pv_line += ", ";
					pv_line += pv_table.get_line(0)[i].Output();
				}
				s_logger->info("Depth {:>2}: Score: {:>5} Best: {}",
					depth, best_value, pv_line/*, tt.count_entries(), tt.count_pv_nodes()*/);
			}
		}
		std::cout << " (TT: " << tt.count_entries() << ", PV nodes: " << tt.count_pv_nodes() << ")\n";
	}

	return best_value;
}

int AIPerplex::pvs(int depth, int alpha, int beta, int ply, bool is_pv_node, TranspositionTable& tt, PVTable& pv_table) {

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

	auto key = m_Board.GetCurBoardHKey();
	int original_alpha = alpha;
	Move hash_move;
	Move pv_move;

	bool tt_hit = false;

	// TT probe
	if (auto entry = tt.probe(key, ply))
	{
		if (entry->phase == SearchPhase::MAIN) { // Avoid the Quiescence nodes to affect main search - just to make sure
			tt_hit = true;
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
	moveList.reserve(MAX_PLY * 8);

	// Henter de lovlige traek
	MoveGenerator::ComputeLegalMoves(info, moveList);
	bool first_child = true;
	int best_value = -GameValues::Search_Init;
	Move best_move;

	// TODO: Relocate MoveSorting to MoveSorter class
	// Move ordering: PV move first, then hash move, then rest
	auto order_moves = [&](const Move& a, const Move& b) {
		if (a == pv_move && b == pv_move) return false;        // Equal
		if (a == pv_move) return true;                         // a before b
		if (b == pv_move) return false;                        // b before a

		if (a == hash_move && b == hash_move) return false;    // Equal
		if (a == hash_move) return true;
		if (b == hash_move) return false;

		// Apply MVV-LVA for captures
		auto scoreA = Move::Value(a);
		auto scoreB = Move::Value(b);
		if (scoreA != scoreB)
			return scoreA > scoreB;

		// Tie-break by lex order
		if (a.From != b.From)
			return a.From < b.From;
		return a.To < b.To;
		};

	bool moveFound = false;

	std::sort(moveList.begin(), moveList.end(), order_moves);

	for (const auto& move : moveList) {
		if (tt_hit && move == hash_move && !first_child) continue;
		if (is_pv_node && move == pv_move && !first_child) continue;

		_searchCount++;

		if (m_Board.DoMove(move))
		{
			// Tilfoejer dette traek til nuvaerende traekfoelge - og opdaterer resten
			AddMoveToSeq(move, ply);
			int value;

			if (first_child) {
				value = -pvs(depth - 1, -beta, -alpha, ply + 1, is_pv_node, tt, pv_table);
				first_child = false;
			}
			else {
				value = -pvs(depth - 1, -alpha - 1, -alpha, ply + 1, false, tt, pv_table);

				if (value > alpha && is_pv_node) {
					value = -pvs(depth - 1, -beta, -alpha, ply + 1, true, tt, pv_table);
				}
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
				}
			}

			if (beta <= alpha) break;
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
	verify_tt_store(tt, key, ply, best_value, depth, best_move, bound, node_type, SearchPhase::MAIN);

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
			// We are not saving anything in the hash as we have no move here

			return -GameValues::Mate + static_cast<int>(ply);		// return mate value minus distance to it
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

	auto key = m_Board.GetCurBoardHKey();

	// Probe TT for cached info
	if (auto entry = tt.probe(key, ply)) {
		// Only use TT cutoff if QUIESCENCE entry AND depth stored is sufficient
		if (entry->phase == SearchPhase::QUIESCENCE && entry->depth >= qsearch_depth) {
			// Scale quiescence depth for comparison
			//int adjusted_depth = entry->depth / 2;
			//if (adjusted_depth >= depth) {
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
	moveList.reserve(MAX_PLY * 8);
	// Generate only capture moves and promotions
	MoveGenerator::ComputeCaptures(info, moveList);
	// Sort the found captures
	MoveSorter::SortMovesByValue(moveList, moveList.size());

	// Tjek om der er lovlige brugbare traek her
	bool moveFound = false;
	Move best_move = Move::EmptyMove();

	for (const auto& move : moveList)
	{
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
			//alpha = score;
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

void AIPerplex::verify_tt_store(const TranspositionTable& tt, std::uint64_t key, int16_t ply,
	int16_t value, int16_t depth, Move best_move,
	BoundType bound, NodeType node_type, SearchPhase phase)
{
	auto verify = tt.probe(key, ply);

	assert(verify && "Probe failed after store");
	assert(verify->key == key && "Key mismatch");
	assert(verify->depth == depth && "Depth mismatch");
	assert(verify->bound == bound && "Bound mismatch");
	assert(verify->phase == phase && "Phase mismatch");
	assert(verify->value == value && "Value mismatch");
}
