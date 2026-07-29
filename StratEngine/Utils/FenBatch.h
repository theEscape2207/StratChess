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

// Classifies one input line using FENParser::ParseFEN as the single
// authoritative gate.
//
// This used to carry its own field-count pre-filter ahead of the parser, for
// the sake of a precise diagnostic on the most damaging malformation: a FEN
// missing its side-to-move field is silently treated as Black-to-move (issue
// #46), which across a large tuning corpus is garbage fitted with no warning.
// That tier is gone (issue #143). It reported "need at least 4" while the
// parser's regex in fact demanded all six fields, so the advice was wrong;
// and ParseFEN now performs the field-count check itself, before its regex,
// emitting "too few fields in FEN" for exactly the same inputs. One tier now
// covers what two used to, without the misleading message.
inline LineResult ClassifyLine(std::string_view line) {
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return { LineKind::Skip, {} };            // blank line
    }
    if (line[first] == '#') {
        return { LineKind::Skip, {} };             // comment
    }

    FENParser::FENGameState state;
    std::vector<std::tuple<ePiece, eSquare>> pieces;
    if (auto err = FENParser::ParseFEN(std::string(line), state, pieces)) {
        return { LineKind::Malformed, *err };
    }

    return { LineKind::Valid, {} };
}

}  // namespace FenBatch
