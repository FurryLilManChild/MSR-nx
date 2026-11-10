# MSR-nx

A native Nintendo Switch port of **Metroid: Samus Returns** (Nintendo 3DS, 2017), made by static recompilation.

It is not an emulator: the game's ARM code (`code.bin`) is translated to C and compiled for the Switch, its 3DS
system services are rewritten natively, and the 3DS graphics chip (PICA200) is drawn with deko3d. You need your own
copy of the game.

## Status

**Version 0.5.0 is the first test release.** The game is playable from the start, at the 3DS's 30 frames per second,
handheld or docked at stock clocks. It has not been played to the end, so expect problems further in, and please
report them (see below).

| Part | State |
| --- | --- |
| Code translation | Done: all 22,697 functions |
| System services | Files, saves, input, sound, timing |
| Renderer | deko3d: the game's shaders translated ahead of time, missing ones compiled on the Switch and cached |
| Screens | Four layouts, including a single-screen mode for TV play |
| Resolution | 1x, 1.5x or 2x for the 3D top screen |

Session-by-session notes: [docs/progress-log.md](docs/progress-log.md).

### Known issues

- Only the early areas have been played. Anything past them is untested.
- The first time an unusual effect appears, its shader is compiled on the Switch (about a second, the effect is
  missing meanwhile). It is saved to `sdmc:/switch/msr/shadercache/` and is instant after that.
- At 2x the GPU works hard: expect slowdown handheld at stock clocks. Raise the GPU clock in the port menu or use
  1.5x.
- Single-screen mode reads the bottom screen to know what is up (pause menu, file select, ability screens). A screen
  it does not know stays on the top screen view: tap minus or plus to pause and see the bottom screen.

## Download and install

You need a Switch with custom firmware (Atmosphere) and your own copy of the game: the USA cartridge, dumped
**decrypted** as a `.3ds` file (GodMode9 makes one with "Dump cartridge" and then "Decrypt").

1. Download `MSR-nx-v0.5.0-switch.zip` from this repository's **Releases** page.
2. Extract the zip to the root of your SD card. It creates `switch/msr/` with `msr.nro` in it.
3. On a PC with Python 3, make the game files from your dump with the script in the zip:
   `python extract_3ds.py "Metroid - Samus Returns (USA) (En,Fr,Es).3ds" msr_files`
   It writes `code.bin`, `exheader.bin` and `romfs.bin` (about 680 MB) and checks them against the supported
   version.
4. Copy those three files into `switch/msr/` on the SD card, next to `msr.nro`.
5. On the Switch, hold **R** while opening any game to get the homebrew menu with full memory, and start **MSR-nx**.

Your save is kept in `sdmc:/switch/msr/save/`.

## Controls

The 3DS buttons are on the same Switch buttons (A, B, X, Y, L, R; plus is Start, minus is Select). The touch screen
works in handheld mode wherever the bottom screen is shown.

| Switch | Does |
| --- | --- |
| Hold minus (half a second) | Port menu (the game pauses) |
| ZR | Next beam (taps the beam list on the bottom screen for you) |
| D-pad | Aeion abilities, as on the 3DS |
| Both sticks pressed in | Picture of the screen, saved to the run's log folder |
| L + R + plus + minus | Quit |

In single-screen mode, minus or plus pauses the game and shows the bottom screen (map and Samus screens); it goes
back when the pause menu closes. Energy, missiles, the Aeion gauge, the selected beam and the Metroid counter are
drawn over the top screen.

## The port menu

Up/down picks a row, left/right changes it, A confirms, B or minus closes. Settings are saved to
`sdmc:/switch/msr/settings.ini`.

| Setting | Choices |
| --- | --- |
| Screen layout | Top large with bottom small, side by side, stacked, single screen |
| 3D screen resolution | 1x (native), 1.5x, 2x |
| CPU clock | Stock (1020 MHz) or boost (1785 MHz) |
| GPU clock | System default, 460, 614 or 768 MHz |
| Frame counter | On or off |
| GPU sync, Copies | Renderer options for testing; leave them at Fewer waits and From images |
| Button mapping | Any Switch button for each 3DS button and for Next beam |

## Reporting problems

Open an issue with where in the game it happened, what you saw, and the newest folder from
`sdmc:/switch/msr/switchlogs/` (`receipt.txt` and any pictures in it). The last ten runs are kept.

## Building

The game's code is generated from your own `code.bin`, so there is nothing to build without the game.

1. `tools/extract_3ds.py` on your dump, into `extract/` (never committed).
2. `tools/gen/generate.py` translates `code.bin` to C.
3. The Switch build runs in devkitPro's devkitA64 Docker image: see [tools/switch/README.md](tools/switch/README.md).

## Credits and license

MSR-nx is MIT licensed (see [LICENSE](LICENSE)); third-party parts are listed in [NOTICE.md](NOTICE.md).
Metroid and Samus Returns are trademarks of Nintendo. This project is not affiliated with Nintendo and contains no
game data.
