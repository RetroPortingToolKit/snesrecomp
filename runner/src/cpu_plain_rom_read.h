#pragma once

#include "cpu_state.h"

/* Claim only immutable plain LoROM/HiROM high-window ROM. A successful read
 * updates the same CPU/open-bus and cartridge address latches as cpu_read8,
 * while exposing that it is stable to the interpreter's poll detector.
 * False has no effects: use cpu_read8 for devices, SRAM and invalid pointers.
 * No pointer or derived mapping survives this call. */
bool cpu_read8_plain_rom(CpuState *cpu, uint8 bank, uint16 addr, uint8 *out);
