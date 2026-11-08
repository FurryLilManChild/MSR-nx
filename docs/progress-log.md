# Progress log (newest first)

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
