# Super FX halted clocks

`SNESRECOMP_SUPERFX_IDLE_IMPL=LLE` (default) keeps the original six-clock
`run_one` loop. `NATIVE` drains stopped, empty quanta arithmetically. Both
implementations use portable C, selected when building; there is no runtime
switch or host instruction-set requirement.

With G clear, `run_one` only advances buffered ROM/RAM operations and clock
credit. The native path retains that scalar operation until both buffers and
any pending presentation/widescreen completion dispatch have retired. It then
subtracts complete six-clock quanta, counting them against the original
2,000,000-iteration guard and retaining the same remainder or guard-limited
debt. It changes no instruction, memory mapping, STOP/IRQ acknowledgement,
pipeline, snapshot field or state format. Replay completion callbacks run at
the original boundary and may start another task before draining continues.

`python tests/superfx/run_idle_span.py --cc <C11-compiler>` checks both
implementations against the original sync loop. Directed and seeded cases
cover buffered reads/writes, clock residues and debt, the guard limit,
backward master time, IRQ acknowledgement and callbacks restarting execution.
The existing enhancement and PC-hook checks also apply.

This is an exact host-cost optimization. It adds no CPU capability or network
protocol; the selected build remains fixed. Current-build replay and actual
consumer qualification are still required. Agreement of these two algorithms
does not establish independent hardware accuracy for the shared Super FX model.
