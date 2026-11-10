# MSR-nx 0.6.0 (test release)

A native Switch port of Metroid: Samus Returns made by static recompilation. You need your own decrypted USA
cartridge dump; see the README for the three-file setup (unchanged from 0.5.0: keep your code.bin, exheader.bin and
romfs.bin, and replace msr.nro).

## New

- 60 frames per second (port menu, "Frame rate"). It holds at 60 docked with the CPU clock on boost (1785 MHz) and
  the GPU at 768 MHz, at 1x and 1.5x. The game's frame limit and its fixed 1/30 s interface steps are patched in
  the translated code.
- Graphics thread: the game's graphics commands (3D command lists, display transfers, texture copies, fills) run in
  order on their own CPU core beside the game, as the 3DS's GPU ran beside its CPU. Screens, input and sound moved to
  a third core. This roughly doubled what the CPU side can do per frame.
- Logs are written by the main loop about once a second, never by the game's thread (writing them to the SD card
  there cost the game about 40% of its time at 60).
- Run log folders are numbered after the highest one and the last thirty are kept (0.5.0 could reuse old numbers).

## Known issues

- Handheld 60 is not reached yet: about 40 at the default GPU clock and 48 at 460 MHz. The GPU is the limit.
- 2x at 60 is too much for the GPU.
- At 60, anything that moves at the wrong speed is a timing patch we missed: please report it with where it was.
- Only the early areas have been played; later areas are untested.
- The first appearance of an unusual effect may be missing for about a second while its shader compiles.

## Reporting problems

Open an issue with where it happened, what you saw, and the newest folder from `sdmc:/switch/msr/switchlogs/`.
