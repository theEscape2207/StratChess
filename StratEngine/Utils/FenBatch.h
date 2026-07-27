#pragma once

// Header-only classification for one line of a batch FEN input file (as consumed
// by `StratChessEvolved.exe eval <path>`, issue #129 phase 1). Extracted out of
// evalrunner() (StratChessEvolved.cpp) so the classification logic — which is
// load-bearing, not defensive; see below — is directly testable from
// StratChessTests without linking StratChessEvolved.cpp (issue #140).
//
// Why this guard matters: Board() default-constructs an empty board and
// SetupFromFEN() applies nothing on a parse error, so an unguarded malformed
// line would silently score a fresh empty board as 0 — plausible-looking
// garbage in a tuning corpus rather than an obvious failure.

#include "FENParser.h"

#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace FenBatch {

enum class LineKind {
    Skip,       // blank line or '#' comment — not an error, nothing to report
    Malformed,  // failed either validation tier; `error` explains why
    Valid       // parses cleanly; safe to SetupFromFEN() and score
};

struct LineResult {
    LineKind kind;
    std::string error;  // populated only when kind == Malformed
};

// Classifies one input line using the same two-tier gate evalrunner() has
// always used:
//   1. A field-count pre-filter (< 4 whitespace-separated fields), purely so
//      the most damaging malformation gets a precise diagnostic: a FEN
//      missing its side-to-move field is silently treated as Black-to-move
//      (issue #46), which across a large tuning corpus is garbage fitted
//      with no warning.
//   2. FENParser::ParseFEN as the authoritative gate — its regex actually
//      requires all six standard FEN fields, so a 4- or 5-field line clears
//      tier 1 but still fails here with the parser's own error message.
// These two tiers deliberately produce distinguishable messages: whoever is
// cleaning a corpus needs to know whether a line "isn't a FEN at all" or "is
// a FEN the parser is strict about".
inline LineResult ClassifyLine(std::string_view line) {
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return { LineKind::Skip, {} };            // blank line
    }
    if (line[first] == '#') {
        return { LineKind::Skip, {} };             // comment
    }

    std::istringstream field_stream{ std::string(line) };
    int field_count = 0;
    std::string field_tok;
    while (field_stream >> field_tok) ++field_count;
    if (field_count < 4) {
        std::ostringstream msg;
        msg << "malformed FEN (" << field_count << " field(s), need at least 4)";
        return { LineKind::Malformed, msg.str() };
    }

    FENParser::FENGameState state;
    std::vector<std::tuple<ePiece, eSquare>> pieces;
    if (auto err = FENParser::ParseFEN(std::string(line), state, pieces)) {
        return { LineKind::Malformed, *err };
    }

    return { LineKind::Valid, {} };
}

}  // namespace FenBatch
