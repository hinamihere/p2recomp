# p2recomp — Persona 2: Innocent Sin (PSP) static recompilation

A native Windows build of *Shin Megami Tensei: Persona 2 – Innocent Sin* (PSP, ULES-01557, Europe),
produced by statically recompiling the game's Allegrex (MIPS) executable to C++ and running it
against a host reimplementation of the PSP system software.

No game data is included or produced in this repository. You need your own copy of the ISO.

## Status

Playable from boot into the story:

- intro logos, opening anime movie (H.264 + ATRAC3+), title screen
- New Game, name entry, story scenes with dialogue and portraits
- field exploration (3D map, camera), school interiors, NPC conversations, main menu, automap
- background music (ATRAC3+ streaming with loops), sound effects (SAS with reverb), movie audio
- saves stored on the host (`memstick/PSP/SAVEDATA/...`)
- runs at the game's native 30 fps; fast-forward (Tab) and savestates (F5/F8)

Verified by scripted play through the whole opening chain at Seven Sisters High, the Persona
awakening (movie, Philemon scene) and the first scripted battle against the Joker (fusion spell,
battle effects). Not verified yet: random-encounter battles in a dungeon, saving at save points. Known gaps: Bezier/spline surfaces, utility dialogs other than savedata are
auto-confirmed, the renderer is a software rasterizer (primitives are batched and rasterized in
horizontal bands on several threads).

## Why a recomp and not a matching decomp

The EBOOT was built with Sony's SDK compiler (`GCC 4.0.4+allegrex psp-2.7.3`, per its `.comment`
section), which is not publicly available, so a byte-matching C decompilation cannot be compiled.
Static recompilation gives a working native program now; the recompiled code can later be replaced
function-by-function with hand-written C as a non-matching decomp.

## Layout

| Path | What |
|------|------|
| `framework/` | [PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp) (MIT): ELF/PRX loader, Allegrex decoder, C++ code generator, dispatch runtime. Vendored with local fixes (see git log). |
| `game/src/` | The host: PSP kernel HLE (threads, sync, interrupts, memory), file IO, display/input (SDL2), GE display lists + software rasterizer, sceAudio mixer, SAS, ATRAC3+, MPEG movies, savedata. |
| `game/third_party/at3_standalone/` | ATRAC3/ATRAC3+ decoder from PPSSPP's `ext/at3_standalone` (FFmpeg-derived, LGPL-2.1+). |
| `tools/` | Extraction, decryption, NID table, disassembly and codegen helper scripts. |
| `generated/` | Recompiled C++ (generated locally, git-ignored). |

## Building

Requirements (Windows): MinGW-w64 GCC 13+ with CMake and Ninja (`C:\mingw64\bin`), Python 3,
Git Bash. Third-party binaries go in `tools/third_party/`:
SDL2 2.32 MinGW development package and an FFmpeg LGPL shared build (BtbN `n8.1` win64).

**Build from a path without non-ASCII characters** (e.g. a junction such as `C:\p2r`): Ninja cannot
track dependencies under paths it cannot represent and then rebuilds everything every time.

```bash
python -m venv .venv && .venv/Scripts/python -m pip install pycdlib pyelftools rabbitizer
# 1. Extract the ISO (path to your own copy)
.venv/Scripts/python -I tools/extract_iso.py "<game>.iso" extracted
# 2. Decrypt the EBOOT (tools/build/prxdec.exe is built from tools/src/prxdec.cpp + pspdecrypt)
tools/build/prxdec.exe extracted/PSP_GAME/SYSDIR/EBOOT.BIN bin/EBOOT.dec
# 3. Recompile to C++
bash tools/regen.sh
# 4. Build (P2_GENERATED_OPT=-O1 compiles the generated code faster for development)
cmake -S . -B build/ascii -G Ninja -DCMAKE_BUILD_TYPE=Release -DP2_GENERATED_OPT=-O2
cmake --build build/ascii --target p2
```

## Running

Double-click `play.bat`, or:

```bash
build/ascii/p2.exe --eboot bin/EBOOT.dec --disc extracted
```

The ISO is found automatically in the working directory or one level below (`--iso` to override).

| Option | |
|--------|---|
| `--ms <dir>` | memory stick root for saves (default `memstick`) |
| `--scale N` | window scale (default 2) |
| `--smooth` | bilinear upscaling instead of sharp pixels |
| `-v` / `-vv` / `-q` | more / trace / less logging |
| `--headless` | no window or audio (audio queues drain in game time) |
| `--dump-frames N` | write every Nth frame to `build/frames/` |
| `--dump-audio <file>` | record the final audio mix (raw s16 stereo 44.1 kHz) |
| `--input "<vblank>=BUTTON[+BUTTON][:frames],..."` | scripted input for unattended tests |
| `--exit-after <vblank>` | stop cleanly after N vblanks |
| `--turbo-until <vblank>` | fast-forward until that vblank |
| `--load-state N` | start from savestate slot N |
| `--load-state-at <vblank>[:slot]` | load a state at that vblank, like pressing F8 (tests) |
| `--save-state-at <vblank>[:slot]` | write a savestate at that vblank (scripted tests, `tools/drive.py`) |

Scripted play from savestates: `tools/drive.py` (load a slot, play input, save another slot,
last frame and a contact sheet in `build/drive/`), `tools/explore.py` (random walk until the
location banner changes) and `tools/talkhunt.py` (random walk until a conversation opens).

Debug environment variables: `P2_GE_TRACE=<vblank>[-<vblank>]` (log draws and dump textures),
`P2_THREAD_DUMP=<vblank>`, `PSPRECOMP_HLE_HISTOGRAM=1`, `P2_PROFILE=1` (sampling profiler, then
`tools/profile.py build/release/p2.exe`), `P2_RASTER_THREADS=N` (default: cores - 2, at most 4),
`P2_RASTER_VERIFY=1` (check banded against sequential rasterization), `P2_RASTER_STATS=1`.
`P2_MPEG_DUMP=<file>` (raw movie stream + per-picture checksums), `P2_FFMPEG_LOG=1`,
`P2_WATCH=<hex addr>,...` logs guest float32 values every vblank (`tools/ramdiff.py` finds candidates
such as the player position, 08cd4190/94/98 in the field).

Controls: arrows = D-pad, Z = Cross, X = Circle, A = Square, S = Triangle, Q/W = L/R,
Enter = Start, Backspace = Select, F11 or Alt+Enter = fullscreen, Esc = quit.
Hold Tab to fast-forward. F5 = savestate, F8 = load savestate (Shift for a second slot);
states go to `states/`. A savestate is taken at the next safe point.
XInput/SDL gamepads are supported.
