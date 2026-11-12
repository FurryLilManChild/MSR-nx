# Pitfalls to avoid (MSR-nx session list, review with Mark before adding to the skills)

- Repeating an upstream's renderer (OpenGL) as our plan. Upstream choices are information; state our
  own choice (deko3d or Vulkan) with reasons.
- Saying "I have not confirmed X" when X can be checked in a minute (TriAevum's deko3d backend). Check.
- A `.gitignore` rule like `gen/` matches every folder of that name (it hid `tools/gen`). Anchor
  rules to the root (`/gen/`) and check `git status` lists new folders before committing.
- Trusting capstone's detail for ARM semantics: `update_flags` is set for adc/rsc without S, and
  `post_index` is missing for ldrd/strd. Read the S and P bits from the encoding.
- Emitting a function's instructions in address order without first jumping to its entry: 1,185
  functions had code reached by a branch below their entry.
- Looking only 4 instructions back for a jump table's bounds check; compilers slot other work in
  between. Also `lo` tables have n entries, `ls` tables n + 1.
- Stopping work when a tool is blocked instead of continuing with everything that does not need it.
- Calling a likely cause a root cause before checking it in the code (probe 6: "the worker threads
  set the lock up" was wrong; the real cause was a stand-in DSP reply). Trace the failing branch.
- Stand-in IPC replies that answer "success" with zeroed outputs: a zero output can be a failure
  flag the game checks much later (DSP LoadComponent's "loaded" byte).
- Thumb `blx` to a compiler switch helper is a jump table, not a call: the bytes after it are data.
- Capping instruction test samples per kind: the one untested operand shape (rotated ADR) broke 654
  address calculations. Test every distinct shape.
- Assuming a game is stuck when it is waiting for input it accepts in a different form (Samus Returns
  moves with the circle pad; the d-pad is for Aeion abilities).
- Calling a theory the cause in a status message before testing it (the "waits for a sound" theory
  was wrong; say "theory" and test it next).
- Shipping a Switch build with no speed meter or input log: test 1 could not say why A did nothing.
  Every test build reports pictures per second, time per platform job and the first button presses.
- Interpreting the game's PICA vertex shaders inside one GPU shader: uam gave it 255 registers and
  43 KB of spill per warp (deko3d's default queue scratch is 2 KB). Check the .dksh header (num_gprs,
  per_warp_scratch_sz) of every generated shader; translate per configuration instead.
- deko3d's OriginLowerLeft makes the swapchain flip the whole window vertically; 2D-engine blits are
  not flipped, so a picture blitted to the swapchain shows upside down.
- Designing a Switch test that needs file moves between launches (vsinterp/fsgeneral files), a new
  folder per test, and screenshots from the Album: all against Mark's standing preferences. Read the
  project preferences before designing any test. Tests cycle their settings inside one launch, save
  their own pictures and write each run into its own switchlogs folder.
- Building a test before Mark's go-ahead (test 3 and the first test 4).
- Saying "I'll work overnight" and ending the turn: nothing happens between messages. Keep the turn
  running (with status messages) or schedule a wake-up; never promise unattended work otherwise.
- uam replaces non-constant integer division with float math that can be off by one; GPU kernels use
  udiv() (float estimate, then corrected) or shifts.
- The GPU command buffer's memory was only reclaimed on a waiting flush; once test 6 removed the
  readbacks, nothing waited and the 8 MB filled up in a cutscene (deko3d RaiseError OutOfMemory).
  Command memory and the dynamic area now go around rings guarded by fences. Keep the msr.elf of
  every build sent to Mark: Atmosphere crash reports give offsets that addr2line turns into lines.
- Using bit 63 of a hash key as a flag (the early-depth variant) without clearing it in the hash:
  configurations whose hash had bit 63 set looked up the wrong variant or nothing, and the receipt
  called pack shaders "missing" (11 of 27 in test 8). Flags get their own bits; keys are masked.
- Dynamically indexed arrays in a fragment shader (texc[i], vals[i]) go to local memory. The general
  fragment shader was the only one with local memory (2048 bytes per warp) and the only one seen
  drawing white on the Switch; the CPU replay of the same frame was correct. Check per_warp_scratch_sz
  of every fragment shader too, and keep it at 0.
- Taking a correlation (local memory only in the general shader, white-out only where it drew) as
  the likely cause: the on-device self-test showed the rewritten shader just as wrong. Put the check
  on the hardware first when a GPU-only fault cannot be reproduced on the PC.
- libnx's console font rows are little-endian u16 with bit 15 as the LEFT pixel. Check glyph
  orientation with an asymmetric letter (g, F), never with A (test 11 drew mirrored text).
- Scissor is queue state the game's draws leave behind: set it before clearing the output in
  present(), or the clear misses part of the frame (test 11: the closed menu stayed visible).
- A "fresh" id made as mix(x, x + 1) with a multiply-xorshift mix repeats: x ^ (x + 1) is 1 for
  every even x, so every fresh token came out the same. Use a counter: mix(seed, ++n).
- Before building a GPU saving, count what already runs: right-eye reuse was built for work the
  frame plan already skipped (the right eye ran 3.4% of the left eye's draws).
- The frame plan matched operations by position: one extra object early in a frame made every later
  operation "different" and run. Match by order with a look-ahead instead.
- The probe's input replay varies less from frame to frame than real play: the right eye ran 3% of
  the left's draws in the probe and up to 19% on the Switch. Check skip counts on the Switch too.
- Page versions are too coarse for "is this framebuffer unchanged": the top framebuffer ends in the
  same 4 KB page where the left eye's render target begins, which is cleared every frame, so test
  18 showed the scaler's picture for only about 1 picture in 5 (the rest were the shrunk native
  framebuffer: blocky at 1.5x, merely smoothed at 2x). Track exact byte ranges for that.
- A resolution scaler that only renders the eye targets larger shows nothing in this game: its
  picture passes through GX texture copies (VRAM, native size) and full-screen redraws several
  times per frame. The copies need large-size counterparts too.
- Compute kernels that pass vram_ops_test on a PC can still differ on the Switch (k_texup in test
  23). Each new kernel gets a start-up check on the Switch against the same code on its CPU
  (dk_pica.c dkr_scaler_selftest, vram_ref.cpp).
- 0.5.0's log clean-up deleted run(N-10) and new runs took the lowest free number, so after the
  first clean-up new logs went into freed low numbers and looked missing. Number from the highest
  existing run. And when a log is missing, the build is the suspect, not how the tester ran it.

- Function pointers made from the pc (`ldr rX, =offset; add rX, pc`) are invisible to call and
  pointer scans. The generator now finds Thumb ones itself; a missed one stops the game with
  "unknown call target" only when that path first runs (issue 1: snprintf's character writer).
- Callbacks reached only through stored pointers, especially ones starting with `mov r0, r0`, are
  missed by prologue-based function scans. The generator sweeps for uncovered pointer targets and
  for code right after each function (issue 1, second report).
