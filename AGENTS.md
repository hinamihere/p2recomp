# Instructions for coding agents

This repository is a static recompilation of Persona 2: Innocent Sin (PSP) that is being turned into readable C++,
one function at a time. Background: `README.md`, `docs/decomp/README.md`.

Game data (the ISO, the decrypted EBOOT, the generated C++, savestates, disassembly) must never be committed or pasted
anywhere outside this machine. `.gitignore` already excludes it. Work packets in `build/packets/` contain game
disassembly: use them, never commit them.

## Your workspace

Never work in the maintainer's main checkout. Make your own once:

```bash
bash tools/ci_setup.sh C:/p2r-<your-name>
```

This clones the repository to an ASCII path (Ninja cannot build under the non-ASCII user folder), links the game data
in through junctions, and does the first full build, about 10 minutes. Work, build and run checks only there. Start
every task from an up-to-date `origin/master` on a new branch.

## Decompiling a function

1. Pick a `todo` function from `docs/decomp/functions.csv`, or take the one you were given. Generate its packet with
   `.venv/Scripts/python tools/packet.py <ADDR>` (run it from the main checkout if your workspace has no `.venv`). Read
   `build/packets/<ADDR>.md` completely: it has the disassembly, callers, callees and the rules for the code.
2. Write the replacement in `game/src/native_funcs.cpp`, add its `P2_NATIVE(...)`/`P2_NATIVE_VOID(...)` binding at the
   bottom and its line in `game/overrides.txt`. Touch no other file except notes in `docs/decomp/`. The PR check
   rejects anything else.
3. Commit, then run the check from your workspace:

   ```bash
   bash tools/ci_check.sh origin/master
   ```

   It takes about a minute: incremental build, differential fuzz of your function against the original (identical
   registers and RAM required), and 24 deterministic savestate replays over the whole game (byte-identical required).
   On a savestate failure, `python tools/regress.py verify <slot>` names the mismatching function.
4. Open the PR only when the check prints `p2-check: PASS ...` as its last line. Paste that **exact line, unedited**
   into the PR description where the template asks for it. It records the commit and the file hashes that were
   tested. Whenever you push a new commit, rerun the check and replace the line. The PR's check job compares it
   with the PR head and fails if it is missing, edited, from another commit or not a PASS.

Never edit `tools/ci_check.sh`, `tools/regress.py`, `tools/ci_changed.py`, the workflow or the fuzz settings to make a
check pass. If the check itself looks wrong, say so in the PR or to the maintainer.

## Tips

- Smallest first: `tools/packet.py --next 10 --leaf --tier S` writes packets for the ten smallest untouched leaf
  functions.
- The existing replacements in `game/src/native_funcs.cpp` show the style: typed parameters, `guest<T>()`, `Global<>`,
  `guest_call()`, comments that explain what the function is for.
- Name things after what they do in the game. Notes on structs and globals go in `docs/decomp/`.
