# p2recomp — Persona 2: Innocent Sin (PSP) static recompilation

A native Windows build of *Shin Megami Tensei: Persona 2 – Innocent Sin* (PSP, ULES-01557, Europe),
produced by statically recompiling the game's Allegrex (MIPS) executable to C++ and running it
against a host reimplementation of the PSP system software.

No game data is included or produced in this repository. You need your own copy of the ISO.

## Why a recomp and not a matching decomp

The EBOOT was built with Sony's SDK compiler (`GCC 4.0.4+allegrex psp-2.7.3`, per its `.comment`
section), which is not publicly available, so a byte-matching C decompilation cannot be compiled.
Static recompilation gives a working native program now; the recompiled code can later be replaced
function-by-function with hand-written C as a non-matching decomp.

## Layout

| Path | What |
|------|------|
| `framework/` | [PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp) (MIT): ELF/PRX loader, Allegrex decoder, C++ code generator, dispatch runtime. Vendored with local fixes (see git log). |
| `game/src/` | The host: PSP kernel HLE (threads, sync, interrupts, memory), file IO, display/input (SDL2), GE display lists + software rasterizer, sceAudio mixer, SAS, ATRAC3+, stubs for MPEG and utility dialogs. |
| `game/third_party/at3_standalone/` | ATRAC3/ATRAC3+ decoder from PPSSPP's `ext/at3_standalone` (FFmpeg-derived, LGPL-2.1+). |
| `tools/` | Extraction, decryption, NID-table and codegen helper scripts. |
| `generated/` | Recompiled C++ (generated locally, git-ignored). |

## Building

Requirements (Windows): MinGW-w64 GCC 13+ with CMake and Ninja (`C:\mingw64\bin`), Python 3,
Git Bash. SDL2 2.32 MinGW development files go in `tools/third_party/sdl2/`.

```bash
python -m venv .venv && .venv/Scripts/python -m pip install pycdlib pyelftools rabbitizer
# 1. Extract the ISO (path to your own copy)
.venv/Scripts/python -I tools/extract_iso.py "<game>.iso" extracted
# 2. Decrypt the EBOOT (tools/build/prxdec.exe is built from tools/src/prxdec.cpp + pspdecrypt)
tools/build/prxdec.exe extracted/PSP_GAME/SYSDIR/EBOOT.BIN bin/EBOOT.dec
# 3. Recompile to C++
bash tools/regen.sh
# 4. Build
cmake -S . -B build/game -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/game --target p2
```

## Running

```bash
build/game/p2.exe --eboot bin/EBOOT.dec --disc extracted --iso "<game>.iso"
```

Options: `--ms <dir>` memory stick root (saves), `--scale N` window scale, `-v`/`-vv` verbose logs,
`--headless` no window.

Controls: arrows = D-pad, Z = Cross, X = Circle, A = Square, S = Triangle, Q/W = L/R,
Enter = Start, Backspace = Select, Esc = quit. XInput/SDL gamepads are supported.

## Status

See the git log for progress. Missing pieces: MPEG movie playback, savedata dialog, reverb,
Bezier/spline patches.
