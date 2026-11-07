# Progress log (newest first)

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
