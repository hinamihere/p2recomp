#!/usr/bin/env bash
# One-time setup of the CI checkout used by the self-hosted GitHub runner (.github/workflows/decomp-check.yml).
# Creates C:\p2r-ci (ASCII path: Ninja needs one) as a clone of this repository whose game-data directories are junctions
# to this checkout, so nothing copyrighted is copied or committed, then does the first full build (build/ci, -O1, ~5 min).
#
# usage (from the main checkout): bash tools/ci_setup.sh [target=C:/p2r-ci]
set -euo pipefail
cd "$(dirname "$0")/.."
src_win=$(pwd -W)
dst=${1:-C:/p2r-ci}
remote=$(git remote get-url origin)
if [ ! -d "$dst/.git" ]; then
    git clone -q "$src_win" "$dst"
    git -C "$dst" remote set-url origin "$remote"
fi
for d in extracted bin states memstick tools/third_party; do
    [ -e "$dst/$d" ] && continue
    mkdir -p "$(dirname "$dst/$d")"
    powershell -NoProfile -Command "New-Item -ItemType Junction -Path '$(cygpath -w "$dst/$d")' -Target '$(cygpath -w "$src_win/$d")'" >/dev/null
done
cd "$dst"
export PATH=/c/mingw64/bin:$PATH
bash tools/regen.sh >/dev/null
cp game/overrides.txt generated/.overrides_used
cmake -S . -B build/ci -G Ninja -DCMAKE_BUILD_TYPE=Release -DP2_GENERATED_OPT=-O1 >/dev/null
cmake --build build/ci --target p2 | tail -1
echo "CI checkout ready at $dst"
