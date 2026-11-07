# Progress log (newest first)

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
