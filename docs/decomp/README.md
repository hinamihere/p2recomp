# Decompiling Persona 2: Innocent Sin

The goal is readable C++ source for the whole game, built inside this static recompilation. Each recompiled
function is replaced one at a time by a hand-written equivalent. Every replacement is machine-checked against the
original code before it is merged, so the game stays playable at every step. A byte-matching decomp is not possible
here: the original compiler (GCC 4.0.4+allegrex, psp-2.7.3) is not public.

## The function list

`docs/decomp/functions.csv` (rebuild with `.venv/Scripts/python tools/funclist.py`) has every function in the
executable: its address, size, how it was found, call counts, leaf flag and status (`todo` or `replaced`).
It contains addresses and call-graph metadata only, no game code.

Current count: 7932 functions, 871,248 instructions.

| tier | instructions | count |
|---|---|---|
| S | 16 or fewer | 2322 |
| M | 17 to 64 | 2838 |
| L | 65 to 256 | 1905 |
| XL | more than 256 | 867 |

2841 are leaf functions (they call nothing). 25 are replaced. 345 `prologue` entries are unreferenced code,
probably dead, and are left for last.

Suggested order: S and M leaf functions first, then functions whose callees are all replaced, then the subsystems
that are already understood (event script interpreter, dungeon maps and movement, town maps, battle, menus) and
the engine last.

## Working on one function

1. `.venv/Scripts/python tools/packet.py <ADDR>` (or `--next 20 --leaf --tier S`) writes `build/packets/<ADDR>.md`.
   It holds the bounds, callers, callees, disassembly and the rules. Give it to the agent or person doing the work.
   Packets contain game disassembly, so they stay local and are never committed.
2. The result is a PR that touches only `game/src/native_funcs.cpp`, `game/overrides.txt` and `docs/decomp/`.
3. CI (`docs/decomp/ci.md`) fuzzes the changed replacements against the originals and replays 24 savestate windows
   over the whole game, in about a minute. Only green PRs get merged.

Several agents can work in parallel on different functions. Conflicts only happen in the two shared files, and
they are line-local (one binding line, one overrides line, one function each).
