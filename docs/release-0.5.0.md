# MSR-nx 0.5.0 (test release)

The first public build of MSR-nx, a native Switch port of Metroid: Samus Returns made by static recompilation. You
need your own decrypted USA cartridge dump; see the README for the three-file setup.

## What works

- The game from the start, at 30 frames per second, handheld and docked at stock clocks.
- deko3d renderer with the game's shaders translated ahead of time; any missing one is compiled on the Switch once
  and cached.
- Screen layouts: top large with bottom small, side by side, stacked, and a single-screen mode for TV play (the
  bottom screen comes up by itself for the pause menu, file select and new-ability screens; the HUD readouts are
  drawn over the top screen).
- 3D screen resolution 1x, 1.5x or 2x.
- Port menu (hold minus): layout, resolution, CPU and GPU clocks, frame counter, button mapping.
- ZR switches beams without the touch screen; both sticks pressed in saves a picture of the screen.

## Known issues

- Only the early areas have been played; later areas are untested.
- The first appearance of an unusual effect may be missing for about a second while its shader compiles.
- 2x can slow down handheld at stock clocks.

## Reporting problems

Open an issue with where it happened, what you saw, and the newest folder from `sdmc:/switch/msr/switchlogs/`.
