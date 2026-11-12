# MSR-nx 0.6.1 (test release)

A fix for issue #1: the game stopped when Samus hit certain objects.

## Fixed

- The crash on hitting objects (issue #1, reported by JAGSTAX, with the USA and EUR dumps). The game builds a
  piece of text with its own formatting routine, which calls a small helper for each character. The game reaches
  that helper only through an address it works out while running, so the port's function list did not have it and
  the call stopped the game ("unknown call target" in the receipt). The code generator now finds functions reached
  that way: 7 in all, including this one.
- If a call ever reaches code the port does not know, the receipt now names the address.

## Installing

Replace `switch/msr/msr.nro` on the SD card with the one in the zip. Your `code.bin`, `exheader.bin`, `romfs.bin`
and save stay as they are.

Everything else is as in 0.6.0: see the README for the 60 frames per second option and the known issues.

## Reporting problems

Open an issue with where it happened, what you saw, and the newest folder from `sdmc:/switch/msr/switchlogs/`.
