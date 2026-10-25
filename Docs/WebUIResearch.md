# Web chess UI: requirements

Status: requirements discovery, 2026-10-25. Not an approved design.

**Next task:** set up the UI repository (scaffold, CI, license, this document moved there as its
requirements). Engine issues below follow when their milestone needs them.

## Intent

A browser chess app served from the Windows dev machine to phones and tablets on the home network.
It proves StratChess end to end, then becomes the owner's engine workbench, then a place where the
owner's daughter and other beginners play and learn. It talks to engines over UCI only, lives in a
separate open-source repository, and has no commercial goal.

## Confirmed requirements

- **Platform:** local server on Windows only; engines run on the host; devices use a browser over
  the LAN.
- **Integration:** UCI only; no coupling to engine code.
- **Control:** one active game, controlled by one browser; other devices cannot take it over. No
  user accounts. A browser pairs through a code shown on the host. This is an accepted, clunky
  compromise; revisit for Play.
- **Session:** a game lives only while its page is open. Saved games, and timed games that survive
  a reconnect, are backlog.
- **Priority:** human vs engine, then analysis/statistics, then engine vs engine (single
  configurable games, including Stockfish; batches later).
- **Clocks and difficulty:** human untimed; StratChess limited by depth or time. These limits are
  never presented as Elo levels.
- **Statistics:** standard UCI output first, StratChess-specific statistics later.
- **Playing aids:** legal moves enforced from M1; legal-move highlighting in M2; takebacks and hints
  in M3.
- **Positions:** standard start in M1; FEN input in M2, including positions copied from puzzle sites.
- **Distribution:** separate GitHub repository with CI; new dependencies acceptable; GPL-3.0, matching
  the engine.

## Milestones

| Milestone | Audience | Scope | Done when |
|---|---|---|---|
| 1. Initial gameplay | Owner | Raw UI, standard start, human vs StratChess, depth/time limit, LAN access, pairing | A full game played from a phone finishes with the correct result, covering promotion choice, castling, en passant, checkmate, stalemate and automatic draws |
| 2. Technical | Owner | Board, move list, side selection, FEN load/copy, engine profile and limits, live search info, UCI transcript, legal-move highlighting | Any valid FEN can be loaded and played from; an invalid FEN is rejected with a reason; live depth/score/nodes/PV shown |
| 3. Play | Daughter, beginners | Simple controls, understandable difficulty choice, technical detail hidden, takebacks, hints | A beginner can win against the easiest setting, and use hints and takebacks without help |

M2 and M3 are two views of one app, sharing game and engine handling. No configurable dashboard.

## Decisions (agreed 2026-10-25)

| Decision | Choice | Why |
|---|---|---|
| Backend | Node + TypeScript (Fastify proposed for HTTP/WebSocket; not yet agreed) | One language end to end; chessops runs the rules on both server and browser; a single-engine UCI controller is small. python-chess saves protocol work but adds a second runtime |
| Board | Chessground | Lichess's board: proven touch handling on phones, arrows and square highlights for hints, PV and legal moves. Framework-neutral, so it needs a thin wrapper component |
| Rules | chessops | Same author as Chessground and designed to pair with it; FEN/SAN/UCI notation, PGN trees for later analysis. chess.js is a simpler fallback |
| Frontend | React + Vite + TypeScript | Widest choice of UI components; Chessground removes React's board advantage, so Svelte is an equal alternative |
| Transport security | Plain HTTP/WS, accepted for the home LAN | Self-signed certificates on phones (iOS especially) cost more than the threat they address |
| Pairing persistence | Browser keeps its token until revoked on the host | One code per device, not per game |
| Disconnect | Short grace period (e.g. 60 s), then stop the search and end the engine process | Phones drop sockets when locked; page-close events are unreliable |

## Architecture constraints (independent of stack)

- **The server owns the game.** It holds the position, validates every move and whose turn it is,
  and decides results. The browser validates only for instant feedback. The board widget is a view.
- **The server sends the whole position on every search** (`position ... moves ...`). Takeback and
  FEN load are then just new positions; the engine keeps no game state.
- **Searches carry a generation ID.** A `bestmove` from a cancelled or superseded search is
  discarded.
- **Engine profiles are configured on the host:** executable, arguments, working directory, options.
  Browsers choose a profile ID; they never supply paths or commands. Launch without a shell; no
  administrator rights.
- **Option forms are generated from what the engine announces** (`option name ...`), not
  Stockfish-shaped.
- **Bounded everything:** search duration, Threads, Hash, process count, message size, retained
  transcript; always drain stdout/stderr.
- **Cheap network hygiene:** Windows Firewall Private profile, local subnet only; no port
  forwarding; Host and WebSocket Origin allowlists; reject CR/LF in anything that becomes a UCI
  command; render engine output as text. Serve built assets; never expose the Vite dev server.

## StratChess UCI behaviour the UI must handle

Checked against `StratEngine/UCIHandler.cpp`.

- **A bad FEN does not fail loudly.** The engine resets to the start position and reports only
  `info string position: rejected FEN ...`. Validate FEN before sending it, and treat that line as
  an error.
- **Commands sent during a search are ignored** (`info string <cmd>: ignored, a search is in
  progress`). The controller must send `stop` and wait for `bestmove` before the next `position`.
- **`go` with no limit searches to depth 20.** Always send an explicit limit.
- Supported limits: `wtime/btime/winc/binc/movestogo`, `depth`, `movetime`, `nodes`, `infinite`.
  No `ponder`, `mate` or `searchmoves`.
- Options: Threads, Hash, plus tuning options from `SearchTuning.def`. No MultiPV, Chess960 or
  strength limiting.
- Per-iteration info: `depth score nodes hashfull time pv`. No `nps`, `seldepth`, `currmove` or
  `multipv`.
- Iteration nodes count the main thread only; the final line counts all threads and quiescence.
- The final info line carries a one-move PV.
- Engine-specific `info string` lines (`treenodes main N qs N`, search telemetry counters) can
  feed the StratChess statistics view later. TT and profile counters need instrumented builds; show
  them as unavailable, not zero.
- Scores are side-to-move centipawns; normalise the display perspective explicitly. Scores from
  different engines are not comparable.

## Engine issues

Fixed in the engine rather than worked around in the UI:

| Issue | Change | Needed by |
|---|---|---|
| #911 | Report `nps` and `seldepth` | M2 |
| #912 | Full PV on the final info line | M2 |
| #94 (reopened) | MultiPV | M3: hint alternatives, weakened opponent |
| #913 | Beginner strength: probe depth 1-2 and small `nodes` budgets, then a skill option if needed. Largest risk to M3 | M3 |
| #914 | `currmove`/`currmovenumber` (optional) | M2 |

## External dependencies

All npm packages, pinned by a committed lockfile. Runtime licenses are GPL-compatible.

| Dependency | Role | License | Update cadence |
|---|---|---|---|
| Node.js (LTS) | Server runtime | MIT | New LTS line yearly; each supported ~30 months |
| TypeScript | Language, shared types | Apache-2.0 | Minor releases ~quarterly |
| Fastify + `@fastify/static` + `@fastify/websocket` | HTTP, static assets, WebSocket, Host/Origin hooks | MIT | Major every ~2 years |
| chessops | Rules on server and browser | GPL-3.0+ | Slow, stable |
| Chessground | Board | GPL-3.0+ | Slow, stable (Lichess-maintained) |
| React | UI | MIT | Major every ~1-2 years |
| Mantine | Controls, dialogs, forms | MIT | Major ~yearly |
| *Dev:* Vite | Build, dev server | MIT | Major ~yearly |
| *Dev:* Vitest | Unit tests, fake-engine transcripts | MIT | Tracks Vite |
| *Dev:* Playwright | Browser and touch tests | Apache-2.0 | Monthly; bundles browsers |
| *Dev:* ESLint + Prettier | Lint, format | MIT | ESLint major ~yearly |

Not dependencies: StratChess and Stockfish are separately installed executables, configured as
engine profiles.

## Maintenance

The JavaScript ecosystem changes faster than this repo's C++ dependencies. The churn sits mostly
in build tooling (Vite, ESLint, TypeScript) rather than in the chess or server code.

- **Updates:** Dependabot opens one grouped minor/patch PR per month; merge it when CI is green.
  Take majors deliberately, one at a time. Expect two or three small migrations a year.
- **Urgency is low.** The app is LAN-only, has no accounts and no internet exposure, so a skipped
  month costs nothing. Quarterly is fine; `npm audit` findings are informational unless they affect
  the server's request handling.
- **Node:** move to the next LTS line about every two years, before the current one reaches end
  of life.
- **Keep the list short.** Every added package is future update work: prefer the platform and the
  libraries above over adding a new one.

## Open questions (later milestones)

- **M2:** read-only transcript or manual UCI entry (manual entry can desync the controller)? Does
  FEN load start engine play immediately? Does the human side default to the side to move? PGN
  export (recommended: retains page-bound games without a database).
- **M3:** hint form (piece, move or line) and its engine/limit; takeback = one ply or back to the
  human's previous turn; resign and draw offers; how difficulty is named.
- **Analysis:** linear move navigation or variation tree; continuous analysis; multiple engines.
- **Engine vs engine:** per-side profiles and limits; bundled Stockfish (GPL source obligations) or
  user-supplied binary.
- **Delivery:** documented developer launcher first; installer is a separate requirement.
- **Accessibility and keyboard** requirements.
- **CI:** integration tests need a StratChess binary; pin a released build or use a fake engine
  replaying recorded transcripts (recommended for unit tests anyway).

## Research appendix

**Runtime options considered.** Local server with native engines (chosen). Hosted service: brings
accounts, compute and isolation for no stated need. Tauri: desktop packaging only, cannot serve
phones. Browser-only WASM: cannot run native binaries; StratChess would need a WASM build.
Stockfish.js could later be an optional browser opponent; its strength and speed are not comparable
to native.

**Components.**

| Component | License | Notes |
|---|---|---|
| [react-chessboard](https://github.com/Clariity/react-chessboard) | MIT | React, drag/drop, mobile; check touch and promotion in a probe |
| [cm-chessboard](https://github.com/shaack/cm-chessboard) | MIT | Framework-neutral SVG, markers, promotion, accessibility extension |
| [Chessground](https://github.com/lichess-org/chessground) | GPL-3.0+ | Lichess board; arrows, premoves, excellent touch; combined work must be GPL |
| [chess.js](https://github.com/jhlywa/chess.js) | BSD-2 | Rules, legal moves, game end, FEN/PGN |
| [chessops](https://github.com/niklasf/chessops) | GPL-3.0+ | Variants, PGN trees; pairs with Chessground |
| [python-chess](https://python-chess.readthedocs.io/en/stable/engine.html) | GPL-3.0+ | Mature async UCI engine management |
| [Mantine](https://mantine.dev/) / [shadcn/ui](https://ui.shadcn.com/docs) | MIT | UI controls; use a small subset |
| [Playwright](https://playwright.dev/docs/intro) | Apache-2.0 | Browser tests: promotion, castling, illegal drops, touch, reconnect |

The UI is GPL-3.0, so all of the above are usable. Distributing Stockfish binaries still requires
their corresponding source. Board artwork can carry its own attribution terms.

**Reference app.** [En Croissant](https://github.com/franciscoBSalgueiro/en-croissant) (Tauri,
GPL-3.0): desktop only, so it cannot serve phones. Try it for interaction ideas, not as a fork base.
