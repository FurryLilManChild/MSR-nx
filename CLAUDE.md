# MSR-nx

Native Nintendo Switch homebrew port of Metroid: Samus Returns (3DS, USA, title 00040000001BB200).

## Who does what

- Claude writes all code, scripts and docs. Mark (no coding) builds on his PC when asked, tests on
  his Switch and PC, and drops logs in `C:\MSR-nx\out\sd\switch\msr\switchlogs`.
- Instructions for Mark: numbered, click by click, copy-paste PowerShell.
- Switch NRO builds may be made in the cloud. PC builds run on Mark's PC. No cloud test runs without
  asking him first.
- Before every test build: present the plan, wait for his go-ahead, say plainly what the test is
  for and what he should see.

## Hard rules

- Never use em dashes anywhere (code comments, docs, commits, notes). No decorative emoji.
- Never commit game data or anything derived from it: dump, code.bin, exheader, RomFS, generated
  code, analysis output, shader packs. Run `git status` before every commit.
- Commit as `FurryLilManChild <346934328+FurryLilManChild@users.noreply.github.com>`.
- Upstream projects are information. We pick the design. Goal: game functions are real Switch
  functions with real calls and returns, game memory is plain memory, coarse event-driven timing,
  hardware only through a thin layer, system code replaced with native code.
- Renderer on the Switch: deko3d or Vulkan (NVK), never OpenGL without a stated reason Mark agreed to.
- Never ship a Switch build missing a core part (renderer, sound, input, game code).

## Layout (planned)

- `assets\` (Mark's PC only): the dump.
- `extract\` (PC only): code.bin, exheader.bin, romfs.bin.
- `analysis\` (PC only): function discovery output.
- `docs/progress-log.md`: newest first, one entry per session, with evidence.
- `docs/upstream.md`: upstream projects, pinned commits, licenses, what we take from each.
