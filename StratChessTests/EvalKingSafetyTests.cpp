#include "EvalTestFixture.h"

// ── King safety: shelter, storm and king-file openness (issue #97) ────────────
//
// All three contributions are middlegame-only, so every position here keeps a
// queen on each side. A bare-king position sits at phase 0, where each of them
// blends to exactly 0 and any assertion about them is vacuous — the phase-fade
// case below is the one that asserts that on purpose.

// An illegal FEN does not fail loudly: Board falls back to the starting
// position, and the term then reports a perfectly plausible number for a
// position nobody wrote. Every case that places the White king somewhere
// specific pins it here first.
static void RequireWhiteKingOn(const Board& board, eSquare square)
{
	REQUIRE(board.GetPiece(square) == ePiece::WHITE_KING);
}

// ── The zone anchor (D3) ──────────────────────────────────────────────────────

TEST_CASE("Eval - king zone: the file clamp makes the corner indistinguishable from the b/g file", "[eval]")
{
	// The failure mode the clamp exists to prevent: without it a king stepping
	// g1 -> h1 loses a third of its surroundings, every count taken over them
	// drops, and walking into the corner reads as SAFER. Kg1, Kh1 and Kg2 must
	// therefore anchor on the same square.
	REQUIRE(EvalComplexTestFixture::KingZoneAnchor(g1) == g2);
	REQUIRE(EvalComplexTestFixture::KingZoneAnchor(h1) == g2);
	REQUIRE(EvalComplexTestFixture::KingZoneAnchor(g2) == g2);

	// The queenside mirror of the same three.
	REQUIRE(EvalComplexTestFixture::KingZoneAnchor(a1) == b2);
	REQUIRE(EvalComplexTestFixture::KingZoneAnchor(b1) == b2);

	// A central king is its own anchor: both clamps are no-ops there.
	REQUIRE(EvalComplexTestFixture::KingZoneAnchor(e4) == e4);

	// Black's back rank, the vertical mirror of the first group.
	REQUIRE(EvalComplexTestFixture::KingZoneAnchor(g8) == g7);
	REQUIRE(EvalComplexTestFixture::KingZoneAnchor(h8) == g7);
	REQUIRE(EvalComplexTestFixture::KingZoneAnchor(a8) == b7);
}

TEST_CASE("Eval - king zone: the anchor is on the b..g files and off the outermost rows, everywhere", "[eval]")
{
	for (int sq = a8; sq < NUM_SQUARES; ++sq) {
		const auto square = static_cast<eSquare>(sq);
		CAPTURE(sq);
		const eSquare anchor = EvalComplexTestFixture::KingZoneAnchor(square);
		// Both clamps hold for every square, which is what keeps the 3x3 block
		// around the anchor wholly on the board.
		CHECK(File(anchor) >= 1);
		CHECK(File(anchor) <= 6);
		CHECK(Rank(anchor) >= 1);
		CHECK(Rank(anchor) <= 6);
		// The anchor never moves more than one square from the king itself.
		CHECK(EvalComplexTestFixture::KingDistanceProbe(square, anchor) <= 1);
	}
}

// ── Shelter (D4) ──────────────────────────────────────────────────────────────

TEST_CASE("Eval - eval_king_shelter: an intact shield beats a pushed one, which beats none at all", "[eval]")
{
	Board intact(FEN_KING_SHIELD_INTACT);
	Board pushed(FEN_KING_SHIELD_PUSHED);
	Board absent(FEN_KING_SHIELD_ABSENT);

	// The same three White pawns in all three positions — only where they stand
	// differs, so nothing but shelter can explain the ordering.
	const int shelterIntact = EvalComplexTestFixture::KingShelter(intact, WHITE);
	const int shelterPushed = EvalComplexTestFixture::KingShelter(pushed, WHITE);
	const int shelterAbsent = EvalComplexTestFixture::KingShelter(absent, WHITE);

	CHECK(shelterIntact > shelterPushed);
	CHECK(shelterPushed > shelterAbsent);
	// A king with no cover at all is penalised, not merely under-rewarded: the
	// table is roughly zero-mean, so a missing shield pawn is a negative entry.
	CHECK(shelterAbsent < 0);
}

TEST_CASE("Eval - eval_king_shelter: a pawn level with the king is a pawn, not an absent one", "[eval]")
{
	// The sentinel case D4 exists to separate. Indexing by distance from the
	// king would give a level pawn distance 0, colliding with "no pawn on this
	// file"; indexing by relative rank cannot.
	Board level(FEN_KING_PAWN_LEVEL);
	Board offFile(FEN_KING_PAWN_OFF_FILE);
	RequireWhiteKingOn(level, e4);
	RequireWhiteKingOn(offFile, e4);

	CHECK(EvalComplexTestFixture::KingShelter(level, WHITE) > EvalComplexTestFixture::KingShelter(offFile, WHITE));
}

TEST_CASE("Eval - eval_king_shelter: own pawns behind the king are no shelter", "[eval]")
{
	// White Ke4 with its only pawn on e2, two ranks behind it. That pawn is not
	// cover, so the e-file must score as if it were empty.
	Board behind("3q2k1/8/8/8/4K3/8/4P3/3Q4 w - - 0 1");
	Board empty("3q2k1/8/8/8/4K3/8/P7/3Q4 w - - 0 1");
	RequireWhiteKingOn(behind, e4);
	RequireWhiteKingOn(empty, e4);

	CHECK(EvalComplexTestFixture::KingShelter(behind, WHITE) == EvalComplexTestFixture::KingShelter(empty, WHITE));
	// Both must be the no-cover result, not merely equal to each other — an
	// equality alone would also hold for a term that returned a constant.
	CHECK(EvalComplexTestFixture::KingShelter(behind, WHITE) <
	      EvalComplexTestFixture::KingShelter(Board(FEN_KING_SHIELD_INTACT), WHITE));
}

TEST_CASE("Eval - eval_king_shelter: every reachable shield rank mirrors between the colors", "[eval]")
{
	// Sweeps the whole reachable index range of the shelter table: a White pawn
	// on g2..g7 in front of Kg1 covers relative ranks 2..7, and index 0 (no
	// pawn) is covered by the absent case above. Each is checked against its
	// color mirror, which is what pins the defender-relative rank index.
	const int rank = GENERATE(range(2, 8));
	CAPTURE(rank);

	// FEN ranks are written from rank 8 down; rank r sits (8 - r) rows in.
	std::string placement;
	for (int row = 8; row >= 1; --row) {
		if (!placement.empty())
			placement += '/';
		if (row == 8)
			placement += "3q2k1";
		else if (row == 1)
			placement += "3Q2K1";
		else if (row == rank)
			placement += "6P1";
		else
			placement += "8";
	}
	const std::string fen = placement + " w - - 0 1";
	CAPTURE(fen);

	Board board(fen);
	Board mirrored(MirrorFen(fen));

	const int white = EvalComplexTestFixture::KingShelter(board, WHITE);
	CHECK(white == EvalComplexTestFixture::KingShelter(mirrored, BLACK));
	// A shield pawn is never worse than no pawn at all on that file, and the
	// nearer ranks are worth more — the shape the table exists to express.
	CHECK(white > EvalComplexTestFixture::KingShelter(Board(FEN_KING_SHIELD_ABSENT), WHITE));
}

// ── Storm (D4) ────────────────────────────────────────────────────────────────

TEST_CASE("Eval - eval_king_storm: an enemy pawn behind the king is not storming it", "[eval]")
{
	Board ahead(FEN_KING_STORM_AHEAD);
	Board behind(FEN_KING_STORM_BEHIND);
	RequireWhiteKingOn(ahead, g4);
	RequireWhiteKingOn(behind, g4);

	// Same Black pawn, one rank either side of White's king on g4.
	CHECK(EvalComplexTestFixture::KingStorm(ahead, WHITE) < 0);
	CHECK(EvalComplexTestFixture::KingStorm(behind, WHITE) == 0);
}

TEST_CASE("Eval - eval_king_storm: a blocked storm pawn counts half", "[eval]")
{
	// Pins the exact r' - 1 condition: the same Black pawn on g3, with White's
	// pawn either directly in its path (g2) or one file off it (f2). Asserted on
	// the unblended mg endpoints, because the halving is exact there and the
	// phase blend truncates.
	const int blocked = EvalComplexTestFixture::KingCover(Board(FEN_KING_STORM_BLOCKED), WHITE).storm.mg;
	const int unblocked = EvalComplexTestFixture::KingCover(Board(FEN_KING_STORM_UNBLOCKED), WHITE).storm.mg;

	REQUIRE(unblocked < 0);
	CHECK(blocked == unblocked / 2);
}

TEST_CASE("Eval - eval_king_storm: every reachable storm rank mirrors between the colors", "[eval]")
{
	// The storm counterpart of the shelter sweep. A Black pawn on g2..g7 in
	// front of White's Kg1 covers the whole reachable index range, and each is
	// checked against its color mirror — a defender-relative sign error confined
	// to the far ranks would survive the ahead/behind and blocked cases alone.
	const int rank = GENERATE(range(2, 8));
	CAPTURE(rank);

	std::string placement;
	for (int row = 8; row >= 1; --row) {
		if (!placement.empty())
			placement += '/';
		if (row == 8)
			placement += "3q2k1";
		else if (row == 1)
			placement += "3Q2K1";
		else if (row == rank)
			placement += "6p1";
		else
			placement += "8";
	}
	const std::string fen = placement + " w - - 0 1";
	CAPTURE(fen);

	Board board(fen);
	RequireWhiteKingOn(board, g1);
	Board mirrored(MirrorFen(fen));

	const int white = EvalComplexTestFixture::KingStorm(board, WHITE);
	CHECK(white == EvalComplexTestFixture::KingStorm(mirrored, BLACK));
	// Storm is a penalty or nothing, never a bonus — the sign the bound in
	// KING_SAFETY_MAX_PENALTY depends on.
	CHECK(white <= 0);
}

// ── King-file openness (D5) ───────────────────────────────────────────────────

TEST_CASE("Eval - eval_king_files: open is worse than half-open, which is worse than closed", "[eval]")
{
	// Isolated on the g-file: White has no g-pawn in either of the last two, and
	// Black's g-pawn — too far from White's king to storm it — is the only
	// difference between half-open and open.
	const int closed = EvalComplexTestFixture::KingFiles(Board(FEN_KING_FILE_CLOSED), WHITE);
	const int halfOpen = EvalComplexTestFixture::KingFiles(Board(FEN_KING_FILE_HALF_OPEN), WHITE);
	const int open = EvalComplexTestFixture::KingFiles(Board(FEN_KING_FILE_OPEN), WHITE);

	CHECK(closed == 0);
	// The isolation rests on Black's g7 pawn scoring no storm: it IS inside
	// White's scan and does index the storm table, at a row that is zero today.
	// Asserted so a #117 retune breaks this loudly rather than quietly making
	// the comparison below measure two terms at once.
	CHECK(EvalComplexTestFixture::KingStorm(Board(FEN_KING_FILE_HALF_OPEN), WHITE) == 0);
	CHECK(halfOpen < closed);
	CHECK(open < halfOpen);
}

TEST_CASE("Eval - eval_king_files: an own pawn behind the king still closes the file", "[eval]")
{
	// The deliberate difference from eval_rooks, which measures own pawns over
	// the forward span only. What this term prices is a lane an enemy rook can
	// use, and that is a whole-file property — so White Ke4 with a pawn on e2
	// has a closed e-file even though the pawn is behind the king.
	Board behind("3q2k1/8/8/8/4K3/8/4P3/3Q4 w - - 0 1");
	Board empty("3q2k1/8/8/8/4K3/8/P7/3Q4 w - - 0 1");
	RequireWhiteKingOn(behind, e4);
	RequireWhiteKingOn(empty, e4);

	CHECK(EvalComplexTestFixture::KingFiles(behind, WHITE) > EvalComplexTestFixture::KingFiles(empty, WHITE));
}

// ── Invariants (D7) ───────────────────────────────────────────────────────────

TEST_CASE("Eval - king safety: every contribution has an endgame endpoint of exactly 0", "[eval]")
{
	const char* fen =
	    GENERATE(FEN_KING_SHIELD_INTACT, FEN_KING_SHIELD_ABSENT, FEN_KING_STORM_BLOCKED, FEN_KING_FILE_OPEN);
	CAPTURE(fen);
	Board board(fen);

	for (const eColor color : {WHITE, BLACK}) {
		const KingPawnCover cover = EvalComplexTestFixture::KingCover(board, color);
		CHECK(cover.shelter.eg == 0);
		CHECK(cover.storm.eg == 0);
		CHECK(cover.files.eg == 0);
	}
}

TEST_CASE("Eval - king safety: fades to nothing as the pieces come off", "[eval]")
{
	// Same shattered White kingside; the second position has no pieces left to
	// exploit it. At phase 0 every king-safety contribution blends to exactly 0,
	// which is the property that made tapering (#99) a hard prerequisite.
	Board middlegame(FEN_KING_SHIELD_ABSENT);
	Board endgame("6k1/5ppp/8/8/8/8/1PPP4/6K1 w - - 0 1");

	CHECK(EvalComplexTestFixture::KingShelter(middlegame, WHITE) < 0);
	CHECK(EvalComplexTestFixture::KingShelter(endgame, WHITE) == 0);
	CHECK(EvalComplexTestFixture::KingStorm(endgame, WHITE) == 0);
	CHECK(EvalComplexTestFixture::KingFiles(endgame, WHITE) == 0);
}

TEST_CASE("Eval - king safety: a kingless board contributes nothing", "[eval]")
{
	// Default-constructed Board has no king, the same guard eval_pst,
	// eval_mopup and eval_castling carry. GetFirstPiece's assert(mask != 0)
	// precondition is a Release no-op, so an unguarded read here would silently
	// index a table out of bounds rather than trap.
	Board board;
	for (const eColor color : {WHITE, BLACK}) {
		CHECK(EvalComplexTestFixture::KingShelter(board, color) == 0);
		CHECK(EvalComplexTestFixture::KingStorm(board, color) == 0);
		CHECK(EvalComplexTestFixture::KingFiles(board, color) == 0);
	}
}

TEST_CASE("Eval - king safety: the combined contribution stays inside its declared bound", "[eval]")
{
	// KING_SAFETY_MAX_PENALTY is derived from the table extrema by a
	// static_assert, which proves the arithmetic. This proves the other half:
	// that the bound is right about which entries are actually reachable.
	// The corpus alone does not reach the extremes — its most shattered king is
	// nowhere near the bound, so it would pass against one three times too
	// large. FEN_KING_SAFETY_WORST is the case that makes the claim mean
	// something, and its own margin is asserted below.
	const char* fen = GENERATE(from_range(kSymmetryFens), FEN_KING_SAFETY_WORST);
	CAPTURE(fen);

	Board board(fen);
	for (const eColor color : {WHITE, BLACK}) {
		const KingPawnCover cover = EvalComplexTestFixture::KingCover(board, color);
		const int combined = cover.shelter.mg + cover.storm.mg + cover.files.mg;
		CAPTURE(combined);
		CHECK(combined <= EvalComplexTestFixture::KingSafetyMaxPenalty);
		CHECK(combined >= -EvalComplexTestFixture::KingSafetyMaxPenalty);
	}
}

TEST_CASE("Eval - king safety: the worst reachable king gets most of the way to the bound", "[eval]")
{
	// The other half of the bound claim. The static_assert proves the arithmetic
	// over the table extrema; this proves those extrema are reachable, so the
	// constant is a real ceiling rather than one inflated by entries no legal
	// position can index. White has no f/g/h pawn and Black has three on the
	// third rank.
	Board board(FEN_KING_SAFETY_WORST);
	RequireWhiteKingOn(board, g1);

	const KingPawnCover cover = EvalComplexTestFixture::KingCover(board, WHITE);
	const int combined = cover.shelter.mg + cover.storm.mg + cover.files.mg;
	CAPTURE(combined);

	CHECK(combined >= -EvalComplexTestFixture::KingSafetyMaxPenalty);
	// Within a third of the ceiling. A looser assertion would not distinguish a
	// tight bound from one that is merely never approached.
	CHECK(combined < -(EvalComplexTestFixture::KingSafetyMaxPenalty * 2) / 3);
}
