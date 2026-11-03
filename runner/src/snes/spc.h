#ifndef SPC_H
#define SPC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct Spc Spc;

#include "apu.h"
#include "saveload.h"

struct Spc {
  Apu* apu;
  // registers
  uint8_t a;
  uint8_t x;
  uint8_t y;
  uint8_t sp;
  uint16_t pc;
  // flags
  bool c;
  bool z;
  bool v;
  bool n;
  bool i;
  bool h;
  bool p;
  bool b;
  // stopping
  bool stopped;
  // internal use
  uint8_t cyclesUsed; // indicates how many cycles an opcode used
  /* Access timing (saved with the remaining opcode clocks). The core runs an opcode
   * at its first clock; busIdx numbers its bus accesses so a port access can
   * be placed on its own cycle (apu.c: port lookahead and hold). */
  uint8_t busIdx;
  uint16_t opPc;        /* address of the running opcode */
  uint8_t op;
  uint8_t accessCycle;  /* cycle of the access in progress */
  bool dry;             /* probe run: no side effects, port accesses recorded */
  int8_t dryLastPort;   /* latest port ($F4-$F7) access cycle seen in a probe, -1 none */
  uint8_t dryWrites;    /* writes seen in a probe */
  int8_t dryPortWrite[4]; /* write ordinal of each port write in a probe */
  uint8_t dryPortWrites;
};

Spc* spc_init(Apu* apu);
void spc_free(Spc* spc);
void spc_reset(Spc* spc);
int spc_runOpcode(Spc* spc);
/* Dry-run the next opcode without side effects: the last cycle (from the
 * opcode's first) on which it reads or writes $F4-$F7, or -1 if it does not. */
int spc_probePortCycle(Spc* spc);
void spc_saveload(Spc *spc, SaveLoadInfo *sli);

#endif
