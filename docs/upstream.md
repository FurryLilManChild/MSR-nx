# Upstream projects (reference material)

Pinned to the commits read on 2026-11-07. None of these dictate our design.

| Project | Commit | License | What it gives us |
|---|---|---|---|
| gminustwo/msr2ds | 0d96ccf (2026-11-06) | GPL-2.0 (Azahar fork; check "or later" per file before reuse) | Samus Returns function discovery (`tools/msr/recomp/analyze.py`, `extra_entries.txt`), hook locations for HUD, beam cycling, morph ball, amiibo unlocks, frame limit (`patchable.txt`). Its runtime is a dispatcher model on Azahar: not our design. |
| JohnUzoka/TriAevum-Switch | 3546da8 (2026-11-05) | GPL-3.0-or-later | 3DS ARM11 AOT generator and Switch runtime for OoT3D; deko3d renderer (`docs/DEKO3D_PROGRESS.md`): precompiled shader package plus parametric uber fallback; Horizon threading, audio, pacing lessons (`docs/PORTING_GUIDE_3DS_SWITCH.md`). |
| GustavoHenriqueRocha/TriAevum-Switch | 62a6ac4 (2026-10-24) | GPL-3.0-or-later | The Portuguese original of the above. |
| fearkov/3dsrecomp | df82e6e (2026-11-04) | MIT (Cargo.toml; repo has no LICENSE file) | Generic 3DS function discovery and ARM/Thumb/VFP to C. |
| fearkov/zakuro | c856685 (2026-11-05) | MIT | 3DS emulator built to run recompiled code: HLE services, PICA, audio. |

## Notes

- Mixing GPL-3 (TriAevum) code into this repo makes the port GPL-3. msr2ds pieces are only usable
  if their files are GPL-2.0-or-later. Decide the repo license before the first borrowed file.
