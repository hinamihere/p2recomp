#!/usr/bin/env bash
# Regenerates the recompiled C++ for bin/EBOOT.dec into generated/.
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH=/c/mingw64/bin:$PATH
cmake -S framework -B build/framework -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSPRECOMP_PROFILE="" -DPSPRECOMP_BUILD_TESTS=OFF >/dev/null
cmake --build build/framework --target psp_recomp
build/framework/psp_recomp.exe bin/EBOOT.dec --auto generated 0x08804000 0x4000
