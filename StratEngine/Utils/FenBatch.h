#pragma once

// Header-only classification for one line of a batch FEN input file (as consumed
// by `StratChessEvolved.exe eval <path>`, issue #129 phase 1). Extracted out of
// evalrunner() (StratChessEvolved.cpp) so the classification logic is directly
// testable from StratChessTests without linking StratChessEvolved.cpp (issue #140).
//
// What this adds over Board::SetupFromFEN's own bool return: the three-way
// blank/comment/malformed split an input file needs, and the parser's message for
// the malformed case, so a bad line can be reported with its line number and
// skipped rather than aborting the batch.
//
// Scope: this is a check on FEN *syntax* only, because it goes through
// FENParser::ParseFEN, which has no board to generate attacks on. Position
// legality (issue #45 — the side not to move being in check) is enforced by
// Board::SetupFromFEN, so a line classified Valid here can still fail to load.
// Callers must handle both.

#include "FENParser.h"

#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace FenBatch {

	// LineResult below isn't bulk-stored; its std::string member dwarfs any savings anyway.
	// NOLINTNEXTLINE(performance-enum-size)
	enum class LineKind {
		Skip,      // blank line or '#' comment — not an error, nothing to report
		Malformed, // failed either validation tier; `error` explains why
		Valid      // parses cleanly; safe to SetupFromFEN() and score
	};

	struct LineResult {
		LineKind kind;
		std::string error; // populated only when kind == Malformed
	};

	// Classifies one input line, delegating to FENParser::ParseFEN as the single
	// authoritative gate -- it reports both a bad field count and a bad format.
	inline LineResult ClassifyLine(std::string_view line)
	{
		const auto first = line.find_first_not_of(" \t\r\n");
		if (first == std::string_view::npos) {
			return {LineKind::Skip, {}}; // blank line
		}
		if (line[first] == '#') {
			return {LineKind::Skip, {}}; // comment
		}

		FENParser::FENGameState state;
		std::vector<std::tuple<ePiece, eSquare>> pieces;
		if (auto err = FENParser::ParseFEN(std::string(line), state, pieces)) {
			return {LineKind::Malformed, *err};
		}

		return {LineKind::Valid, {}};
	}

} // namespace FenBatch
