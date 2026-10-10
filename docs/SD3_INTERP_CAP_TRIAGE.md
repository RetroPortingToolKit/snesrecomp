# Seiken Densetsu 3 cap investigation (#80)

This is a diagnostic preparation, not a freeze fix. The issue demonstrates a
cap/freeze but does not establish that the interpreter changes the stack pointer
incorrectly. Its custom frame code fast-forwards the guest clock and changes IRQ
delivery, and the supplied reports do not identify an exact engine commit.

Keep the ROM private. Record the engine and title commit hashes, cfgs, host frame
implementation and input route for each run. Use a separate profile/save copy.
Build with `-DSNESRECOMP_ENABLE_TRACE=ON` and retain the complete existing cap,
instruction-ring and run-report output. Record the entry/resume PC and S/D/P at
the host frame boundaries. Observe running execution; do not pause or step it.

Summarize the existing logs without changing execution:

```sh
python tools/summarize_interp_caps.py sd3_trace.log \
  --engine-revision ENGINE_SHA --game-revision GAME_SHA --output caps.json
```

The summary preserves register values and counts, identifies zero entries as an
observation, and reports unparseable cap lines. Zero entry alone is not proof of
an invalid CPU address; the caller's reset/resume contract must explain it.

Compare the stock current host separately from the modified host, with the
manual `g_cpu.master_cycles = frame_end` fast-forward removed. Then compare
the same route with `SNESRECOMP_LLE_BOUNCE=0` to isolate mixed-tier transfers.
Do not raise the instruction cap, invent a yield at 1,300,000 steps, or remove
raster IRQs as a proposed framework fix: those alter the behavior under test.

Issue #109 has an independently reproduced nested deadline-unwind defect. Its
fix is a candidate to test on this route, not evidence that it caused this
particular freeze. Keep #80 open until an exact current reproduction and a
baseline/candidate comparison establish causation.
