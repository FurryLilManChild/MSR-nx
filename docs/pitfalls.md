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
