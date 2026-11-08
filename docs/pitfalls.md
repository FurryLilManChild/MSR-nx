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
