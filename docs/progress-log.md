# Progress log (newest first)

## 2026-11-09 (night): run 17 and 18 read, test 17 (single screen follows the pause menu, beams)

- Run 17 (test 16, docked 768 MHz): VRAM work about 600 to 430 ms per 5 s, stores about 1200 to
  150, right-eye draws run about halved, GPU busy about 40% to 30%, steady 29.6 to 30.
- Run 18 (test 16, handheld, GPU 307 MHz, 11 minutes): mostly 29.5 to 30; GPU busy 75 to 95% in
  busy rooms (draws 2.4 to 3.7 s, VRAM work 1.0 s per 5 s). Dips to 21 to 27 mostly with GPU time
  to spare. Handheld at stock is near the GPU's limit: the scaler will need more speed there.
- Test 17, single screen: the bottom screen is shown while the game's pause menu frame is on it
  (the header line at y 24; found in all 7 pause and Samus screen pictures of runs 10 to 18 and in
  none of the other 40), so closing the menu with B also goes back, and the screen that explains
  a new ability comes up. Minus and plus are only the game's Start there now.
- The top-right HUD crop is the lit beam slot (power beam or ice beam so far), and a new mapping
  "Next beam" (ZR) taps the next slot of the beam column, trying the one after if nothing changed.
- Pack: 161 fragment configurations, 191 shaders.

## 2026-11-09 (evening): run 16 read, test 16 (realigned frame plan, copies from images)

- Run 16 (test 15, docked, 768 MHz): the GPU timers check out (the three parts add to about 5000 ms
  each 5 s). Gameplay held 29 to 30 per second with the GPU busy about 35 to 40% (draws about
  1.3 s, VRAM work about 0.6 s per 5 s). The right eye still ran 10 to 19% as many draws as the
  left (the probe: 3%). A white top screen at 85 s set off the white-out capture; replayed on the
  reference renderer it is the same picture (583 of 96000 pixels differ by rounding), so it is the
  game's own. 7 new fragment shaders came back in fs_new.bin.
- Frame plan: operations were matched to last frame's by position, so one more or one fewer early
  on made everything after it run. Now it looks up to 48 places ahead to line the lists up again.
  Probe, 240 s of play: right-eye draws 23331 to 14473, 0 of 75 sampled top pictures differ from
  the run without it. (MSR_FPPOS=1 gives the old matching.)
- Copies from images: a display transfer or texture copy that reads only a render target the GPU
  drew since its last store now reads the target's image directly (k_transfer_img, k_copy_img in
  vram_ops.inc) instead of storing it to VRAM first. tools/tests/vram_ops_test.cpp checks both
  against store-then-transfer and store-then-copy for the game's cases and every format: all pass.
  Port menu row "Copies (test)": From images (default) or Through VRAM. Only the Switch runs this
  path; the receipt counts copies and transfers made from images.
- Pack: 159 fragment configurations, 189 shaders. White-out autocapture off.

## 2026-11-09 (night): test 14, test 15 (GPU timers)

- Test 14: in single screen the file select screen is shown by itself while it is up (found from
  pixels of its frame), back to the top screen when the game loads.
- Right-eye reuse (gpu/eye_reuse.c) was built and checked in the probe: replays of autocapture1
  and capture1 gave top and bottom framebuffers identical to runs without it. But the 240 s
  gameplay probe without it already ran only 23331 right-eye draws against 679648 left-eye draws
  (3.4%): the frame plan has been skipping the unseen right eye since test 9, and the Switch logs
  show the same plan at work. Reuse can save at most those few percent, so it is not in the menu;
  the code stays, off, for the probe (MSR_EYE=1). Two bugs found on the way: fresh tokens repeated
  (see pitfalls) and the eyes' depth buffers were learned from passes that name a depth buffer
  without using it.
- Test 15: GPU timers. Timestamps around each part of the GPU's work give, every 5 s in the
  receipt, "GPU time: draws, VRAM work, elsewhere or idle" in ms (the three should add up to about
  5000, which checks the tick conversion), plus "eyes: left N draws run of M, right N run of M".
  This tells whether test 16 (GX work kept on GPU images) or the draws themselves are where the GPU
  time goes.

## 2026-11-09 (evening): test 12 results, test 13

- Run013 (handheld): Fewer waits recorded about 62% of the barriers Safe did (3952 of 6384) with
  no visible frame rate change. Raising the GPU to 768 MHz with sys-clk made the game much smoother
  for Mark; the main thread's GPU wait fell from about 2.5 s to about 0.9 s per 5 s. The GPU clock
  is the handheld limit; the CPU clock barely matters.
- Test 13: GPU clock setting (system default, 460, 614, 768 MHz; reapplied after docking changes,
  system clocks restored at exit), single-screen HUD drawn with blending (dark backgrounds
  see-through, bright text solid), and in single screen a minus or plus tap sends Start and shows
  the map, another tap goes back.

## 2026-11-09 (late afternoon): tests 11/11b results, test 12 (single screen, GPU sync test)

- Runs 011/012 (handheld, stock): menu, clock switch, pause and settings all worked; 26 shaders
  compiled in play (one vertex shader, 1.9 s, interpreter meanwhile); general shader unused. Mostly
  28.5 to 30; two GPU-bound dips to about 20 (199 to 204 s, 269 to 274 s) with the main thread
  waiting 1.6 to 2.4 s of every 5 for the GPU and nothing compiling.
- Test 12: single-screen layout (bottom-screen readouts cut out and drawn at 2x on the top screen
  while the game's HUD frame is visible; minus tap toggles the map), frame counter, and a port menu
  "GPU sync (test)" row: Safe (every barrier, all caches), Fewer waits (no barrier when nothing was
  recorded since the last), Lean (also no L2 invalidate). Barrier counts go to the receipt. Pack:
  152 fragment configurations, 181 shaders.

## 2026-11-09 (afternoon): test 10 results, test 11 (vertex compiles, port menu)

- Test 10 (docked): the four pack configurations compiled on the Switch drew identically to the
  pack's; 20 shaders compiled in play at about 0.9 s each; the general shader was never used; 29.5
  to 30 pictures per second throughout except 231 to 236 s (about 21), when three new vertex
  shaders ran on the interpreter (462 draws).
- Test 11: runtime/gpu/vsgen.c is pica2glsl.py in C (tools/tests/vsgen_test.sh: identical output on
  all 51 dumped configurations), so vertex shaders missing from the pack are compiled on the Switch
  too (the interpreter draws meanwhile). Two compile threads were not done: uam's front end is
  shared global state (glsl_frontend_init/exit), and on the host the front end, not the optimizer,
  is most of the time (opt levels 0 to 3 all about 120 ms).
- Port menu (runtime/switch/port_menu.c): hold minus, game paused (os/sched.c pause point, clock
  without the paused time), screen layout (top large with bottom small, side by side, stacked, top
  only), CPU clock (stock 1020 or boost 1785 MHz), mapping of the 3DS A, B, X, Y, L, R, Start and
  Select to Switch buttons; saved to sdmc:/switch/msr/settings.ini. A minus tap still reaches the
  game as Select. Touch follows the layout. Pack: 128 fragment configurations, 156 shaders.

## 2026-11-09 (midday): test 9, shaders compiled on the Switch

- Test 9: no white-out in a new area, but the start-up self-test answered the question: the general
  fragment shader differs from the pack's in 84 of 93 configurations, the rewritten one exactly as
  much as test 8's. Local memory was not the cause; the general shader is wrong on the Switch GPU
  for reasons still unknown. With every shader in the pack the game holds 30 pictures per second at
  handheld clocks; the slow spots are configurations missing from the pack.
- Test 10: uam (devkitPro's deko3d shader compiler, pinned in tools/switch/uam/build.sh) is built for
  the Switch and linked in (about 2.3 MB). A missing fragment configuration is specialized on the
  Switch exactly as fsgen.py does (checked byte for byte on the host), compiled on a worker thread,
  and saved to sdmc:/switch/msr/shadercache, which is loaded at start. Its draws sit out until the
  shader is ready; the general shader is only a fallback if a compile fails. The self-test now also
  compiles four pack configurations on the Switch and compares them with the pack's. 112 fragment
  configurations in the pack (19 from run009).

## 2026-11-09 (afternoon): test 8, white-out traced to the general fragment shader

- Test 8: the white-out came back in new areas; the build's own detector caught it and saved a
  capture (autocapture1.bin, run008). Replaying that frame in the probe on the reference renderer
  gives the correct cave picture from the same starting VRAM and command lists, so the fault is in
  the Switch renderer, not in game state. The scene's draws only read textures from FCRAM, the
  bottom screen came out about 3 times too bright as well, and the bloom chain matches.
- Two findings: (1) the fragment key used bit 63 both as part of the hash and as the early-depth
  flag, so 11 of the 27 configurations reported missing were in the pack and still ran on the
  general shader; (2) the general fragment shader is the only shader with local memory (2048 bytes
  per warp, from arrays indexed at run time), and tests 3 to 8 only went bright or white where it
  drew. Neither is proven to be the white-out yet.
- For test 9: keys keep bit 63 clear (the existing pack stays valid), the general shader has no
  arrays (uam: 0 bytes of local memory, 85 registers), 16 new configurations from run008 in the
  pack (93 fragment configurations, 116 shaders), and a start-up self-test that draws every pack
  configuration with the pack's shader, the new general shader and test 8's, and writes which
  differ to the receipt with a picture (selftest.png).
- Frame plan checked against the capture: the skipped operations in that frame (right eye copies
  and its glow chain) only fed draws that a later full-screen draw replaced.

## 2026-11-09 (day): tests 6 to 7

- Test 6 crashed at start (guest memory read before it was mapped; graphics-only queue). Test 6b ran
  the title at 28 to 30 pictures per second, then crashed in a cutscene: command memory only freed
  by waiting flushes (Atmosphere crash report + addr2line on the kept msr.elf). Test 6c: command and
  dynamic memory go around fenced rings; playable, 30 at the start, 6 to 9 in areas with fragment
  configurations missing from the pack; GPU at 307 MHz (handheld).
- White-out (whole screen white with blurred yellow): seen in tests 5 and 6c, in the same kind of
  area. The Switch shaders' lighting and combiner code matches the reference line by line; cause
  unknown. runtime/gpu/capture.c records one frame (ZL + ZR + minus on the Switch); the probe
  replays it on the reference renderer (MSR_REPLAY). Self-check: a title frame and a gameplay frame
  captured and replayed in the probe give byte-identical VRAM.
- Test 7: 11 more fragment configurations, early-depth variants for z-buffer draws, screen output
  waits on its own fence instead of the whole queue under the lock, audio time stretching.

## 2026-11-09 (morning): VRAM on the GPU, vertex fetch on the GPU

- The 3DS VRAM lives in Switch GPU memory. Fills, display transfers, texture copies, DMA into VRAM,
  render target stores and loads, texture decoding from VRAM and the screen conversion run as
  compute shaders from runtime/gpu/dk/vram_ops.inc. tools/tests/vram_ops_test.cpp runs the same
  source on the CPU against the reference code: all cases byte-identical (every transfer format and
  mode, all 14 texture formats, the game's own parameter sets).
- Vertex fetch: pica_vertex_layout gives each shader input's loader, offset, type and size; the
  game's vertex bytes are copied as they are and decoded in the vertex shader. Probe check
  (MSR_VFETCH_CHECK) over 200 s: 504804 draws, 359 million inputs, no difference.

## 2026-11-08 (night): tests 4 and 5, right eye skipped

- Test 4 (one launch cycling four shader modes, pictures saved by the build): mode 1 (translated
  vertex shaders, specialized fragment shaders) looked correct and ran about 10 pictures per second
  at CPU 1020 / GPU 307 / memory 1331 MHz; the old shaders ran 3 to 7. Mode 1 is now the only mode.
- Fragment specialization (tools/shaders/fsgen.py): pica_fsh.glsl with the configuration registers
  as constants; 33 configurations, 9 to 39 registers, no spill.
- The game renders both eyes every frame (its slider read at 0x21B630 returns 0, both eyes are drawn
  anyway). runtime/gpu/frame_plan.c records each frame's GPU operations with the memory they read
  and write, works out which ones feed the shown left framebuffers, and skips the rest next frame
  when the same operation comes up at the same position. Probe check over 240 s of title, intro and
  gameplay: 41% of GPU operations skipped, all 150 saved pictures byte-identical to a run without
  skipping (MSR_NOSKIP=1).
- Readbacks go through a CPU-cached buffer (the uncached dynamic area cost about 2 ms each).
- Remaining costs to attack next: texture copies and display transfers of GPU pictures run through
  the CPU (readback, convert, re-upload), and vertex attributes are decoded on the CPU.

## 2026-11-08 (afternoon and evening): Switch tests 1 to 3, speed

- Switch test 1 (deko3d renderer): reached the title screen, picture upside down (reflected top to
  bottom). Cause, from deko3d's source: OriginLowerLeft sets HAL_TRANSFORM_FLIP_V on the window; our
  present path blits rows in memory order. Fix: screen pictures are stored bottom to top. Confirmed
  fixed by test 2.
- Probe profile (gprof, 14 s of game time): 85% of CPU time was display transfers (bloom chain and
  screen output, about 240 per second) done with float colors. Integer, specialized loops: 6.5 s to
  2.0 s, every saved frame byte-identical. RGBA8 surface readback and upload walk 8x8 tiles.
- Switch test 2: speed meter in the receipt. 1.4 pictures per second; per 5 s, about 4.4 s waiting
  for the GPU, 70 ms of transfers, 95 ms of vertex fetch. Button presses reach the game (A works when
  held, the game was too slow to see short presses).
- Cause of the GPU time, measured from the .dksh headers: the vertex interpreter shader used 255
  registers and 43520 bytes of spill per warp. The fragment uber-shader: 73 registers, 2048 bytes.
- Vertex shader translation (tools/shaders/pica2glsl.py): each configuration (program reachable from
  the entry point, swizzles, output map) becomes its own GLSL shader with plain registers and
  structured control flow; the same text compiles as C++ for the probe's check. 12 configurations
  cover the title screen, intro and first gameplay; the check compared 7.2 million vertices against
  the reference interpreter with no difference. Translated shaders: 20 to 47 registers, no spill.
  Pack format: shaders.pack (built from game data, SD card only). Misses fall back to the interpreter
  and are saved to vs_new.bin. Queue scratch raised to 0xAA00 per warp for that fallback.
- Probe statistics: two 240x400 render targets, no draw samples a render target directly (the game
  copies them with texture copies first), 4 display transfers and about 3 texture copies per frame.

## 2026-11-08 (early morning): first pictures, title screen, gameplay (cloud probe)

- PICA200 command processor, vertex shader interpreter and a software reference renderer (probe
  only, never shipped) write PNG frames. First frame: planet SR388.
- View matrices were NaN: cfg block 0x50005 (stereo camera settings) was answered with zeros and
  the game divided by it. Retail defaults now. Found with a NaN-check build of the generated code.
- Title screen: triangle culling was the wrong way round (every 2D layer culled) and clamp-to-border
  wrap always read texel 0 (final composite black). Fixed.
- File select, story intro, save data written (profile0/*.bmssv), then gameplay.
- "Frozen" first scene: Samus moves with the circle pad, not the d-pad; she was invisible because
  w-buffer depth was computed as w * scale (negative, clamped to 0) instead of (z/w * scale +
  offset) * w. Fixed: Samus on her gunship, running and jumping.
- Native DSP audio: 24 voices (PCM8, PCM16, DSP-ADPCM), status reports, mixer with aux busses,
  output ring for the platform layer, WAV dump in the probe.
- Probe speed: pixel work only for saved frames; 20 s of game time runs in about 19 s.
- Probe options: MSR_SECONDS, MSR_FRAME_EVERY, MSR_FRAMES_AFTER, MSR_INPUT (buttons and circle pad
  over game-time ranges), MSR_WAV, MSR_DRAWLOG, MSR_DUMPRT.

## 2026-11-07 (night): probes 5 to 9 in the cloud (Claude runs the probe loop)

- Probe 6: native fs:USER (SelfNCCH RomFS from romfs.bin offset 0x1000) and cfg:u; the RomFS opened
  and read, then the main thread spun in an ldrex/strex lock slow path (F_0018BA84) on an
  uninitialized lock at 0x7C9D64.
- Probe 7: thread scheduler (`runtime/os/sched.c`): one host thread per guest thread, a baton so
  one runs at a time, strict priority (lower number first), handover at kernel calls, events with
  reset types, recursive mutexes, semaphores, address arbiter wait/signal, sleeps and timeouts on a
  virtual clock. Worker threads ran, but the spin stayed. Probe 6's guess (workers set the lock up)
  was wrong.
- Cause found in the code: F_004271E0 sets the lock up only if F_00428730 succeeds; it returned
  0xC8A0A801 because the byte at 0x76C9E1 was 0. That byte is the "component loaded" output of
  dsp::DSP LoadComponent (0x1ADE10 stores reply word 2 there); the stand-in reply gave 0.
- Probe 8: native dsp::DSP (accepts the firmware upload, audio pipe answers like the firmware,
  semaphore and interrupt events). Past the lock: 12 threads, gsp RegisterInterruptRelayQueue,
  GX TriggerCmdReqQueue, then every thread waiting (main on an arbiter, the GSP thread on its
  interrupt event).
- Probe 9: periodic hardware events on the virtual clock (vblank 59.83 Hz, DSP frame 4.89 ms), the
  GSP interrupt relay queue, GX commands (DMA and memory fill done natively; command lists, display
  transfers and texture copies acknowledged until the renderer exists). Reached hid:USER, then an
  unexpected return at 0x1010A8: `blx 0x2FE048`, the compiler's Thumb switch helper
  (__ARM_common_switch8, byte table after the call). Generator now turns it into a switch.
- Probe 10: crash reading guest address 4 while loading effect assets: the name pointer was wrong.
  `add r1, pc, #112, #30` (a non-canonical rotated immediate, 654 such ADR forms in the game) was
  emitted with the unrotated byte. Fixed: data-processing immediates come from the encoding; the
  instruction test sampler now takes every operand shape (a per-kind cap had hidden this one).
- Probe 11: the asset loader ran through; then fs OpenArchive save data and a stand-in OpenDirectory
  led the game to err:f ThrowFatalError. Added a save data archive in a host folder (format,
  open/create/delete/rename files and folders, directory listing, read/write/size), and err:f now
  stops the probe with the game's error.
- Probe 12: the main thread woke every vblank and slept again: it waits for the GSP to take its
  framebuffer swap (dirty flag in shared memory 0x200). The vblank now latches swaps.
- Probe 13: the game runs its frame loop: about 278 frames in 20 s of game time, each with 8 GPU
  command lists, display transfers and swaps on both screens. Next: the PICA200 command processor.

## 2026-11-07 (evening): probes 1 to 3 on Mark's PC

- Probe 1: the generated code ran from the entry point; the game looped on svc QueryMemory
  (stand-in gave no region info). Fixed: QueryMemory answers from a region table.
- Probe 2: startup reached srv: (RegisterClient), APT:A (cmd 0x0101), heap 0x08000000 size 0x40000,
  linear heap 0x14000000 size 0x37C0000, then "lost control" at 0x0016E754.
- Probe 3 (unexpected-return logging): the constructor loop at 0x104D3C (init_array 0x6FDF18, 1204
  relative entries) called entry 46 (0x603B8C), which tail-calls the false entry 0x610074; that
  function's generated body started at 0x107CB0 (lowest address, a veneer reached by a branch)
  instead of its entry. Cause: functions were emitted in address order with no jump to the entry.
  1,185 of 22,697 functions had code below their entry. Fixed: `goto L_entry` first.
- Lookup now refuses a Thumb target that the function list has as ARM.
- Probe 4: past the constructor at 0x603B8C (new svc 27 DuplicateHandle); stopped at an undetected
  jump table, `ldrlo pc, [pc, r4, lsl #2]` at 0x42571C with its `cmp r4, #6` five instructions
  earlier (stores in between). The jump was taken as a return. Fixed: search 16 instructions back,
  `lo` tables have n entries (`ls` n + 1). Tables found: 141, the same as msr2ds. The 7 remaining
  non-stack loads into pc first switch over the function's block starts.

## 2026-11-07 (day): generator, instruction tests, probe runtime

- 3dsrecomp cross-check (Mark's PC): 22,959 functions, 90.6% code, 5.1% unreached, same largest
  unreached runs as msr2ds. msr2ds's list (22,697) is the base.
- Our generator (`tools/gen`): decoder walks all 22,697 functions (1,418,854 instructions, 107 ARM
  and 39 Thumb instruction ids); translator to C; whole-game output 27 units, 13 untranslated
  instructions (12 user-mode ldm/stm, 1 cdp: privileged forms an application cannot run, so data
  between functions; logged if ever reached).
- Instruction tests (`tools/gen/test_insns.py`) against Unicorn (ARM1176), 751 instruction samples
  x 16 random states: all pass except accesses whose address wraps past 0xFFFFFFFF (Unicorn wraps to
  0; our window does not; games never do this).
- Bugs the tests found and fixed: capstone's update_flags is set for adc/rsc without S (now read
  from bit 20), capstone misses post-indexing for ldrd/strd (now read from the P bit), ARM shift
  aliases with two operands, str pc stores pc + 8.
- One 5 MB unit compiles at -O2 in 45 s on the cloud machine.
- Probe runtime: 4 GiB window (VirtualAlloc reserve, commit on demand), dispatcher for register
  calls/jumps, first 3DS kernel calls (memory, handles, ticks, ports, srv:GetServiceHandle; other
  IPC answered with stand-in success and logged), crash handler with the last 64 functions entered.
  Linux and MinGW-w64 compile checks pass.

## 2026-11-07: kickoff

- Dump: decrypted USA cartridge, `Metroid - Samus Returns (USA) (En,Fr,Es).3ds`, NoCrypto,
  title 00040000001BB200, product CTR-P-A9AE, SHA-1 b8c678dc4ea32c661e020dc264440c4209374ded
  (not checked against a dump database).
- Extracted with TriAevum `ctr_rom.py` and msr2ds `ctr.py`; both give the same code.bin.
  code.bin sha256 669090e6caeef405aaf4d2b884749460f8f0b868ecd158c7f1943ac235c102c6 (6,754,304 bytes),
  exheader.bin sha256 1ebde6cc7304affc99417af4fd9cd748057d0e2ba38e8d32650042801c322fa6,
  romfs.bin sha256 4e1884ae07f531023f75a00e1d5b7c2c57badd2ca18708829dac451fd46a79df (709,132,288 bytes).
- Layout: text 0x00100000 (6,047,816 bytes), rodata 0x006C5000 (238,056), data 0x00700000
  (459,128), bss 384,528. Title name in the exheader: MATADORA. No CRO modules found in the RomFS.
- msr2ds `analyze.py` on our dump: 22,697 functions (21,833 ARM, 864 Thumb), 1,373,701
  instructions, 217,358 VFP instructions, 164 svc sites, 141 jump tables, 1 unresolved indirect
  jump (0x20CAD8). Matches msr2ds's published numbers.
- Renderer direction: deko3d first (TriAevum-Switch's precompiled package plus uber fallback
  solves runtime PICA shaders; render thread CPU 34.3% to 20.5% vs Mesa GL on OoT3D). Vulkan (NVK)
  is the fallback.
- Workflow: PC bring-up for game logic and services, Switch for renderer, speed and controls.
- Blocked: building fearkov/3dsrecomp from the session (safety check on building outside code).
  Mark can run it in PowerShell for the cross-check; not blocking.
