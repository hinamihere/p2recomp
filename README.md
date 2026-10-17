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

### Zodiac dungeon notes (F0098.BIN)

Zodiac's 1F is `F0098.BIN` (area 14x32 tiles; the lower 7 rows are the corridor ring seen on the Auto Map, "Zodiac 1F"). Door selectors: cell `0xC0nn` in layer C3 with blob = nn + 11
(club door 0xC003 -> b_014 = script 014; 0xC005 -> b_016 = script 16, the masked-guests lounge with the mask box (sets flags 274, 1089, 1090, 1319); 0xC006 -> b_017). The remap
`P2_IO_REMAP=220516352,220520448` at the club door (state 3831, `UP:70`, Cross) enters script 16's room (a dim backstage lounge with sofas, masked NPCs, boxes; renders correctly, state 3930).
Script 15 (Ginko/Maya/Yukino about the secret lounge, masked guests; tests flag 1146) and script 18 (Hiroki Sugimoto the Leader, Eikichi, Kozy = Miyabi Hanakouji; sets 275, 276, 1097) and 19 (Eikichi's change of heart) are the next beats.
The club room itself (script 014 package, part 7 triggers): the entrance door quad DOA0 at x 10..60, z -300..-200 and the exit-to-corridor door `0x000d` at x 300..400, z -1390..-1330 (DOWN+LEFT from the start state walks straight into it),
both warp back to the dungeon; the room axes here: UP = (-x,+z), DOWN+LEFT = -z, UP+RIGHT = +z, RIGHT = +x/+z.

### Spreading the "Zodiac second floor" rumor (needed before Zodiac's real dungeon)

The rumor system (Innocent Sin): the Kuzunoha Detective Agency's Chief Todoroki spreads rumors you have collected. Route: Hirasaka node 0 "Kameya Alley" (a Shopping Mall: Shiraishi Ramen, Toua Defense,
Satomi Tadashi, Tominaga Chiropractic, Gatten Sushi, Kuzunoha Detective Agency, Velvet Room); Kuzunoha Detective Agency opens the agency room (script 319/320/321 family, desk quad "DESK"): in the
room the desk is at about x 115, z -920 (room units; leader position = i32 at `0x08e44c14` / `0x08e44c1c`). `P2_GOTO_INT=1 P2_GOTO_AXES=-1,-1,-1,1 P2_GOTO="8e44c14,8e44c18,8e44c1c;300,-1250;115,-1250;115,-920"`
walks there from the entrance (state 4000), then `UP:6` + Cross opens the Chief's menu: Spread rumors / Check rumors / Talk with the Chief / Cancel / Leave. "Spread rumors" lists
"Zodiac's second floor... Zodiac's second floor is a huge maze." (3,000 yen); Confirm, Yes. The rumor enters the list after the Zodiac club scene (flag 1145 set by script 014) and after a rumormonger hears
it - in my run flag 1145 was poked/forced, see below. States: 4006 (at the desk), 4009 (after spreading, 14,480 yen left), 4035 (Zodiac exit entrance, rumor spread).
`P2_GOTO_INT=1` makes the goto autopilot read int32 room coordinates (30-unit tolerance, 15 s timeout).
`tools/placeblobs.py <town slot> <nodes>` prints the branch.bin blob (door selector) every place node reads.
Peace Diner (inside Yumezaki Center): after the Zodiac club scene, Chikarin and Kozy's friends say Kozy "went to infiltrate" Zodiac; the scene with Eikichi and "Let's just go to Zodiac" is script 342.

Zodiac with the rumor "spread" (state 4040/4041, map analysis/maps/zod2.txt, 376 cells in the ring corridor, `Zodiac: Exit` entrance): the ring now contains the storefronts "Cafe", "Home Goods Store" and an
"Emergency Exit" door (cells around (14,61), (20,69), (8,69)); Cross at them shows the "!" bubble but no dialogue box in my tour (the tool's DIALOGUE verdict there is the bubble, not a text box).
The stairs (guide: "Upstairs" icons 1, 2, 7 on 1F) were not reached; the mapper (P2_MAPPER_EXIT=1) ran 500k vblanks without a floor change. Still unknown: whether the Emergency Exit is the way up and
which facing/flag opens it (the tour presses Cross after facing the recorded direction only).

### Zodiac: where the real dungeon is (user's IS guide + decoding)

The user's Innocent Sin guide (Gamecoder FAQ) and a walkthrough say: the Club Zodiac scene ends *inside the club*; the dungeon proper ("1F") starts through **the door Michel ran through**
(the club's far door, trigger `0x000d`); the 1F stairs are "in the N section", 2F is a maze with 9 item boxes (rumor only adds the boxes; it is not needed), the 3F staff room (east) holds the Strange Mask.
Findings:
* Both club doors are `op 0x51` warps whose arguments live in guest RAM (`0x09440f80` for `0x000d`, `0x09440f08` for DOA0 in state 3900): `(1, area, col, row, facing)`. Cell coordinates are in 256-unit cells:
  `x = 256*col - 128`, `z = 256*row - 2613` (area 2 is the valid one). Poking them (`P2_POKE=<vblank>,9440f84,x2,9440f88,x<col>,9440f8c,x<row>,9440f90,x4`, then `DOWN+LEFT:500`, `CROSS` at the exit door of state 3900) **teleports the leader anywhere**.
  Default target (col 8, row 47) is the hall in front of the club's front door (state 3922 = same place); (col 4, row 19) is inside the red lounge maze north of the club.
* The north part of 1F is a red-lit maze of lounges. It is **not connected on foot** to the hall/ring component; the mapper maps 641 cells there (`analysis/maps/zod4.txt`, `P2_MAPPER` with `P2_NO_FLOOR=1`)
  and 0 exits: the Auto Map (Square, pan with the D-pad, Triangle = legend) of that run shows no Upstairs icon, so the stairs are in another component.
* `P2_NO_BATTLE_DETECT=1` is needed in the red lounges: the mapper/tour read the red HUD-less screen as a battle (the tour also takes the red lounge for a dialogue box).
* F0098.BIN chunk 0 holds 3 areas (14x32, 25x25, 13x23; tools: `tools/p2lz.py`); area 1's layer 0 is a 14-wide tile grid (512 units/tile, row = (z+2613)/512): rows 23..31 are the hall/ring ("0x1xx" tiles),
  rows 16..22 cols 2..7 the club block (`0x3b`), rows 0..15 the red maze (`0x32` floors). Layer 1 edge values `0xCA03` are the club doors, `0xCA04/0xCA05/0xCA06` the stair pairs (A1 rows 50/52, A2 (13,5),(21,8),(21,21), A3 (16,3),(16,10)).
  The door/warp cells with `0xCA0n` are not yet located in raw coordinates; the Emergency Exit in the ring is only a "locked" text (DNGSCN chunk 13).

### Zodiac: stairs found (1F -> 2F)

* The warp op arguments are `(dungeon, area, col, row, facing)`: arg 0 selects the dungeon file (1 = Zodiac F0098, 2 = Kasugayama F0099 - poking it warps between dungeons),
  arg 1 the floor (1 = 2F maze, 2 = the 1F hall/club side, 3 = 3F "Secret Lounge"), `x = 256*col - 128`, `z = 256*row - 2613`. Floor 3 crashes the game
  (`fatal: Guest memory access ... 0x80008000` in `recomp_unit_0097`, 225 vblanks after the warp) - the 3F loader is not usable by teleport (climbing the stairs on foot works, see below).
* The club's DOA0 warp `(1,2,4,34,6)` lands at (896, 6091) on 1F, **west of the club block, in the component that contains the stairs**: `P2_MAPPER` there (`P2_NO_FLOOR=1 P2_NO_BATTLE_DETECT=1`) walks into
  "Zodiac: Stairs" by itself (state 5140). From the landing, `RIGHT:40` climbs to the 2F landing at (5430, 386) (state 5143); the Zodiac staff there only says "Did you remember your mask?" and lets you walk away.
* The 2F component reachable from that landing (3,500 cells, `analysis/maps/zod2g.txt`, 150k-2M vblanks of the mapper) is closed: x 2544..11592, z 1000..6500, item sparkles on the Auto Map, **no up stairs**.
  The guide puts the stairs to 3F and the Aqua Card box on the west side and Trish's Spring in the centre (warp target (area 1, col 8, row 20) = "Trish's Spring" banner, x 1920, z 2507, which is west of
  the mapped component): the west half of 2F must be entered through something the mapper does not cross (door/teleporter/the spring room).
* `P2_NO_DIALOGUE_DETECT=1` (tour/mapper) and `tools/exitscan.py <slot> <map> <first> <last>` (walks into the dir-4 event cells of a map, prints AREA CHANGE) were added.

### Zodiac: 2F south strip, 3F and the Strange Mask (all on foot)

* **True position = cell bytes.** The dungeon floats at `08CD4190/98` are camera-frame coordinates: they change frame between camera zones (the two "2F components" mapped
  before were the *same* floor seen through two frames). The player's cell in the 256-unit grid of the F-file layers is the byte pair `08CEB2E0` (col) / `08CEB2E4` (row);
  `08CEB358` is the 8-way facing index inside rooms (0 = DOWN+LEFT ... clockwise). The active area descriptor of `F0098.BIN` chunk 0 is the one whose D-G offsets the game
  has relocated to absolute pointers: 0 = 1F (14x32 tiles), **1 = 2F (19x18)**, 2 = an unused 2F variant (25x25), 3 = 3F (13x23).
* **Layer C is the walkability map** (cells = 2x2 per tile): low byte `0x3b`/`0x3d` solid, `1/2/3/4` wall on the top/right/bottom/left side, `5-8` corner posts, `0xc0`
  (2F) / `0x00` (3F) floor, `0xd0` chest; high nibble = region (stairs halls, rooms). **Layer B holds markers**: `0xCA0n` room doors, `0x2800/01` stairs down,
  `0x2802/03` stairs up, `0x2c0n` staffers, `0x2001` event lines, `0x0001` chests.
* 2F: the south strip (rows 28-33) is open at cells (21,26)->(21,27); the mapper never probed it. West along row 31 (chest at (14-15,32-33)), past "Trish's Spring",
  the up-stairs at (1,32-33) lead to 3F (state 5670; arrival scene with Yukino, "It's Eikichi. He's calling for us", 5681 = control after it). Climbing on foot works;
  the earlier 3F crash only happens with a teleport warp.
* The 2F "Staff Room" (door `0xCA05` at (32-33,27)) is event script 015 (masked guests, no box). The **3F Staff Room** (door at col 18, rows 32-33, east corridor:
  walk (5,36)->(5,25)->(18,24)->(21,24)->(21,32), then UP into the door; state 5684) is script 016: walk to room (437,-578) (`P2_GOTO_INT=1 P2_GOTO_AXES=-1,1,1,1`
  via (303,-209),(425,-209),(425,-590)), `DOWN+LEFT:3`, Cross -> "Obtained the Strange Mask" (flags 274 + 1319; state 5690).
* With the mask, the 3F staffer at the "Secret Lounge" corridor (cell (12,17), north from (12,25)) says "Welcome to the wild dance, my brothers!" (states 5695/5696):
  that is the real gate. The 2F staffer by the 1F stairs (cell (14,4)) keeps saying "Did you remember your mask? ... there should be extras somewhere" with the mask
  too (state 5830): a hint, not a gate.
* Tools: `P2_GOTO_TOL=<units>` (waypoint tolerance; with `P2_GOTO_INT=1` and the cell bytes, `P2_GOTO="8ceb2e0,8ceb2e0,8ceb2e4;col,row;..."`, tolerance 0.5 steers
  cell by cell), `tools/cellwalk.py <chunk0.bin> <area> <from-slot> <col,row> <first-out-slot>` (plans on layer C, recalibrates the stick per leg, learns blocked steps;
  took the party from the 2F strip to the staffer in 23 legs).
* **Secret Lounge (script 018) played through, Zodiac cleared.** The lounge door is cells (10-11,16) (`0x4024` in layer C, `0xCC02` in layer B) at the top of the 4-cell-wide
  corridor; the staffer only blocks col 12, so from state 5696 `DOWN:14, LEFT:60, CROSS` enters (5901). The scene's one choice (op `0x1a`, state 5922 = menu open) picks
  HELP or YOUSU; `DOWN, CROSS` = "Wait and see what happens" (sets flag 275, the guide's choice for Eikichi's Prime Persona). The scene (Hiroki, Miyabi, Eikichi's stand) ends
  on the Yumezaki town map with Eikichi back in the party (state 5932). The staffer's own script (DNGSCN 067) only picks his line by flag 274.

## Kasugayama High (F0099) - in progress

Route after Zodiac (script 019: "Next stop, the Cuss High student council president!"): Yumezaki exit node 14 -> Cross -> City Map (Rengedai / Hirasaka /
Yumezaki / Theater) -> UP, Cross = Hirasaka -> `P2_MAPNAV=16` (Kasugayama High, now a place node) -> Cross enters the Entrance Hall (state 5945).
F0099 chunk 0 (`build/f99_c0.bin`, decode with tools/p2lz.py): areas 0 = 1F, 1 = 2F, 2 = 3F (38x21 tiles each), 3 = 4F/roof?, 4 = B1F (14x10); the floor byte
`0x08CE819D` is the area index. Walkability (layer C) as in Zodiac, floor low byte 0x80/0xc0; door cells: C type `0x22` with a `0xC8nn` id in B (normal doors)
and type `0x2a` (`0xA001` in B, flanked by `0x2c` guards); stairs: the west stair hall rows 33-34 (1F (2-3,34) down to B1F, (6-7,34) up; 2F/3F the same pattern).
The door-to-selector offset differs per floor (2F `C809` -> b_022 Class 2-1, 3F `C809` -> b_025); `ENTER room n` selectors open room scenes whose script is
resident in RAM (tools/roomscript.py dumps labels, character placements and flags).
Steps so far (guide order): 2F Student Council Room = door `C80F` at (4-5,28) (UP+Cross from (4,29)): talk to Eikichi (room (131,-398), sets 787) - the
"Earnest student" talk (sets 278) could not be located and **flag 278 was set with tools/setflags.py** (state 6510); 3F Class 3-1 (front door `C808`, party at
(1029,-1356), walk DOWN+RIGHT ~36 frames to Eikichi, sets 788; state 6572); 2F Class 2-4 (east doors `C80D`/`C80E` at col 67, front door DOWN+Cross from (66,16);
DOWN+RIGHT 57 + DOWN+LEFT 6 + Cross talks to the students in front of Eikichi, sets 789; state 6832).
Tools: tools/fmarkers.py (layer-B markers per area), tools/stairpush.py, tools/roomexit.py, tools/route.sh (chains cellwalk/stairpush), tools/setflags.py.
B1F: door `CC03` at (14,12-13) (UP + Cross from (15,12), selector b_028 = `ENTER room 51`) opens the shelter entry scene (script 020) once flag 278 is set.
**Air Raid Shelter** = dungeon file **F0100** (`build/f100_c0.bin`, one area, 90x84 cells, two halves) plus two link rooms. Bottom room (script 020's
package, "Bomb Shelter"): exits are walk-in quads EXIT_LEFT x 13..110 / EXIT_RIGHT x 1284..1373 (z -496..-312, face DOWN+RIGHT resp. DOWN+LEFT to
fire) that warp (op 0x51 `(3, 0, 37|56, 74, dir)`) to the F0100 doors `C805` (38,74) / `C807` (55,74); the top room (door 57 `C803` at (57,10) reads
b_033 = script 021, door 40 `C801` the other side) links the halves at the top. Doors need Cross (tools/doorpush.py). Crossing the bottom room's
CHECK segment sets flags 486 -> 487 -> **279**; the top room's CHECK sets 493 -> 494. Stick axes: bottom room UP = (-x,+z), RIGHT = (+x,+z); top room
UP = (+x,+z), RIGHT = (+x,-z); F0100 like the school. b_032 (flag 279 -> script 022, the sleep scene) is the bottom doors' selector.
tools/shelterloop.sh runs one circuit (cellwalk halves, doorpush, Cross through the room scene, tools/roomwalk.py slides along beams to the exit quad).
Quiz (script 024, 5 questions, op 0x1a): correct options are 1st, 2nd, 2nd, 1st, 2nd (from the bytecode's repeat branches).

## Open problems (remind the user of these at the start of the next session)

* Kasugayama 2F Student Council Room: the "Earnest student" talk that sets flag 278 was never triggered by Cross (the NPC at room (189,-227) only
  gave the Glib student's line); flag 278 was forced with `tools/setflags.py` (state 6510). Find the real trigger (facing/position) some time.
* Dungeon doors need Cross from the adjacent cell, and the door-id -> branch.bin selector mapping is per floor and not linear (2F `C809` -> b_022,
  3F `C809` -> b_025, B1F `CC03` -> b_028); only IO traces resolve it so far.
* Mapper/cellwalk get stuck on corner posts and NPCs; the adaptive walker (tools/cellwalk.py) is slow (~10 min per maze half).
**Shelter cleared (2026-10-16 night):** three bottom-room passes set 279; door 37 then ran script 022 (sleep), 023 (dream), 024 (quiz; a random
Cross / DOWN+Cross pattern passes it since wrong answers only repeat) -> flags 280 + 281, control back in the B1F basement entry room (state 9163).
Second visit (flag 281 -> the school is dungeon file **F0126**, the festival version; warp op arg 0 = 29): 1F west wing door `CC01` (2-3,20)
(LEFT + Cross from (2,21)) = "Gym Entryway" room (axes UP = (-x,-z), RIGHT = (-x,+z)); the committee member KASU_A_1_c at room (1251,-834) gives the
Masquerade Mask (face DOWN + Cross from (1174,-857), flag 1321; state 9421); warp2 quad x 797..998 z -165..-104 (push RIGHT) is the gym door (needs
1321) -> script 025 (masquerade, shadowmen; sets 498; state 9450 = control in the gym, axes UP = (+x,+z), RIGHT = (+x,-z)); the gym exit quad is at
x 173..415, z -2995..-2940 (DOWN+RIGHT) and returns to the entryway at (1008,-235); warp1 quad x 966..1105 z -1296..-1240 (DOWN+RIGHT) leads back
to the corridor. Roof door = 4F `CC03` (10-11,20), 4F reached from the 3F east stairs (68,34-35).
**Kasugayama cleared (2026-10-17 early):** roof door -> script 026 (Yasuo) -> boss fight. The battle party table sits at `0x094AB290` (+ copy at
`0x094ABD90`) this time (records 176 bytes: hp, hp, max hp, sp; the 6th = Yasuo, 735 HP); a one-shot `P2_POKE` at load+1 with 999/999 HP for the five
party records and 1 HP for Yasuo, then Triangle (Auto) wins in one round (states 9911 = battle start, 9920 = Level Up screens). Level Up: RIGHT x9 then
Cross per character.

## Chapter 2 (after Kasugayama)

After the roof boss: script 027 (King Leo), the gym aftermath (Maya: "ask people for rumors about the Masked Circle and Joker"), control on the
Hirasaka town map (state 9950). Peace Diner (Yumezaki node 20 -> mall menu DOWN x2 -> Peace Diner): Chikarin stands at room (817,-662) after the
first visit; walk to (790,-700) (axes UP = (+x,-z), RIGHT = (-x,-z)), face LEFT, Cross -> "Ask about rumors / Talk with Chikarin / Never mind";
DOWN + Cross ("Talk") gives "maybe the fortune teller in Kounan would know" and sets flag 795 -> **Kounan** appears on the City Map (state 9996).
Leaving a mall shop: walk onto the exit mat (600,-1254), Cross opens the mall menu, Circle returns to the town map. tools/settle.py closes dialogue
until the leader can move.
Kounan town map (City Map: DOWN from Yumezaki, Cross; wait ~300 vblanks before P2_MAPNAV, node type 0x11 = road + event bits, mapnav now accepts
combined type bits): node 2 = Kounan PD, 7 = Abandoned Factory, 15 = (party comment), 19 = Seaside Mall (Jolly Roger, London Clothier, Satomi
Tadashi, Hiiragi Psychotherapy, Sumaru Genie, Velvet Room), 23 = (party comment), 24 = Lunar Palace Kounan. States: 10008 = Kounan map.
Rumormonger chain (each "Talk with ..." sets one flag; facing matters, the talk menu is Ask about rumors / Talk / Never mind -> DOWN + Cross):
Chikarin (Peace Diner, 795) -> "fortune teller in Kounan"; Kounan PD: Little Chii (CHIBOU, room (1097,-1291), UP+RIGHT + Cross from (1034,-1305))
tells the Dresser Hag rumor (flags 800-803; returning the Hag's Dresser is a side quest, skipped); PD exit = walk into the quad x 603..801
z -1583..-1517 facing DOWN. Kounan Seaside Mall (node 19) -> mall menu DOWN x2 = Sumaru Genie: walk to the fortune table (514,-449), face LEFT,
Cross -> the Genie (sets 797) -> "the man at Honmaru Park". Rengedai Honmaru Park (node 17; a student scene plays on entry, flag 798): Toku-san
(BLOCK_TOKSAN, room (1343,-1012)): from (1406,-1004) face DOWN+LEFT, Cross, Talk -> 794 -> "ask the salesman at the sushi restaurant".
tools/mallexit.py leaves a shop from its exit mat (Cross opens the mall menu, Circle -> town map; detects the node-table pointer change);
tools/roomscript.py now also lists the room's trigger quads (label, x/z box, facing).
Sushi salesman (Hirasaka Kameya Alley mall -> Gatten Sushi, BLOCK_TIRO char at (446,-656)): from (442,-722) face LEFT; tools/rumortalk.py
(tries 4..14 intro pages, then DOWN + Cross = "Talk") found 7 pages -> flag 796. With 794-797 set, the Kuzunoha Detective Agency selector (b_171)
enters the scene room: walk to the Chief's desk (115,-920) via (300,-1250),(115,-1250) and press Cross without turning -> Chief Todoroki /
Tadashi scene (Muses photo), flag **283** (state 10514). Shop exit doors: stand in the warp1 quad (e.g. Gatten Sushi x 202..399 z -1392..-1314) and
press Cross without turning (the "!" bubble shows the right facing). Town-map navigator pitfall: started on a dead-end type-00 node with
`node = 0xFF` (right after leaving Honmaru Park) it stalls; nudge the stick (DOWN 40 frames) first. tools/travel.sh <slot> <to> <exit-node>
<district> goes through the City Map (0 Rengedai, 1 Hirasaka, 2 Yumezaki, 3 Kounan, 4 Theater).
**Mu (Yumezaki node 19, F0101):** after flag 283, Mu Casino plays script 028 (Muses music-video shoot; AD: "the shoot's being held on the fourth
floor"; flag 284). The casino room's DOA_0 quad (x 793..1003, z -91..-14; room axes UP = (-x,-z), RIGHT = (-x,+z)) + Cross enters the Mu dungeon
1F "Casino Entrance" at cell (14,17) (state 10670). F0101 chunk 0: areas 0-3 = 1F-4F (16x22 tiles); every staircase is its own 0x28nn marker pair
(1F: 2802 (4,2-3), 2804 (27,2-3), 2806 (10,26-27), 2808 (21,26-27), 280a (9,40-41), 280c (22,40-41)); 4F has the door `C838` (18-19,20).
The south half of 1F is split into a left and a right region by the wall at cols 15-16; cells drawn as wall edges (types 1-4) in row 23 are not
walkable there (only the gaps cols 5-10 and 21-26). tools/stairmap.py tries a floor's staircases and records the landing floor/cell.
Travel: tools/travel.sh now takes <from-district> <to-district> (the City Map list wraps around and starts on the current district).
tools/shopexit.sh leaves a shop through its warp1 quad.
**Mu stair route (found with tools/stairmap.py, `P2_CELLWALK_EDGES_SOLID=1`):** 1F entrance (14,17) -> stairs 2804 (27,2-3) -> 2F (23,2) ->
281c (21,28-29) -> 1F right-south room (22,26) -> 280c (22,40-41) -> 2F (26,40) -> 281a (10,28-29) -> 1F left-south room (9,26) ->
280a (9,40-41) -> 2F (5,40) -> 281e (2-3,37) -> 3F (2,33) -> 282e (12-13,27) -> 2F central column (12,23) -> 2812 (12-13,11) -> 3F (12,7) ->
2834 (19,40-41) -> 4F (15,40) -> walk to (18,21), push DOWN into door C838 = Video Game Room -> script 029 (Muses shoot, Lisa), flag **285**
(state 12470). Dead ends: 2802 -> 2F top-left -> 2816 -> 3F (4,26); 2814 -> 3F (19,16) -> 2830 -> 4F right region (no way to the door).
The 1F south rooms are walled off from the entrance by a rail (cells with C low byte 0xb5-0xb8 in row 24) and are reached from 2F.
After Mu: script 029 ends with "Let's head for Giga Macho" (Mami and Miho's live radio show). Giga Macho = Yumezaki node 10, dungeon file
**F0102** (4 floors, 20x20 tiles; 1F stairs 2800 (26,2) -> 2F (34,2); floors 2-3 have stairs at (26,2-3) and (33,2-3); 4F has the door
`C801` (18-23,33) and staff 2c00 (26,26)). The floors are store-shelf mazes: the shelves are props, not in layer C, so the cell walker cannot
plan there; the probing mapper (`P2_MAPPER`, floor-aware) is used instead (analysis/maps/giga.txt).
**Giga Macho cleared:** the 1F-3F stairs (F0102) only go up to 3F; the 3F up-stairs pocket (24,2) is a dead end (an invisible barrier before the
"EVENT FLOOR 4" stairs) and the 3F east edge (38,2) is a closet ("You can't get in through here..."). The way up is the **elevator** = the 2x2
layer-B marker `0xA40n` at (32-33,36-37) on every floor: walk to (32,35) (mapper route on 3F), DOWN + Cross opens "Go to 5F / 4F / 3F / 2F / 1F";
UP + Cross from 3F = 4F. 4F-5F are a separate dungeon file **F0103** (area 0 = 4F, 1 = 5F); 4F stairs (33-34,2) -> 5F (34,2) (found by the mapper,
`P2_MAPPER_FLOORSAVE` writes a savestate the first time it reaches a floor); on 5F walk to (15,6) (`P2_CELLWALK_EDGES_SOLID=1` cellwalk) and push LEFT
into door `C802` = FM Live Studio -> script 030 (Muses on Meteor Masa's radio show, Ginji Sasaki), flags 286 + 287 (state 12921).
Guide note (almarsguides chapter 2): Ginko leaves the party for the next two dungeons after this scene.
After Giga Macho: the City Map gains **Aoba** (between Yumezaki and Kounan; tools/travel.sh district indices: 0 Rengedai, 1 Hirasaka, 2 Yumezaki,
3 Aoba, 4 Kounan, 5 Theater from now on). Aoba town map node 12 = Aoba Park (Outdoor Concert Hall label), dungeon file **F0104** (area 0 30x26
tiles, area 1 30x33); entering lands at (12,44); the `0x2400/01` cells at (40-41,10) move the party within area 0 to (20,12) ("South Side").
**Aoba Park -> Concert Hall -> Prince Taurus (2026-10-17):** Aoba Park (F0104) area 0 exit = mapper cell (8,-24) LEFT (raw x~2856 z~-7900, the
mapper map uses AXES 1,0,0,1 here) -> area 1 (41,53); the mapper route then cellwalk to (34,4) and LEFT = Trish's Spring (`2402`); door `2403`
(50-51,8) from (50,7) pushing +row = Concert Hall Rear Entrance (script 031): staff C at room (1263,-55), from (1246,-149) face UP, Cross x7, the
badge offer "Yes" (10,000 yen) -> employee badges (flag 288). UP + Cross at the back door -> Outdoor Concert Hall (F0105, one 46x30 corridor
ring): south-west door `C803` (6-7,20) from (6,19) = stage -> scripts 032/033; on the stage walk to (1030,-1250) and talk to Eikichi with
DOWN+RIGHT, first option "wait and see" (flag 292); the concert and Prince Taurus scene follow, then the boss fight. **Battle table at 0x094AB290
again** (tools/bosswin.py now checks it first; poke at the Tactics menu, ~400 vblanks after the battle intro); party Lv15 after the win (state 13321
= stage after the fight, flag 295 = escape timer phase).
tools/bosswin.py: HP cheat + Auto + level-up distribution in one go.
Escape: after the fight (flag 295, King Leo's bomb timer), the stage room's EXIT quad is at x 408..465 z -561..-467 on the floor level (y -100):
from the upper stage (y 300) walk +x (DOWN+RIGHT) down the stage stairs to the y 100 level (e.g. DOWN+RIGHT 40, DOWN 40, LEFT 60 from (990,-324)),
then roomwalk to (436,-514) and press Cross without turning -> Backstage (EXIT quad x 5..60 z -404..-292, Cross) -> corridor (6,19). The riddle is
in the dressing room behind door (34,9) (b_048 -> script 217 once 292 is set): locker ROCKER3 (x 692..744, z -280..-224): from (698,-254) face
DOWN+LEFT (facing index 6 = +x here) and Cross -> "Obtained the Riddle!" (flag 304). Then the corridor's north-west door `C805` (3,2), push DOWN
-> the park explodes (bomb timer cut-in), Lisa scene with a choice (Gently cheer her up / Slap her - first option taken), Yukino: "She's gonna
meet us at Double Slash in Aoba" (state 13551).
Double Slash = Aoba node 2 (Aoba Way mall: Clair de Lune / Rosa Candida / Satomi Tadashi / Etheria / Double Slash / Velvet Room), DOWN x2 + Cross:
Elly (Eriko Kirishima) decodes the riddle on the city map -> the next target is in Hirasaka, "when the winds of change blow" (flag 305, state 13600).
**Smile Hirasaka (Hirasaka node 9, F0107):** areas 0-3 = 1F-4F (35x21 / 35x19 tiles), elevator marker `0xA40n` at (32-33,8-9). Entry: the party
splits (Maya's cigarette scene) and the hero starts alone in the 4F Women's Restroom (door `CA02` (64,28-29)); 4F stairs (5,20-21) -> 3F (6,24).
Guide order (almarsguides chapter 2): Maya on 3F south-east, Ginko on 1F south (3F western stairs), Yukino on B1F south (3F north-eastern stairs),
then the elevator to 5F and Eikichi in the Women's Bathroom -> Ixquic + Belphegor (end of chapter 2). Optional rumor spreading skipped.
Smile Hirasaka 3F: Maya is in the Men's Restroom on the south-east (cell (65,28) marker `2C00`); route (6,24) -> (46,33) -> (66,33) -> (65,29),
then LEFT:4 + Cross (flags 312, 1681; state 14011). Use route.sh with intermediate goals: cellwalk alone gets stuck on the 3F pillars.
Ginko: 3F west stairs (5,20-21) -> 2F (6,25) -> 2F (5,20-21) -> 1F (6,25) -> walk to (7,37), RIGHT:14 (south) + Cross -> flag 314 (state 14430).
Yukino: B1F is a separate file **F0106** (35x17 tiles). The elevator (2x2 marker `0xA40n` at (32-33,8-9) on every floor, stand at (32,10), LEFT +
Cross on 1F) offers 4F / 2F / 1F / B1F; DOWN + Cross = B1F. Yukino = B1F marker `2C00` at (67,26) in the east wing (reach it through the north
passage, row 4-8).
Smile Hirasaka topology: the elevator stops at 4F / 2F / 1F / B1F (not 3F). On 2F, 1F and B1F the north-east block (stairs `2804`/`2806` at
(58-59,5)/(62-63,5)) is walled off from the elevator side; B1F's way east is locked double doors ("Storage", (49,4)). Only 3F's north-east area
is open to the rest of the floor, so the B1F east wing (Yukino) is reached through the 3F north-east stairs: 2F west stairs (5,24-25) up -> 3F,
walk to (62,7), then down the north-east stair chain.
North-east stair chain (verified): 3F (62,7) push -row -> 2F (58,6); 2F walk to (62,6) push -row -> 1F (58,6); on 1F from (62,6) push LEFT
(-col, toward the 58-59 pair) -> **B1F east wing** (58,6) (F0106; the floor byte reads 0 on B1F because it is that file's area 0).
Yukino: B1F east (66,27), UP+RIGHT:3 + Cross -> flag 313 ("hurry and join up with Eikichi"), state 15810.
Return trip: B1F east (58,6) UP -> 1F (62,6). 1F and 2F up-stairs are the (58-59,5) pairs; approach them from the south ((62,9) -> (58,9) ->
(58,7), push -row) because walking west along row 6 enters the (62-63,5) down-stairs instead. tools/where.py <slot...> prints cell/floor and which
decoded chunk-0 files are resident (B1F (F0106) and 1F (F0107) both report floor 0).

### Savestate housekeeping
Bot runs write hundreds of 36 MB savestates. `tools/prune_states.py <from-slot>` deletes every slot below <from-slot> except the milestones listed
in `states/KEEP` (2 = boot/early, 3200 Clock Tower done, 3300/3400 town maps, 3900 Zodiac club, 5932 Zodiac cleared, 9950 Kasugayama cleared,
10008 Kounan, 12470 Mu cleared, 12921 Giga Macho cleared, 13321 Prince Taurus beaten, 13551 Aoba Park escape, 13600 Double Slash, 14011/14430/15810
Maya/Ginko/Yukino recovered, 16750/16824 Smile Hirasaka 4F). Pruned 2026-10-17: 45 GB -> 3 GB.
**Chapter 2 finished (2026-10-17):** with Maya/Ginko/Yukino recovered, the 4F Women's Restroom door (`CA02`, from (65,29)) runs script 051 (Eikichi vs
the cleaning lady) -> Ixquic and the Masked Circle -> boss Ixquic + Belphegor. This fight uses a different battle table: **0x0950BAA2** (copy
+0xB00), records hp, max hp, sp, max sp (layout B in tools/bosswin.py); party at 1 HP at the start, Ixquic 1656 HP, Belphegor 1778. Blind Cross
presses walk into the fight and lose (game over -> title); step in 90-vblank chunks until the enemy record fills, then bosswin (records 5..7 are
enemies; one that is already 0 HP makes the layout check fail -> poke 0x0950BAA2+176*6 directly). Party Lv17 after it; Elly decodes the next
riddle: **"El Dorado"** ("the sign of water that presides over death and rebirth"); control on the Hirasaka map (state 17170).

## Chapter 3 (guide order, almarsguides): GOLD (Yumezaki node 23; 3F Men's Staff Room = transmitter + riddle, then chase the man to the 1F
Gold Boxing Gym) -> Aerospace Museum (Kounan; kids on 2F-4F, choice "Leave this to Maya", boss King Leo on the roof) -> Kuchisake-Onna rumor (old
man near Mt. Iwato) -> Mt. Iwato (Rengedai, north of Alaya Shrine; masks on B1F/B2F; boss Shadow Maya, Prime Persona choices) -> Mt. Katatsumuri.
GOLD (Yumezaki node 23): choice "Yeah, let's check it out", Ulala scene, control in the GOLD Boxing Gym room. Its exit quad (x 200..400, z -1000..-930;
tag byte 6 = 01 here, tools/roomscript.py now matches it) first runs the staffer's evacuation line (flag 508), then warps to the dungeon
**F0108** (areas 0-2 = 1F-3F, 20x20 tiles) at 1F (30,19). Stairs: 1F (1,32-33) -> 2F; 2F (38,30-31) -> 3F (38,32-33); 3F doors CA03-CA06 at
(26-27,12)/(34-35,12) and CC07 (8,34-35).
GOLD 3F Men's Staff Room = door at (34-35,12), from (34,13) push DOWN; the man there runs off (flag 318) -> chase him back to the 1F Boxing Gym.
GOLD done: from 1F (30,19) push DOWN into the gym door -> the runaway (Masked Circle member) scene, King Leo's next target note; control on the
Yumezaki map (state 17791). Next: Aerospace Museum (Kounan).
Aerospace Museum = Kounan node 15 (entry scene: Tadashi, King Leo sets fire). Dungeon file **F0109**: area 4 = entrance floor (25x17 tiles,
stairs (12-13,3)), areas 0-2 = 2F-4F (39x24); `0x1002` layer-B markers = the trapped kids (guide: 2F one group, 3F two, 4F two; the floors open
up as they are rescued). F0121 is resident as well (probably the roof / King Leo arena).
The museum has a **time limit** (7:56 on entering 2F): the countdown is an int32 seconds counter at `0x08E36DFC`; for bot runs hold it with
`P2_POKE_HOLD=1 P2_POKE=<vblank>,08e36dfc,x384` (900 s).
Museum kids: the `0x1002` markers are fire walls (Cross on them sets 1682, a message only). The children are in the exhibition rooms behind the
`C8nn` doors (rescue scenes = event scripts 072-076, flags 319-322/333/334; script 067 = the teacher/evacuation setup, 071 = broken elevator,
077 = the blimp escape). 2F south-west doors (6-7,34), (14-15,34), (22-23,34).
Museum 2F east: the south-east corridor is cut off (wall at col 33, fire at (43-46,31)); the Catwalk Anteroom is the east door `C80E` (47,22-23),
from (46,22) push LEFT (+col) -> script 068; choice at the 26th Cross: DOWN + Cross = "Leave this to Maya" (flag 331, needed for Maya's Prime
Persona); then the rope rescue and a flashback (flags 332, 490; state 19120).
Museum 2F -> 3F: the Catwalk Anteroom's far door DOA_1 (room x 11..50, z -1805..-1600) exits to 2F (46,34) in the south-east block, whose
bottom stairs `2800` (36-37,46) go UP to 3F (40,45) (state 19455). tools/doorsweep.sh tries a list of doors on a floor and reports the story
flags each one sets.
Guide (almarsguides chapter 3, user-provided link): museum kids 2F SW, 3F SE + SW, 4F SW + one more (a kid stuck in an elevator scene on 4F,
demons appear after it); then the roof (King Leo, weak to water). Mt. Iwato map images on that page: Pink Azas / Yellow Owl / Red Eagle / Blue Swan
mask locations and the B2F pit-fall room (".../AlmarsImages/Retro/Walkthroughs/PSP/Persona2InnocentSin/FullPlaythrough/Chapter3/*.png").
Museum 3F kids: south-east door (64,34) (flag 320) and south-west door (12-13,34) entered from (13,35) pushing UP (flag 319; state 20182);
the doors at (28,34), (48,34), (34,12), (43,12) are empty exhibit rooms.
Museum 3F -> 4F: 3F stairs (40-41,3) -> elevator kid scene (flag 333) -> 4F (F0109 area 2) at (38,12) inside the central elevator room
(state 20456). The room opens to the south at (36,19); the exhibit doors (`C804`-`C80B`) only open from the outer ring corridor (cols 1-4 /
73-76, rows 1-4), reached through row 35. 4F kids: west door `C807` (5,31) from (4,31) pushing RIGHT (flag 321; state 21382) and north-east door
`C809` (69,5) from (69,4) pushing DOWN (flags 322 + 492; state 22082). East `C80A/B` (72,30-31) and north-west `C804/5` (8-9,5) are empty.
The north-east ring pocket (63-64,2) traps the cellwalker (diagonal camera there); push LEFT then DOWN by hand to get out to (59,13).
**Stuck (2026-10-17):** after both 4F rescues the north stairs `2800/2801` (36-37,3) are still blocked by fire (Maya: "If we don't hurry, we'll
all be burned to death!"), and the stairs `A000` (40-41,3) are behind a closed shutter. Untested: the south stairs `2802/2803` (36-37,46) with the
NPC `2C00` at (36,45), and the unique layer-B marker `B096` at (63,8). Last state 22884 at (36,33), 4F.
**Aerospace Museum cleared (2026-10-17):** the roof is reached through the 4F **south** stairs `2802` (36-37,46) (walk to (37,45), push
DOWN) -> 5F stair room (F0109 area 3, 8x10 tiles, arrive (4,7)) -> north doors (2-5,0) from (3,1) pushing UP -> roof scene (Tamaki, Tatsuya
Sudou, the roof collapses; FMV) -> blimp escape (script 077) -> free control on the **Airship Bridge** (room scene; labels H_/E_/Y_SENTO, HENKO =
leader changes). The bridge exit: walk LEFT to the wall (57,-559), then hold DOWN -> deck scene "Esmeralda" -> King Leo battle. Its battle table
is layout A at **0x094EB2A0** (copy +0xB00; party 5 x 1 HP, King Leo 2067 HP); the generic scan in tools/bosswin.py picked a wrong 2-member
table first (game over, the game reloaded the chapter-1 memory card save), so bosswin now checks 0x094EB2A0 as a second known address.
Battle state 23205, won -> 23250 (Lv 23); back on the Kounan town map at Ebisu Beach (state 23258).
build/scratch/inbattle.py <slot> prints whether a layout-A/B table is filled (layout B gives false positives outside battles).
**Chapter 3 -> Mt. Iwato (2026-10-17):** Kounan exit node 25 -> Rengedai (travel.sh 23258 23300 25 4 0). Rengedai nodes: 14 = Mt. Iwato,
15 = Alaya Shrine, exits 7-11. Mt. Iwato is locked (no "IN" marker) until (1) the Kuzunoha Detective Agency scene (Hirasaka node 0 Kameya Alley,
mall grid DOWN, DOWN, RIGHT; Tamaki, flags 336-344; the agency EXIT quad x 411..588 z -1394..-1346: UP+RIGHT 20 frames from the scene's end
spot, then ONE Cross and Circle - extra Crosses re-pick the agency from the mall menu; Ms. Ideal scene on the way out sets 345) and (2) the
Alaya Shrine scene (flag 346). Then the Mt. Iwato node needs a fresh arrival (mapnav away to node 12 and back to 14) before Cross enters.
Kuchisake-Onna rumor and the four masks skipped (optional: Ultimate Personas). Mt. Iwato B1F entrance "Mt. Iwato: Exit" at (8,43) (state 23356).
