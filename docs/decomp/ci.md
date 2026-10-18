# Checking replacement functions (CI)

Every hand-written replacement of a recompiled function is checked against the original code before it is merged.
The check needs the game data (decrypted EBOOT, recompiled code, savestates), which never goes into this public
repository, so it runs on a self-hosted GitHub runner on the maintainer's PC.

## What a PR may contain

A decomp PR touches only:

- `game/src/native_funcs.cpp`: the replacement (an ordinary typed C++ function) and its `P2_NATIVE(<ADDR>, name)` or
  `P2_NATIVE_VOID(<ADDR>, name)` binding line at the bottom
- `game/overrides.txt`: one line with the entry address and a short name
- `docs/decomp/`: notes (structs, what the function does)

Anything else fails the `gate` job unless the maintainer adds the `allow-infra` label.

## What the check does (`tools/ci_check.sh`, about a minute)

1. Regenerates the recompiled C++ if `game/overrides.txt` changed, then builds incrementally (`build/ci`, `-O1`).
2. Differential fuzz: every replacement the PR touches (`tools/ci_changed.py`) is called 200 times with random
   arguments and RAM, next to the original on identical state. All registers and all of RAM must match.
3. Savestate regression: `tools/regress.py compare` replays a 600-vblank window (scripted input: Cross every 45
   vblanks plus d-pad holds) from 24 milestone savestates spread over the whole game. Each window runs once with the
   original code only and once with the replacements, with `P2_DETERMINISTIC=2`, so the guest clock no longer
   depends on the host and the results must be byte-identical.

Measured: clean pass 26 s; a deliberately wrong `rand` fails at step 2 after 8 s.

When step 3 fails, `python tools/regress.py verify <slot>` replays the window with `P2_NATIVE=verify` and names
the replacement that mismatched, with the first differing register or address. It is slower, since it compares all of
RAM on every call.

Deeper checks that are too slow for every PR:
- `python tools/regress.py compare` runs all milestone savestates (228, about 5.5 min).
- `P2_NATIVE_FUZZ=2000 build/ci/p2.exe --headless` fuzzes every replacement (about 10 ms per call).

## One-time setup on the maintainer's PC

1. `bash tools/ci_setup.sh` (from the main checkout) creates `C:\p2r-ci`. That is a clone whose `extracted/`,
   `bin/`, `states/`, `memstick/` and `tools/third_party/` are junctions to the main checkout. It then does the
   first full build, about 10 minutes.
2. GitHub, repository Settings, Actions, Runners, New self-hosted runner, Windows. Follow the download and
   `config.cmd` steps it shows, add the label `p2`, and run it as a service (or `run.cmd`).
3. Repository Settings, Actions, General: "Require approval for all outside collaborators" (or stricter). The
   workflow already skips the self-hosted job for PRs from forks. The repository is public and the runner executes
   the PR's code on your machine, so only branches pushed to this repository by you or your agents should reach it.
