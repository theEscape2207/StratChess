# CONTEXT

Project glossary. Terms whose boundaries matter, defined once so code, comments, issues and reviews
mean the same thing by them.

This is a glossary and nothing else — no thresholds, no ordering tiers, no implementation detail.
Design decisions live in `.claude/plans/`, architecture in `CLAUDE.md`.

## Move classification

Chess terms in general use, recorded here because the categories overlap in ways that have already
produced a defect: a move can belong to more than one, and code that treats them as a partition is
wrong.

### Out of check

**Capture** — a move that removes an opponent piece. En passant is a capture whose removed pawn is
**not** on the destination square.

**Quiet move** — a move that removes nothing.

Promotions and checks are *noisy* moves in the general literature and are neither captures nor quiet
in that scheme. The engine does not use that classification; it treats a promotion as its own case
where one is needed.

### In check

Three kinds of legal move, and every legal move in check is one of them:

**King evasion** — the king moves. This includes the king capturing a piece, and the captured piece
need not be the one giving check.

**Capture of the attacker** — the checking piece is removed. Not testable as
`move.to() == checker_square`: an en passant capture of a checking pawn lands on a different square
from the pawn it removes. A king capture of the checking piece is *both* a king evasion and a
capture of the attacker.

**Interposition** — a piece moves onto a square between the checking slider and the king. Only
possible against a slider, and never against a knight or pawn check.

**Double check** admits **king evasions only**. No capture of the attacker and no interposition can
answer two checking pieces at once, which makes double check a cheap early-out wherever the three
kinds are enumerated.

**Evasion** is the umbrella term for all three.
