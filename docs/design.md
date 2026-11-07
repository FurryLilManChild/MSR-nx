# MSR-nx design (draft, 2026-11-07)

Status: proposal for Mark to review. Nothing here is built yet.

## Goal

Samus Returns as a native Switch program: every game function is a C++ function called with a
real call and returning with a real return, game memory is a plain block of Switch memory, timing
is driven by the events the game waits on (VBlank, DSP frames, timers), and 3DS system services
are native code. Fast enough at stock clocks if the game allows it.

## What we know about the game (from our dump and msr2ds)

- One executable, no CRO modules: text 0x00100000 to about 0x006C4000, 22,697 functions,
  1.37 million instructions, 98% ARM, 2% Thumb, 217k VFP instructions.
- 30 fps game. The main loop (0x156450) sleeps until 33 ms have passed since the last frame,
  then waits for VBlank. Movement uses the measured frame time, so a higher rate is a shorter
  limit (the `cmp r0, #33` at 0x156484 and `rsb r6, r0, #33` at 0x15648C) plus fixed 1/30 s UI
  steps at 0x49DFF8, 0x4E3BA4 and 0x10C6B4 (msr2ds `frame_rate.h`).
- On the 3DS the game keeps its 268 MHz ARM11 busy about 19 of every 33 ms (msr2ds note).
  The Switch's A57 at 1020 MHz has several times that, before any native gain. 60 fps looks
  within reach; to be measured on the console, not promised.
- Amiibo extras are flag reads at 0x3F8D40 and nearby (msr2ds `cheats.h`).

## Pieces and where they come from

| Piece | Plan | Reference |
|---|---|---|
| Function list | msr2ds `analyze.py` output plus its `extra_entries.txt`; 3dsrecomp's list as a cross-check | msr2ds, 3dsrecomp |
| ARM/Thumb/VFP to C++ | Our generator. Functions as C++ functions with direct calls; guest registers in host locals; flags computed lazily; indirect calls through one table lookup; no dispatcher between functions. | ideas from TriAevum's whole-AOT generator (direct calls, flow structs, VFP on native AArch64, TBAA) and 3dsrecomp's emitter |
| Guest threads | Real Switch threads only if the game's threads are independent; otherwise cooperative on one core like TriAevum. Decide after mapping the game's svc CreateThread use (1 site). | TriAevum `NativeA32Process` |
| 3DS services (fs, gsp, hid, dsp, ptm, apt, cfg) | Native implementations, only the calls the game makes (51 SendSyncRequest sites to trace) | Zakuro (MIT), TriAevum's CTR services |
| Audio (DSP) | HLE DSP mixing, as both upstreams do | Azahar audio core via TriAevum |
| PICA200 front end | Command list decode, shader generation from TEV state | TriAevum PICA front end |
| Renderer (Switch) | deko3d: precompiled package of the game's shaders plus parametric uber fallback, nothing compiled mid-game; Vulkan (NVK) as fallback | TriAevum-Switch deko3d backend |
| Renderer (PC bring-up) | Whatever backend gets the game logic on screen fastest on Windows | TriAevum desktop backends |
| Single screen, HUD, controls | Our own port menu (hold Minus), Dread-style HUD, beam cycling on ZR, morph ball on ZL | msr2ds hook locations |

## Phases

1. Generator and probe: translate the whole game, run its first blocks on PC and on a probe NRO.
2. Services: boot to the title screen on PC.
3. PICA front end and renderer: title and gameplay on PC, then deko3d on the Switch.
4. Speed on the Switch (profile first), controls, single screen, port menu.
5. Coverage test and release.

## Open questions for Mark

- Repo license: GPL-3.0-or-later proposed (needed if we take TriAevum pieces).
- Frame rate target: 30 (original) first, 60 as an option once the game runs.
