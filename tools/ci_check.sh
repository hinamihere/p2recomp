#!/usr/bin/env bash
# The ~1 minute correctness check for a change to the hand-written replacements (run by .github/workflows/decomp-check.yml
# on the self-hosted runner, or by hand). It needs the game data, so it runs in a checkout that has extracted/, bin/ and
# states/ (tools/ci_setup.sh creates one at C:\p2r-ci with junctions to the main checkout).
#
# usage: tools/ci_check.sh <base-ref>     e.g. tools/ci_check.sh origin/master
#   1. regenerate the recompiled C++ if game/overrides.txt changed (only units containing a changed entry get rewritten)
#   2. incremental build (build/ci, -O1)
#   3. differential fuzz of every replacement the change touches (tools/ci_changed.py), 200 random calls each,
#      original vs replacement on identical registers and RAM
#   4. tools/regress.py compare on 24 milestone savestates spread over the whole game: each window is replayed with the
#      original code only and with the replacements, deterministically (P2_DETERMINISTIC), and the results must be identical
# Exit status 0 = pass. On a regress failure, rerun `python tools/regress.py verify <slot>` to see which replacement mismatched.
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH=/c/mingw64/bin:$PATH
base=${1:-origin/master}
BUILD=${P2_CI_BUILD:-build/ci}
start=$(date +%s)
step() { echo "== [$(( $(date +%s) - start ))s] $*"; }

# Result stamp, the last line of the output: paste it into the PR. The decomp-check workflow compares it with the PR's
# head commit and the two replacement files, so a result from another commit (or none) is caught.
# Only committed work is stamped.
if [ -n "$(git status --porcelain -- game/src/native_funcs.cpp game/overrides.txt)" ]; then
    echo "game/src/native_funcs.cpp or game/overrides.txt has uncommitted changes: commit first (only committed work is stamped)"
    exit 2
fi
head=$(git rev-parse HEAD)
blobs="funcs=$(git rev-parse --short=12 HEAD:game/src/native_funcs.cpp) overrides=$(git rev-parse --short=12 HEAD:game/overrides.txt)"
result=FAIL fuzzed=none states=0
stamp() { echo "p2-check: $result head=$head $blobs fuzzed=$fuzzed states=$states"; }
trap stamp EXIT

for need in bin/EBOOT.dec states/KEEP extracted/PSP_GAME; do
    [ -e "$need" ] || { echo "missing $need: run tools/ci_setup.sh (needs your own game data)"; exit 2; }
done

# generated/ is regenerated whenever game/overrides.txt differs from the copy it was last generated with
if ! cmp -s game/overrides.txt generated/.overrides_used || [ ! -f generated/generated_registry.cpp ]; then
    step "regenerating recompiled code (overrides changed)"
    bash tools/regen.sh >/dev/null
    cp game/overrides.txt generated/.overrides_used
fi

step "building"
[ -f "$BUILD/build.ninja" ] || cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DP2_GENERATED_OPT=-O1 >/dev/null
cmake --build "$BUILD" --target p2 | tail -1
export P2_EXE="$PWD/$BUILD/p2.exe"

changed=$(python tools/ci_changed.py "$base")
if [ -n "$changed" ]; then
    fuzzed=$changed
    step "fuzzing changed replacements: $changed"
    set +e
    out=$(P2_NATIVE_FUZZ=200 P2_NATIVE_FUZZ_ONLY="$changed" "$P2_EXE" --headless 2>&1); status=$?
    set -e
    echo "$out" | grep -E "fuzz |MISMATCH" | head -20
    [ $status -eq 0 ] || { echo "FAIL: a replacement differs from the original on random inputs"; exit 1; }
else
    step "no replacement changed; skipping the fuzzer"
fi

n=${P2_CI_STATES:-24}
step "savestate regression ($n windows)"
python tools/regress.py compare --limit "$n" --jobs "${P2_CI_JOBS:-6}"
states="$n/$n"
result=PASS
step "PASS"
