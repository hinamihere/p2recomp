# Host architecture

The recompiled game is ordinary C++ produced by `framework/tools/codegen_main.cpp`: every guest
basic block becomes a labelled block inside one function per 16 KiB "unit" of the executable, and
control leaves a unit by setting `ctx.pc` and returning to `Runtime::run`, the outer dispatch loop.
Everything the PSP's firmware would do is reimplemented in `game/src/`.

## Execution and threads (`kernel.cpp`)

There is exactly one live register context, `Runtime::cpu()`. A PSP thread switch copies that
context into the outgoing `Thread` and loads the incoming one, then sets a new runtime thread
identity; the identity change tells generated code (which may chain calls natively across units)
that it must unwind instead of resuming a stale native frame.

* **Blocking calls** (`WaitSema`, `DelayThread`, `WaitVblank`, ...) run inside an HLE import. The
  caller is saved with `pc = $ra` so it resumes after the import, and its eventual return value is
  written into the saved `v0` when it is woken.
* **Scheduling** is strict priority with FIFO order inside a priority, re-evaluated whenever an
  HLE call readies a thread and at event processing points.
* **Idle**: when no thread is ready the context switches to a host function registered at
  `kIdleAddr`, which processes events and sleeps until the next deadline.
* **Guest code called by the host** (interrupt handlers, thread callbacks, the MPEG read
  callback) runs with `$ra` pointing at `kNestedReturnAddr`, a registered host function that
  restores the interrupted context and runs a completion handler.

`process_events` advances real time: expires waits, raises vblank (59.94 Hz), queues vblank and GE
interrupts, runs audio wakeups, and preempts. It is called from the post-dispatch hook (every 32
outer dispatches), the framework's starvation hook (inside long native call chains) and idle.

## Memory

User memory 0x08800000-0x0A000000 minus the module image is a first-fit allocator used for
`sceKernelAllocPartitionMemory` and thread stacks (allocated from the top, like the PSP). The first
16 KiB of user memory (below the module at 0x08804000) holds the host trampolines and the interrupt
stack.

## Graphics (`ge.cpp`, `gpu_soft.cpp`)

Display lists execute synchronously when enqueued or when the stall address advances. The
rasterizer writes straight into guest VRAM, so render-to-texture, block transfers and CPU reads of
the framebuffer all see the same memory the game expects. At each vblank `display.cpp` converts the
framebuffer chosen by `sceDisplaySetFrameBuf` to RGBA and hands it to SDL.

Rasterization: vertex decode (all formats, morphing, skinning), transform and lighting, near-plane
clipping, span-based triangle traversal with incremental plane equations, sprites, lines; textures
in all PSP formats incl. swizzling, CLUT and DXT; texture functions, fog, alpha/colour/stencil/depth
tests, blending, logic ops and write masks. Not implemented: Bezier/spline patches.

### Batched, banded rasterization

`draw_prim` transforms and clips at once but only queues the resulting screen-space primitives,
each with a snapshot of its pipeline state and palette. The queue is rasterized in horizontal
bands on a small thread pool, every band processing all primitives in issue order, so results
equal sequential drawing (`P2_RASTER_VERIFY=1` checks this). Like the real GE the queue keeps
going while a list is stalled; it is flushed at list completion, `sceGeDrawSync`/`ListSync`,
block transfers, vblank (before presenting), CPU writes to VRAM (`sceMpegAvcCsc`), savestates,
and before a draw samples memory a queued draw renders into. CLUT-indexed 16/32-bit textures
are drawn immediately.

## Audio

* `audio.cpp`: eight `sceAudio` channels mixed on SDL's audio thread; blocking output suspends the
  caller until its channel has drained to one buffer. Without an audio device, or while
  fast-forwarding, the queues drain at 44.1 kHz of game time instead.
* `sas.cpp`, `sas_reverb.cpp`: the SAS voice mixer (VAG ADPCM, PCM, noise; pitch; ADSR envelopes)
  and its reverb.
* `atrac.cpp`: ATRAC3/ATRAC3+ streaming for BGM. Looping streams are fed to the game as a virtual
  frame sequence that wraps at the loop point, so the ring buffer never runs dry.

## Movies (`mpeg.cpp`)

`sceMpegRingbufferPut` calls the game's read callback as nested guest code; every 2048-byte pack
it wrote is copied and demuxed by a small MPEG-PS parser. Video is decoded with FFmpeg's H.264
decoder and converted to RGBA in `sceMpegAvcCsc`; ATRAC3+ audio uses the same decoder as BGM.

## Files and saves (`io.cpp`, `savedata.cpp`)

`disc0:`/`umd0:` paths resolve into the extracted ISO tree; `sce_lbnXXX_sizeYYY` paths read
sectors from the ISO. `ms0:` maps to `memstick/`. Savedata is stored unencrypted in
`memstick/PSP/SAVEDATA/<game><save>/` with a generated `PARAM.SFO`.

## Savestates (`savestate.cpp`)

A state is guest RAM (32 MiB) and VRAM verbatim plus a section per host module, each written by
that module next to its own state: kernel objects and thread contexts, GE lists and registers,
display/controller, audio channel setup (queued samples are dropped), SAS voices and reverb,
ATRAC streams, MPEG contexts, open files (reopened by path), utility dialogs. FFmpeg and ATRAC
decoders are recreated; ATRAC loses one frame of overlap history, a movie a few pictures until
the next keyframe.

Requests (F5/F8, `--load-state`, ...) are serviced only where no native frame of guest code is
half way through something: the post-dispatch hook, the starvation boundary, the kernel idle loop
and right after an import returns on the same thread (that thread then resumes at `ra`). A state
is not taken while an interrupt handler or a callback runs (those hold host closures); the request
waits up to two seconds for such a point. Loading switches the runtime thread identity, which
unwinds any nested native frames exactly like a thread switch.

## Debugging aids

`--input`, `--dump-frames`, `--dump-audio`, `--exit-after`, `P2_GE_TRACE`, `P2_THREAD_DUMP`,
`PSPRECOMP_HLE_HISTOGRAM`, `tools/disasm.py`, and the savestate-driven `tools/drive.py`,
`tools/explore.py`, `tools/talkhunt.py` (see README).
