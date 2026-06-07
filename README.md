# poker-server 🃏

> Multiplayer Texas Hold'em poker engine built in C — concurrent TCP server, custom binary protocol, and an interactive ncurses TUI client

## What It Does

A from-scratch implementation of a 6-player Texas Hold'em poker server. Clients connect over TCP, exchange binary packets for all game actions, and play full hands including preflop, flop, turn, and river betting rounds with complete hand evaluation at showdown.

## Architecture

```
src/
├── server/
│   ├── poker_server.c          # TCP server, game loop, socket management
│   ├── game_logic.c            # Deck, dealing, betting rounds, hand evaluation
│   └── client_action_handler.c # Packet validation, action processing
├── client/
│   ├── automated.c             # Scripted client (reads actions from stdin)
│   └── TUI/client.c            # Interactive ncurses TUI with mouse support
└── shared/
    ├── utility.c               # Card encoding/decoding utilities
    └── logs.c                  # Per-player file logging system
```

## Key Technical Details

**Networking** — Server binds to 6 ports (2201–2206), one per player. Each client connects, sends a JOIN packet, then participates in the full game loop over a persistent TCP connection.

**Binary Protocol** — All communication uses fixed-size C structs sent directly over sockets. Packet types include JOIN, LEAVE, READY, RAISE, CALL, CHECK, FOLD (client→server) and ACK, NACK, INFO, END, HALT (server→client).

**Game Logic** — Full Texas Hold'em: deck initialization with deterministic seeding, Fisher-Yates shuffle, dealing, four betting rounds (preflop/flop/turn/river), and a hand evaluator covering straight flush, four-of-a-kind, full house, flush, straight, three-of-a-kind, two pair, one pair, and high card.

**Betting System** — Validates RAISE/CALL/CHECK legality, tracks per-player bets and pot size, detects betting round completion when all active players have matched bets.

**ncurses TUI** — Interactive client with mouse-tracked buttons (CHECK/CALL/RAISE/FOLD), player panels, community card display, and pot/bet info. Bet amounts entered via keyboard prompt.

**Testing** — GoogleTest-based log comparison: runs scripted games with fixed seeds and diffs per-player log output against expected files across 4 test suites.

## Build & Run

```bash
# Build the server
make server.poker_server

# Build the automated client
make client.automated

# Build the TUI client (requires libncurses-dev)
make tui.client

# Run a test scenario (fixed seed, scripted players)
bash scripts/tests/test1/test1.txt
```

## Test Suites

| Test | Players | Scenario |
|---|---|---|
| test1 | 6 (all check) | 1 hand, all players check every street |
| test2 | 6 (all check) | 2 hands, dealer rotation verified |
| test3 | 2 active (4 leave) | Raises, calls, and folds with sparse players |
| test4 | 5 active (1 leave) | Multi-hand with complex betting, NACK handling |

## Stack

`C` · `POSIX Sockets` · `ncurses` · `GoogleTest` · `CMake` · `GCC` · `Linux`

## Author

**Gatik Yadav** — CS + AMS @ Stony Brook University  