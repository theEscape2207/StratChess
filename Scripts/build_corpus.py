"""Build a score-identity / behaviour-preservation FEN corpus.

Harvests FENs from existing repo assets (never invents positions):
  - Tests/perft_test_cases.json
  - Tests/tactical_test_cases.json
  - StratChessTests/*.cpp FEN string literals
  - Tests/openings/openings-250.pgn        (opening material, sampled every 2 plies)
  - optional self-play PGN directories via --pgn-dir (realistic middlegame +
    endgame material, sampled every N plies, see --every-n-plies)

Emits one FEN per line to --out, deduplicated, order-stable (first-seen wins).
Each candidate FEN is validated before being added: all six FEN fields must
be present (FENParser's regex requires them) and the side NOT to move must
not be in check (the engine silently accepts such positions per issue #45,
and they are not reachable in real play).

Purpose: this is the harvester behind #127's score-identity corpus (8574
positions, used to prove the EvalContext restructure was byte-identical to
the pre-restructure evaluator). It is not #127-specific:
  - #131 (pawn hash table) needs the same score-identity workflow.
  - #117 (Texel tuning) needs a large corpus of labelled quiet positions;
    this is the seed of that harvester.
  - Any future refactor asserting behaviour preservation can reuse it.

Usage:
    python Scripts/build_corpus.py
    python Scripts/build_corpus.py --out /tmp/corpus.fen
    python Scripts/build_corpus.py \
        --pgn-dir StratChessEvolved/logs/elo --pgn-dir /path/to/more/pgns \
        --every-n-plies 7

Run with no arguments from a clean checkout: it produces a corpus from the
in-repo assets alone (perft/tactical JSON, StratChessTests/*.cpp literals,
Tests/openings/openings-250.pgn). Self-play PGNs are an optional addition
via --pgn-dir, since a fresh checkout has no self-play logs (they are
gitignored, written under <repo>/StratChessEvolved/logs/elo/ by
Scripts/Run-EloMatch.ps1 and by ad hoc self-play runs).

Requires: pip install python-chess (developed/tested against 1.11.2).
"""
import argparse
import json
import re
import sys
from pathlib import Path

try:
    import chess
    import chess.pgn
except ImportError as exc:  # pragma: no cover - environment guard, not test logic
    sys.exit(
        "build_corpus.py requires the 'python-chess' package.\n"
        "Install it with: pip install python-chess\n"
        f"(import failed: {exc})"
    )


def add(fen: str, source: str, fens: list, seen: set) -> None:
    """Validate and record one candidate FEN, deduplicating by exact string."""
    fen = fen.strip()
    if not fen:
        return
    # Must have all six fields — FENParser's regex requires them.
    if len(fen.split()) != 6:
        return
    try:
        board = chess.Board(fen)
    except Exception:
        return
    # Reject positions where the side NOT to move is in check: the engine
    # silently accepts them (issue #45) and they are not reachable in real play.
    mirror = board.copy(stack=False)
    mirror.turn = not board.turn
    if mirror.is_check():
        return
    if fen in seen:
        return
    seen.add(fen)
    fens.append((fen, source))


def from_json(path: Path, source: str, fens: list, seen: set) -> None:
    """Recursively walk a JSON document, harvesting every string value keyed 'fen'."""
    if not path.exists():
        return
    data = json.loads(path.read_text(encoding="utf-8-sig"))

    def walk(node):
        if isinstance(node, dict):
            for k, v in node.items():
                if k.lower() == "fen" and isinstance(v, str):
                    add(v, source, fens, seen)
                else:
                    walk(v)
        elif isinstance(node, list):
            for item in node:
                walk(item)

    walk(data)


def from_cpp(directory: Path, source: str, fens: list, seen: set) -> None:
    """Harvest FEN-shaped string literals out of *.cpp/*.h files in directory."""
    if not directory.is_dir():
        return
    fen_like = re.compile(r'"((?:[rnbqkpRNBQKP1-8]+/){7}[rnbqkpRNBQKP1-8]+[^"]*)"')
    for cpp in sorted(directory.glob("*.cpp")) + sorted(directory.glob("*.h")):
        for m in fen_like.finditer(cpp.read_text(encoding="utf-8", errors="replace")):
            add(m.group(1), source, fens, seen)


def from_pgn(path: Path, source: str, every_n_plies: int, max_games: int,
             fens: list, seen: set) -> None:
    """Sample one FEN every `every_n_plies` plies from each game in a PGN file."""
    if not path.exists():
        return
    with path.open(encoding="utf-8", errors="replace") as fh:
        for _ in range(max_games):
            game = chess.pgn.read_game(fh)
            if game is None:
                break
            board = game.board()
            ply = 0
            for move in game.mainline_moves():
                board.push(move)
                ply += 1
                if ply % every_n_plies == 0:
                    add(board.fen(), source, fens, seen)


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build a deduplicated, validated FEN corpus from in-repo "
                     "test assets and optional self-play PGNs.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--repo-root", type=Path, default=None,
        help="Repository root. Defaults to the checkout containing this script "
             "(<repo>/Scripts/build_corpus.py -> <repo>).",
    )
    parser.add_argument(
        "--out", type=Path, default=None,
        help="Output path for the FEN corpus, one FEN per line. "
             "Defaults to <repo-root>/corpus.fen.",
    )
    parser.add_argument(
        "--pgn-dir", action="append", default=[], metavar="DIR",
        help="Directory of self-play PGNs to sample (repeatable). Optional — "
             "omit for an in-repo-assets-only corpus. Typically "
             "StratChessEvolved/logs/elo under a checkout that has run self-play.",
    )
    parser.add_argument(
        "--every-n-plies", type=int, default=7, metavar="N",
        help="Ply-sampling interval applied to --pgn-dir PGNs (default: 7). "
             "A self-play game is long and highly autocorrelated move-to-move, "
             "so a tight interval yields near-duplicate positions — sampling "
             "every 7 plies spreads coverage across opening/middlegame/endgame. "
             "Tests/openings/openings-250.pgn is always sampled every 2 plies "
             "regardless of this flag: it is many short, distinct opening "
             "lines rather than one long game, so a tighter interval is safe.",
    )
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)

    script_dir = Path(__file__).resolve().parent
    repo_root = (args.repo_root.resolve() if args.repo_root
                 else script_dir.parent.parent)
    out_path = (args.out.resolve() if args.out
                else repo_root / "corpus.fen")

    fens: list = []
    seen: set = set()

    from_json(repo_root / "Tests" / "perft_test_cases.json", "perft", fens, seen)
    from_json(repo_root / "Tests" / "tactical_test_cases.json", "tactical", fens, seen)
    from_cpp(repo_root / "StratChessTests", "tests", fens, seen)
    from_pgn(repo_root / "Tests" / "openings" / "openings-250.pgn", "openings",
              2, 250, fens, seen)

    for pgn_dir in args.pgn_dir:
        d = Path(pgn_dir)
        if not d.is_dir():
            print(f"warning: --pgn-dir '{d}' is not a directory, skipping", file=sys.stderr)
            continue
        for pgn in sorted(d.glob("*.pgn")):
            from_pgn(pgn, "selfplay", args.every_n_plies, 400, fens, seen)

    counts: dict = {}
    for _, src in fens:
        counts[src] = counts.get(src, 0) + 1
    print(f"corpus: {len(fens)} unique positions {counts}", file=sys.stderr)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(f for f, _ in fens) + "\n", encoding="ascii")
    print(f"wrote {out_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
