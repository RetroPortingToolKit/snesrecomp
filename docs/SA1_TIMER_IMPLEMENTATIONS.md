# SA-1 timer advancement

`SNESRECOMP_SA1_TIMER_IMPL=REFERENCE` (the default) advances one master clock
at a time. `NATIVE` computes counter wraps and the first comparator match
arithmetically. Select the implementation at CMake configure time; there is
no runtime switch. Both implementations use portable C and the same state.

The native algorithm preserves the full H/V counters, increment-before-compare,
sticky timer IRQ independent of IRQ delivery enable, and `$2302` H/4 and V
latches. Raster and linear mode writes do not normalize counters; the next
increment/wrap handles out-of-period values exactly as the reference does.
Instruction boundaries, reset/ready synchronization, MMIO ordering, snapshots
and interrupt delivery remain unchanged. No title addresses or guest jobs
are recognized.

The focused `tests/sa1/timer_span_test.c` compares the production algorithms
over directed counter/target boundaries, disabled comparators, pending IRQ,
multiple wraps, randomized spans, and sequences of MMIO mode/reset/target
writes. The ordinary SA-1 core tests cover the surrounding execution service.
Build each implementation separately when qualifying a consumer. The selector
and source/binary identities belong in its recorded qualification settings;
this option does not establish cross-build netplay compatibility.
