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
- saves stored on the host (`memstick/PSP/SAVEDATA/...`), with a save/load slot picker in place of
  the PSP system dialog (Up/Down, Left/Right page, X confirm, O back; asks before overwriting)
- runs at the game's native 30 fps; fast-forward (Tab) and savestates (F5/F8)

Verified by scripted play through the whole opening chain at Seven Sisters High, the Persona
awakening (movie, Philemon scene) and the first scripted battle against the Joker (fusion spell,
battle effects). Saving and loading through the game's own menus is verified. Seven Sisters High (the first
dungeon) loads; a random encounter starts (flash transition, loading, battle scene) and the
demon contact/negotiation tutorial plays through, including the Contact menu and the reward.
A normal fight also runs end to end (Tactics menu, attacks with damage numbers, enemy turns, damage to
the party, victory and return to the dungeon). The per-character command menu works too: Skill (Agi cast with its
spell banner, effect and SP cost), Item (Medicine list), Persona (affinity screen with portrait), and the end of a
fight shows the Level Up screens with stat point distribution for each party member. The dungeon command menu
(Skills, Items, Equip, Persona, Status, Analyze, Battle settings, System) opens, and saving from the dungeon's
System menu works (slot picker, overwrite prompt, "Saving system data"). The Guard and Escape commands, the Fusion Spells screen (Hydro Boost with its Auto/Standby setting) and a shop purchase
(Toua Defense: buy list with prices, quantity, "That's gonna be 300 yen" confirmation, yen deducted) work, as do the city
map, the Rengedai and Hirasaka town maps and the transitions between them. Not verified yet: a story boss fight beyond
the scripted Joker fight, selling, the Velvet Room fusion, later chapters. Known gaps: Bezier/spline surfaces, utility dialogs other than savedata are
auto-confirmed, the renderer is a software rasterizer (primitives are batched and rasterized in
horizontal bands on several threads).

## Why a recomp and not a matching decomp

The EBOOT was built with Sony's SDK compiler (`GCC 4.0.4+allegrex psp-2.7.3`, per its `.comment`
section), which is not publicly available, so a byte-matching C decompilation cannot be compiled.
Static recompilation gives a working native program now; the recompiled code can later be replaced
function-by-function with hand-written C as a non-matching decomp.

## Hybrid decompilation (host replacements for guest functions)

Functions can be rewritten by hand and swapped in one at a time while the rest stays recompiled.

1. Write the replacement in `game/src/native_funcs.cpp` as an ordinary typed C++ function (guest structs are plain structs read
   through `guest<T>()`, fixed addresses are `Global<T, addr>` variables) and bind it with `P2_NATIVE(<ADDR>, function)`:
   arguments come from a0.. by their C++ types, the result goes to v0. No thread switches; `guest_call()` runs other guest functions.
2. List the entry address in `game/overrides.txt` and run `tools/regen.sh` (it passes the list to the code
   generator as `P2_OVERRIDES`). Only the units that contain a listed entry are rewritten.
3. The generated entry now calls the host function and then returns through `$ra`; every kind of call (same unit,
   cross unit, indirect) takes the same path. The original body stays reachable as `recomp_orig_<ADDR>`.
4. Verify: `P2_NATIVE=verify` runs original and replacement on identical state (registers and all of RAM are
   compared, mismatches are logged and the original's result is kept); `P2_NATIVE=off` runs the original only;
   `P2_NATIVE_VERIFY_LIMIT` / `P2_NATIVE_STRICT` tune it. `P2_NATIVE_FUZZ=<n>` runs every replacement n times on random
   arguments and RAM against the original before boot (inputs are shaped per function in `native_fuzz_setup`);
   it caught a wrong flag-array address that gameplay never exercised. `tools/callgraph.py` lists leaf functions and callers.

Replaced so far (24 functions, all fuzz-verified; `game/overrides.txt` has the list): the controller helper cluster at
0x08807918-0x08807B08 (held, pressed, mapped-button and d-pad queries), the game's `rand()`, a 3x3 fixed-point
vector*matrix multiply, sin/cos table lookups, pool and entity-table helpers (the object table is a header plus 0x54-byte entities at the pointer in 08CDD898) a few table getters, and the town-map cursor's road-choice routine (`map_cursor_pick_edge`: the nearest-angle edge of the node
within the stick direction's window, with the map graph structs documented in `native_funcs.cpp`). `tools/mapgraph.py`
dumps the town-map graph (nodes, edges, angles) from a savestate; `tools/showfn.py` prints a function's disassembly
together with the relocated constants from the generated C++.

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

Optimization level of the generated code (measured on one 16 KB unit / on 2500 frames of the field scene in turbo):
`-O0` compiles in 6 s but runs about 9x slower (it also needs the 128 MB stack the link options reserve, or the
chained guest calls overflow it); `-O1` compiles in 11 s and runs as fast as `-O2` (3.4 s vs 3.6 s); `-O2` takes
22 s per unit. `-O1` is the sweet spot for development (a full build is roughly half of `-O2`'s ~10 minutes; linking
is a fixed cost either way).

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
| `--exit-after-save` | stop as soon as the scheduled `--save-state-at` state has been written |
| `--save-state-at <vblank>[:slot]` | write a savestate at that vblank (scripted tests, `tools/drive.py`) |

Scripted play from savestates: `tools/drive.py` (load a slot, play input, save another slot,
last frame and a contact sheet in `build/drive/`), `tools/explore.py` (random walk until the
location banner changes) `tools/talkhunt.py` (random walk until a conversation opens), `tools/menuhunt.py` /
`tools/is_mall.py` (find/detect destination menus), `tools/labelmap.py` / `tools/labelhunt.py` /
`tools/labelpos.py` (find place labels on town maps), `tools/ramdiff.py` (RAM diffs between states).

Debug environment variables: `P2_GE_TRACE=<vblank>[-<vblank>]` (log draws and dump textures),
`P2_THREAD_DUMP=<vblank>`, `PSPRECOMP_HLE_HISTOGRAM=1`, `P2_PROFILE=1` (sampling profiler, then
`tools/profile.py build/release/p2.exe`), `P2_RASTER_THREADS=N` (default: cores - 2, at most 4),
`P2_RASTER_VERIFY=1` (check banded against sequential rasterization), `P2_RASTER_STATS=1`.
`P2_SAVEDATA_AUTO=1` (skip the slot picker: newest save / first free slot), `P2_MPEG_DUMP=<file>` (raw movie stream + per-picture checksums), `P2_FFMPEG_LOG=1`,
`P2_EXPLORE=1` (or `<xaddr>,<zaddr>`; `P2_EXPLORE_SEED`, `P2_EXPLORE_LOG=1`, `P2_EXPLORE_AVOID="x0,z0,x1,z1"`) runs an in-game
dungeon exploration bot: it reads the party leader's position (x at 08CD4190, z at 08CD4198), walks the four d-pad directions
towards the least visited neighbouring grid cell, turns away from walls it bumped into, stays out of the avoid rectangle and
taps Cross (doors, dialogue). Battle and menu screens are recognised from the frame contents (red UI bar on top or red party
panels at the bottom, with a 2 s hysteresis): there it only taps Cross, which fights with the default commands. When a party
member is below half HP it drinks Medicine through the Triangle menu (three times at most). It does not avoid fights it cannot
win; `P2_EXPLORE_CHECKPOINT=<slot>` saves a savestate every 1500 vblanks while the party is healthy and rolls back to it
(with a different random route) when two members are knocked out or the Medicine is used up and someone is badly hurt. `tools/bannerscan.py` lists the area banners that appear during a run
(seed 0 = the bot), `tools/bubblescan.py` finds the "!" interaction bubbles while walking, `tools/dungeonwalk.py` is the scripted
random-walk variant. Useful memory words: party HP/max HP/SP/max SP are u16 at 08E3CCBC + 0x54*member.

`P2_MAPPER=<file>` runs a dungeon mapping bot: it keeps a grid of 48-unit cells, plans routes through the edges it has learned to the nearest
cell with an unknown (or only once-seen blocked) direction, walks there and probes the direction by pushing into it, recording passable and
blocked edges, "!" interaction spots (the bubble above the leader), area exits (a jump of the position) and visited cells; battles are
fought with Auto-Battle (Triangle on the Tactics menu) and the walk resumes afterwards. `P2_MAPPER_SLOT` sets the rollback savestate slot,
`P2_MAPPER_LOG=1` logs the decisions, `P2_NO_ENCOUNTERS=1` turns off random encounters (the replaced field encounter check at 08962BE8 then
always reports "no encounter"), which makes mapping about ten times faster. `tools/mapview.py` renders the result as text/PNG. A full
first floor of the school (3,400 cells incl. the classrooms, 235 interaction spots) takes about ten minutes of emulation.

`P2_TOUR=<map>[,first,last]` walks to the interaction spots recorded in a mapper file (breadth-first over the known passable edges), faces
each, presses Cross and pages through the result; `tools/framesheet.py` makes a contact sheet of the dumped frames around given
vblanks. (The school's first-floor tour found the courtyard story scene where Ms. Ideal's statue moves.)

`tools/eventbin.py` unpacks the game's story scripts (`event.bin` = table of offsets + one gzip stream per script, 16-bit text with Atlus
control codes; format notes from github.com/chenetulipe/P2-FR-IS-PSP) and dumps all 13,000 dialogue lines as searchable text.

`tools/crawl.py <start-slot> [max-areas]` crawls a dungeon area by area: the mapper calibrates the stick directions and cell size of the area
automatically (the courtyard-style areas have a diagonal camera, the corridors a rotated one), maps it, `tools/spots.py`-style tours every
interaction spot with one run per spot (`P2_TOUR_AREA_SLOT` keeps a savestate when a door or stairs changes the area, `P2_TOUR_BANNER_FILE` saves
the location banner), and queues the new areas. Maps, banners and results are kept in `analysis/maps/` (crawl.json is the database).

`P2_WATCHPOINT=<hex addr>,...` (log every write to guest words with the writer's host address; map it
to guest code with a `-g1` build and `tools/profile.py`-style symbolization), `P2_WATCH=<hex addr>,...` logs guest float32 values every vblank (`tools/ramdiff.py` finds candidates
such as the player position, 08cd4190/94/98 in the field).

Controls: arrows = D-pad, Z = Cross, X = Circle, A = Square, S = Triangle, Q/W = L/R,
Enter = Start, Backspace = Select, F11 or Alt+Enter = fullscreen, Esc = quit.
Hold Tab to fast-forward. F5 = savestate, F8 = load savestate (Shift for a second slot);
states go to `states/`. A savestate is taken at the next safe point.
XInput/SDL gamepads are supported.

## Story progress notes (Seven Sisters High)

* Auto Map: **Square** in a dungeon opens the whole-floor map (current area, floor name, legend). Handy to find unexplored branches.
* The camera is rotated: the HUD compass shows S at the top, so screen-UP is south, screen-DOWN is north, screen-LEFT is east.
* Principal's Office (script 008, then 009 where Maya and Yukino join): 3F, west end of the main corridor 3-D..3-A, go north (screen DOWN) from the vestibule, door on the corridor's side. The scene plays correctly (portraits, Persona summon effects, party grows to five).
* `tools/doorwalk.py <slot> <LEFT|RIGHT> <tries> <png>` walks a corridor pressing UP+Cross at every step and sheets the results.
* `P2_IOTRACE=1` logs every sceIoRead (fd, offset, size, caller).

## Classroom scene packages (decoded)

Classrooms and other interiors are not F-files: each is one `event.bin` entry (`tools/eventbin.py` unpacks them to
`build/event/script_NNN.bin`). `tools/evscene.py <script_NNN.bin> parts|triggers|collision|labels|dis [LABEL|all]|flags`
decodes a package: a `u32` part count and part sizes, then the parts 16-byte aligned: 0 textures, 1 the `NNNN.smd` room model,
2 an 8 KB work area, 3 (1 KB), 4 collision, 5 door props (24-byte records), 6 (4 bytes), 7 triggers, 8 the script, 9 `PSPCHECK`.
Parts 4 and 7 share one layout: seven `(count, offset)` pairs for points (`x, y, z` as `i16`, room units: the leader's position is the
`i32` triple at `0x08e44c14`), segments (two point indices + 8-byte tag), boxes (16 bytes) and quads (four point indices + 8-byte tag);
a trigger tag's byte 0 is the script label number and byte 1 the required facing (0..255 = 0..360 degrees).

The script is 32-bit bytecode: header `(0x18, 0x18, label count, index offset, code offset, text offset)`, 72-byte labels
(name + instruction index), an index of `(opcode, argument offset)` pairs and argument words (bit 31 = register, else a 31-bit
signed value). Opcodes seen: `0x5e` set flag, `0x60` load flag into a register, `0x01`/`0x02` jump if set/clear, `0x07` jump,
`0x31` place NPC (char, model, x, y, z, facing), `0x2c` place the party, `0x46` bind a character to a label, `0x51` warp
(0, floor, door, 30, 0), `0x13` show text, `0x0e` end of handler.

Story flags are a bit array at `0x08e3b414` (flag `n` = byte `n/8`, bit `n%8`, LSB first). The emblem-clock hunt: flag 260 = hunt
active, 267 = hunt finished (set by script 010); each clock room's `MONMON` handler (trigger quad at x 536..605, z -60..-20 in front of the
blackboard, facing 0x38) sets its room flag - 250: 1079, 254: 1080, 256: 1081, 265: 1082, 396: 1083, 272: 1084, 276: 1085 - and when
all seven are set also flags 266 and 1094. Pre-setting a room's bit with `P2_POKE` makes `MONMON` take the "already destroyed" branch.
In the `0005.smd` classrooms the stick maps to the room axes diagonally: DOWN+LEFT = -x, DOWN+RIGHT = -z, UP+LEFT = +z,
UP+RIGHT = +x; Yukino stands at (667, -38) next to the clock zone, so approach it from below the teacher's dais.
`tools/clockroom.py <door-entry slot> <out slot>` smashes the clock of a `0005.smd` classroom from the state just inside door 1
(input is ignored for the first ~300 vblanks after entering): DOWN+RIGHT 6, DOWN+LEFT 55, UP+LEFT 35, UP+RIGHT 14, UP+LEFT 3, Cross.

### Door selectors: branch.bin

Pressing Cross at a door (face it: DOWN then Cross on the corridor's north wall) makes the game read one 2 KB blob of
`branch.bin` (blob b_NNN at CPK position 220485632 + 0x800 * (NNN + 1)) and run it: `tools/branchbin.py build/cpk/branch.bin [k]` decodes them.
Each blob is a tiny event-bytecode script that loads story flags and ends in op `0x96`: `(0, N)` runs event.bin script N,
`(1, n)` enters room n of the dungeon's room list, no `0x96` on the taken path = the door does nothing. In Seven Sisters High
NNN = door cell id - 7 (the `0xC8nn` cells of the C3 layer in F0097 chunk 0): blob 10 is the Principal's Office (flag 260 clear ->
script 008), blob 11 the Teachers' Lounge (flag 778 set, 266 set, 267 clear -> script 010, the emblem-hunt finale with
Ms. Saeko; 267 set -> the plain lounge). So the clock hunt shortcut is: set flags 1079-1085 and 266 in the bit array at
`0x08e3b414`, then enter the lounge (3F, cell 18 at columns 20-21 of row 2, the branch next to the Principal's Office).
Verified at the real door too: the Teachers' Lounge is on 2F (its room scripts 269/270 warp back with `(0, 2, 2, 7, 4)`),
at the north end of the branch off the west end of the 2F main corridor. Two male students placed by the 2F NPC records
(characters 152/153, gated on flag 1094) stand in front of it saying the teachers are in an emergency meeting; flag 1094 must be
set before the floor loads for them to be gone. Walking recipe from the 2-B door landing (state 2330/2590, encounters off):
`UP:14, LEFT:260, UP:500` reaches the door nook ("Teachers' Lounge" banner), then `RIGHT:20, UP:6, CROSS` opens it. With flags
1079-1085, 266 and 1094 set that starts script 010 (Ms. Saeko); without them the students block the door and say
"you can't go in there right now". The remap test (`P2_IO_REMAP=220493824,220510208` at the 2-B door) gives the same result.

After the lounge: script 010's KYOSEI2 block sets flags 267, 473 and 1093 once its three clocks are smashed. The janitor's
room is 1F door 2 (room scripts 258, warp `(0, 3, 2, 7, 4)`); its selector b_007 runs script 011 (the clock-tower-key search)
when 267 and 473 are set and 261 clear, else enters room 258 - verified at the door (warp there with those flags held, then
DOWN + Cross). Script 011 sets 261, 268, 526 and 1367 (no item opcode seen). The Clock Tower is on 4F, reached by the stairs
branch at column 20 of the 3F main corridor (warp door id 41, then UP 150 up the stairs). 4F is a hall (x 112..2944,
z 3384..4440) with a corridor down its west side to the "Clock Tower" section (z 5832). The tower entrance is a walk-in
trigger, not a Cross door: from (928, 5304) on that corridor's east wall, DOWN+RIGHT walks into the doorway, which reads selector
b_012 and runs script 012 (the boss scene) unconditionally - it fires with or without the key flags, so the key is not
checked at the door (states 2800/2801 = just outside it, with/without flags). Script 013 follows (sets 269, 270).

`P2_IO_REMAP=<from>,<to>` (decimal file positions, as printed by `P2_IOTRACE=1`) serves a read that starts at `<from>` from
`<to>` instead. The classroom warp op `0x51 (0, area, door, 30, 0)` can likewise be re-aimed by poking its live arguments
(1-A: `0x095e1dd8`): area 1 = 3F, 2 = 2F, 3 = 1F; 2F doors 11/21 = 2-A, 25/35 = 2-B, 51/61 = 2-C, 65/75 = 2-D.

## Dungeon/field data (decoded)

`tools/p2lz.py <file>` decodes the chunk container used by `F0097-F0136.BIN` and `DNGSCN.BNP` (`[type u32][size incl. header u32][usize u32]`
chunks, 4-byte aligned). Method 2 (type bytes `..0201`) is a byte LZ: `c < 0x80` copies `c+1` literal bytes; `c >= 0x80` copies
`(c & 0x7f) + 3` bytes from distance `next_byte + 1`. Every such chunk decodes to exactly its stated size and consumes all input; chunk 0 of
F0097 matches the copy the game keeps in RAM except for 32 relocated pointers. Chunk types `..0103` hold sound-bank-like data (`d00_snd`),
not LZ. F0097 chunk 0 (51 580 bytes) is the floor map: a 0x2c-byte descriptor per area (`2c 00 14 00 ff ff 00 00 <id> <7 offsets>`) followed by
tile layers of 16-bit cells (0x33, 0x8000-flagged layers) and 16-byte event records (`0x1c50c`), chunk 1 an offset table, chunk 2 textures.

## Clock Tower boss fight (played through)

From state 2800 (just outside the 4F tower doorway), `DOWN+RIGHT:40` walks in; scenes 012 pages with Cross, the Hanya battle starts on the gear stage
(skills Whatever Drill, Chill Drill, Dark Young Legend all render), the results screen shows 130 EXP / ¥12,000 with level-ups, then script 013
plays (Hanya flees, Maya/Ginko/Eikichi dialogue) and control returns on the 4F Clock Tower corridor. Auto-Battle (Triangle) alone loses with the
party at about Lv 4: the hero starts the fight at 1 HP. A battle-time cheat that works: the battle keeps two copies of the party table at
`0x098eaa50` and `0x098eb550` (176-byte records, first u16 of each = HP, u16 at +4/+6 = max HP / SP), so
`P2_POKE=<vblank>,98eab00,x03e703e7,...` (hex address, raw 32-bit word with `x`) written a few vblanks after the battle state loads gives the party
999 HP and Hanya's record (the sixth, at `0x098eaa50 + 5*176`) 1 HP. The level-up screens take 9 distribution points per character
(RIGHT x9 then Cross). Savestates from this run: 3100 (tower loading), 3102 (battle command menu), 3111 / 3200 (after the boss, tower corridor).

### Leaving the school (1F) and the way down from the Clock Tower

After the boss (state 3200) the party is on 4F at the "Stairs" landing; walking RIGHT goes down to the 3F landing (position resets to (12598, 6530)).
On 3F: LEFT:200 then DOWN:600 follows the spur and main corridor east; UP:168 from the east end, then the stair spur at x~19846 is the 3F-2F flight
(the mapper walks it by itself: starting it on 3F with `P2_MAPPER_EXIT=1` carried the party down to the 1F "Stairs" landing, state 930).
1F frame (positions are continuous across the floor): from the stair landing (3894, 7618) DOWN walks east along the main corridor (z 6898..7570);
at x ~12918, RIGHT turns into the "Courtyard" corridor (z 5746..6370, west end is a window wall at x 11814); from (11814, 6370) `LEFT:600` runs
south along a passage (x ~11958) across the main corridor to z 9634 = "Seven Sisters High: Entrance Hall", where the two Exit icons are right in front of the
party; Cross there shows the loading screen and the Rengedai town map (the party marker sits on "Seven Sisters High"). Shortest recipe from the 1F main
corridor at x=15798 (state 942): `UP:120`, `RIGHT:500`, `LEFT:26`, `UP:200`, `LEFT:600`, `CROSS`. The Auto Map (Square) on 1F shows the 3 pairs of Exit
icons at the south end of the explored map once the hall has been visited. States: 930 (1F stair landing), 942 (1F corridor), 948 (Courtyard west end),
949 (Entrance Hall), 950/3300 (town map after leaving, 5-member party, all clock/boss flags set).
`P2_WATCH_HEX=1` prints each watched word as `value/hex`; `P2_NO_FLOOR=1` makes the mapper/tour ignore the floor byte; `P2_FRAMES_DIR=<relative dir>` redirects frame dumps.

## Town-map navigator (P2_MAPNAV)

`P2_MAPNAV=<node>[,<save-slot>]` drives the town-map cursor to a node by number (game/src/native_funcs.cpp, `mapnav_buttons`): it reads the node and
edge tables (see `tools/mapgraph.py <slot>`), plans a path, and at every node presses the stick direction that the game's own edge selection
(`pick_slot`, the refactored core of `map_cursor_pick_edge`) maps to the next edge; it holds the direction along each straight edge, treats an
edge end as the node (the game keeps `cursor.node = 0xFF` at two-edge nodes), and releases/retries when stalled. On arrival it logs
`arrived at node N` and, with a slot, writes a savestate. Typical use: `P2_MAPNAV=19,3414 build/dev/p2.exe --headless --load-state 3400 --turbo-until 99999999 --exit-after <V+3000>`.
Arrival makes the party comment on the place (Ginko/Yukino/Eikichi lines); Cross at a place node with an IN marker enters it.
Place nodes (type 04) found: Rengedai: Silverman Manor (node 3), Lotus (6); Yumezaki: Zodiac (6), Giga Macho (10), Pachinko Silver (18), Mu Casino (19), Yumezaki Center (20,
no entry marker yet), GOLD (23); Hirasaka: Kameya Alley (0), Sumaru Prison (4), Sakanoue Building (7), Smile Hirasaka (9, Yukino: "weren't we supposed to be going to Peace Diner?"), node 16.
Type 40/10 nodes with a "!" bubble are townspeople (Cross shows their talk text). Peace Diner (Kozy's scene, script 013 end) is not one of the type-04 place nodes of the three districts; it
probably appears through a flag or from the "Theater" entry of the City Map. Savestates: 3400 (Yumezaki map), 3300 (Rengedai map), 3490 (Hirasaka map).

Later-story check by selector remap: with the party on a Yumezaki place node that has an IN marker (state 3439 = Mu Casino), `P2_IO_REMAP=220565504,220516352` makes the place read the selector of blob 14,
which runs event script 014: the Zodiac club scene (dance floor with crowd animation, Noriko, Anna Yoshizaka, Eikichi; "We got a new Leader!"). It plays correctly through the first 100 Cross presses.
Open question: how Peace Diner (Ginko/Yukino's stated next stop after the Clock Tower) is entered - it is not one of the type-04 place nodes of Rengedai, Yumezaki or Hirasaka, and Kasugayama High
(Hirasaka node 16) shows no IN marker yet, so some flag or visit gates it.

### Peace Diner (found with the user's hint) and the Zodiac lead

Peace Diner is inside Yumezaki Center (Yumezaki node 20, type 04): the cursor has to sit exactly on the node (`node=20` in the cursor struct, ix,iy = 237,83); `P2_MAPNAV` now
keeps pressing along the last edge until the game puts the cursor on the node (it logs "cursor not on the node" otherwise). Cross there opens the "Shopping Mall: Please select a destination"
menu (Tony's Shop, Anima Mundi, Satomi Tadashi, Bikini Line, Peace Diner, Velvet Room, price list ¥17,480 shown); DOWN x2 then Cross picks Peace Diner. The diner scene plays fully (counter,
Ru, Eikichi vs Ginko about the Cuss High rumour, Yukino: "Let's just go to Zodiac and learn the truth for ourselves") and returns control in the diner (state 3800 = 3713).
Recipe: `P2_MAPNAV=20,3440` from state 3400, then `CROSS`, wait ~120 vblanks, `DOWN, DOWN`, `CROSS`, then Cross every 45 vblanks.
The next lead is therefore Zodiac (Yumezaki node 6 or the place between it and Pachinko Silver), whose door scene is script 014 once the flags are right.

### Zodiac (the natural way) — state 3813 -> 3900

From Yumezaki Center / the diner: face the exit mat, `LEFT:12, RIGHT:24, DOWN:10, UP:20, CROSS` opens the mall menu and `CIRCLE` returns to the town map with the cursor on node 20 (state 3812).
`P2_MAPNAV=6,3813` then goes to Zodiac (node 6; IN marker, Cross enters, state 3814 = Zodiac "Exit" entrance, 1F dungeon with minimap, compass N). The explorer bot
(`P2_EXPLORE=1 P2_EXPLORE_SEED=3`) reaches "Zodiac: Club Zodiac" about 6000 vblanks in (state 3830/3831: red door ahead, 70 UP + Cross); the club scene (script 014, as the remap test showed) plays:
Noriko, Anna Yoshizaka, Yukino, Eikichi vs the Cuss High students, Maya and Ginko; Eikichi runs off to find Hanakouji-san and the party goes on with four members (hero, Ginko, Maya, Yukino);
control returns inside the club (state 3835/3900). Next lead from the employees' lines: a group of Cuss High students went upstairs; "the maze on the second floor".
