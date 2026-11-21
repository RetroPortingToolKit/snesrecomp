#ifndef SNESRECOMP_LAST_CYCLE_H
#define SNESRECOMP_LAST_CYCLE_H
#include <stdint.h>

/* Master clocks of the last bus cycle of the instruction at pc24, which ran
 * and left the CPU at next_pc24 (a conditional branch: taken or not). */
unsigned snes_last_cycle_clocks(uint32_t pc24, uint32_t next_pc24);

#endif
