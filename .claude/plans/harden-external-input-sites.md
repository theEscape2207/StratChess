# Harden External Input Sites

**Issue**: #178 — harden external input sites (UCI, FEN, JSON, argv)
**Status**: design approved, implementation pending

---

## Goal

Every malformed input reports what is wrong and exits cleanly. Today several report *and then
fail-fast anyway*, and one dies with no message at all.

**Threat model, from the issue and unchanged**: not network-facing, no privilege boundary. The goal
is **robustness**, not security review. Do not gold-plate.

**Scope limits**: no `/guard:cf` work (its own issue — it needs an nps measurement and an acceptance
check, which is an experiment rather than a bugfix). No `Game::Init` state refactor. No fix for
`ParsePlayerConfig`'s confusing `defaultEval`-as-default-type (behaviour is self-consistent; noted
in #178 for a later glance).

---

## What the survey established

All findings are from running the shipped clang-cl Release binary, recorded in #178's third comment.

| Input | Today | Wanted |
|---|---|---|
| `game_settings.json` truncated | precise message, then exit `0xC0000409` | same message, exit 1 |
| valid JSON, no `"game"` key | precise message, then `0xC0000409` | same message, exit 1 |
| `search_limits.depth` is a string | precise message, then `0xC0000409` | same message, exit 1 |
| `perft run abc` | **nothing at all**, `0xC0000409` | message on stderr, exit 1 |

`0xC0000409` is `STATUS_STACK_BUFFER_OVERRUN` — MSVC's fail-fast, i.e. `std::terminate`.

**The config diagnostics already work.** `Game::Init` catches, prints to `stderr`, then `throw;`
(`Game.cpp:138-145`) — that is #176's fix and it does its job. What is missing is only the clean
exit: nothing above `main` catches, so the rethrow reaches `std::terminate`.

**`Game::Init` is called from the constructor** (`Game.cpp:21-24`). When it throws, the object is
never fully constructed, so `~Game()` is **not** run. This is the fact that keeps the fix small: no
destructor-time cleanup of a half-built `Game` is needed, and `unsubscribePlayerEvents`'s
"assumes both players exist" TODO is unreachable from this path. Leave it alone.

---

## Design

### 1. Clean exit — catch in `main`, keep the rethrow

`Game::Init` continues to report and rethrow: a component that reports but lets the caller decide is
the right shape, and it means any future throw site inside `Init` is covered without further work.
`main` wraps the game-mode path:

```cpp
try {
    Game game;
    game.Run();
} catch (const std::exception& ex) {
    // Init already reported the specific cause; this is the backstop that makes
    // a bad settings file exit like a failed program instead of a crashed one.
    std::cerr << "Fatal: " << ex.what() << '\n';
    return 1;
}
return 0;
```

Deliberately **not** deleting `Init`'s `throw;` — that would let a half-initialised `Game` continue
into `Run()`, which is worse than the fail-fast it replaces.

### 2. `argv` — one shared, testable helper

`StratChessEvolved.cpp:183` (`perft run <depth>`) is the only unguarded numeric parse of user input.
The correct pattern already exists 57 lines above it in `tacticalrunner`, twice.

Rather than a third copy of a try/catch, extract it. `StratChessEvolved.cpp` belongs to no other
target and is **not linked into `StratChessTests.exe`**, so anything left in that file cannot be unit
tested — which is why the helper moves into the engine library:

```cpp
// StratEngine/Utils/ArgParse.h
namespace Engine {
/// Parse a whole decimal integer. Returns nullopt on anything else — empty,
/// trailing text, overflow. Never throws: the callers are argv and JSON keys,
/// where an exception is the failure being removed.
[[nodiscard]] std::optional<int> parse_int(std::string_view text) noexcept;
}
```

Call sites converted: `StratChessEvolved.cpp:126`, `141`, `183`, and `Perft.cpp:145`, `152`
(`std::stoi(it.key())` over `perft_test_cases.json` keys — repo-local data, but the same construct).
Each reports on `stderr` and returns non-zero / skips the entry.

Note `std::stoi` accepts trailing garbage (`"12abc"` → 12) and the existing guarded call sites
inherit that. `parse_int` rejects it, which is a small behaviour tightening and the reason the
existing sites move over too rather than being left alone.

### 3. UCI — refuse mutation during a search

`cmd_position` (`UCIHandler.cpp:218`) and `cmd_setoption` (`:325`) mutate state a running search
reads. `cmd_go` already calls `stop_and_join()` first.

**Decision: reject the command, do not abandon the search.** Both handlers return early with a
diagnostic when `search_thread_` is live.

`stop_and_join()` stays in `cmd_go` unchanged. It is **lifecycle, not policy** there — the
legitimate `go infinite` → `stop` → `bestmove` → `go` flow needs the previous thread reaped before a
new one spawns, and making `cmd_go` "ignore" a `go` would drop a valid command and hang the GUI
waiting for a `bestmove` that never comes.

Diagnostics go to **`info string` on stdout**, not only `stderr`: UCI has no error channel, and
`info string` is the convention GUIs surface in their engine log. `stderr` too, for a piped session.

Accepted consequence, stated so it is not rediscovered as a bug: after a refusal the GUI believes the
position changed while the engine holds the old one, so the engine answers for the position it
actually has. That is the conservative reading, and unlike today it is *announced*. Revisit if a real
GUI ever trips it.

### 4. Config — say when defaults are substituted

`ReadConfigFile` returns early when the file is missing, after one `std::cerr` line, and the players
then silently keep their defaults. The message becomes explicit that defaults are in use — issue
requirement 3.

No per-key `try`/`catch` is added. A wrong type already produces a precise nlohmann diagnostic
naming the key and the expected type, and with (1) it now exits cleanly. Wrapping each accessor would
add noise and lose that precision.

---

## Files Changed

| File | Change |
|---|---|
| `StratEngine/Utils/ArgParse.h` / `.cpp` | **New** — `Engine::parse_int` |
| `StratChessEvolved/StratChessEvolved.cpp` | `main` try/catch; 3 call sites use `parse_int` |
| `StratEngine/Tests/Perft.cpp` | 2 call sites use `parse_int` |
| `StratEngine/UCIHandler.cpp` | Refuse `position`/`setoption` during a search |
| `StratEngine/Config.cpp` | Missing-file message states defaults are used |
| `StratChessTests/ArgParseTests.cpp` | **New** — `[argparse]` |
| `StratChessTests/ConfigTests.cpp` | **New** — `[config]` negative cases |
| `StratChessTests/UCITests.cpp` | Mid-search refusal cases |
| `Docs/TestDesign.md` | Two new tags |
| `Docs/Changelog.md` | Entry |

---

## Step-by-Step

### Step 1 — `parse_int` + `[argparse]` tests

Reject: empty, whitespace-only, `"abc"`, `"12abc"`, `"1 2"`, `"+-3"`, values beyond `int`
(`"99999999999999"`), and `""`. Accept: `"0"`, `"-5"`, `"2147483647"`, leading `+`.
Implement over `std::from_chars` — no exceptions, no locale, and it rejects trailing text by
reporting where it stopped.

### Step 2 — call sites

Convert the five sites. Keep each site's existing range check (`n_runs >= 1`, `depth 0..10`) — those
are separate from parseability and their messages are already good.

### Step 3 — `main` try/catch

As above. Only the game-mode path needs it; the perft/tactical/eval runners already return status
codes, and `UciHandler::run()` is next to it — wrap the whole tail rather than one statement, so a
throw from any of them exits cleanly too.

### Step 4 — UCI refusal + tests

Guard both handlers on the search being live. `UCITests.cpp` already drives the handler, so the test
starts `go infinite`, sends `position`, asserts the refusal appears and that the board is unchanged,
then `stop`. If the harness cannot hold a search open deterministically, fall back to asserting the
guard's behaviour with the search flag set directly, and say so in the test comment.

### Step 5 — `[config]` negative tests

`Config(nullptr)` is safe for these: `pGame_` is dereferenced only on the FEN path
(`ReadFEN` → `SetCustomGame`), which none of these configs reach. Write each malformed document to a
temp file, then `REQUIRE_THROWS_AS(reader.ReadConfigFile(path, board), nlohmann::json::exception)`:

- truncated mid-object → `parse_error`
- valid JSON without `"game"` → `type_error`
- `search_limits.depth` as a string → `type_error`
- **and one positive control**: a minimal well-formed config parses and yields the expected depth,
  so the suite cannot pass by throwing for the wrong reason.

### Step 6 — docs

`TestDesign.md` rows for `[argparse]` and `[config]`; `Changelog.md` entry.

---

## Validation

1. `.\build.ps1 all -Config Debug`, then Release. Warnings are errors.
2. `Run-Tests.ps1 "[argparse]"`, `"[config]"`, `"[uci]"`, then the full fast tier.
3. `Validate-PrePR.ps1` — Engine tier.
4. **Re-run the four probes from #178's survey** and confirm each now exits 1 with a message:
   `perft run abc`, truncated JSON, JSON without `"game"`, `depth` as a string. This is the
   acceptance test; the unit tests do not cover the process exit code.
5. `perft test` still passes (Step 2 touches its JSON key parsing).

---

## Invariants After

1. No input path reaches `std::terminate`. Every malformed input exits non-zero with a message.
2. `Game::Init` still reports *and* rethrows; `main` is the only place that decides the exit code.
3. `cmd_go`'s `stop_and_join()` is unchanged — thread lifecycle is not touched by this work.
4. A rejected UCI command leaves engine state exactly as it was.
5. `parse_int` never throws.
6. `movetime`/`depth`/`infinite` search paths and all existing tactical/perft results are unchanged.
