/*
 * Nintendo SA-1 coprocessor.
 *
 * The CPU is the runner's existing MIT-licensed interp816 core. Cartridge
 * mapping, MMIO, DMA, arithmetic, bitmap and synchronization behavior are
 * adapted from ares (ISC), Copyright (c) 2004-2025 ares team, Near et al.
 */
#ifndef SNESRECOMP_SA1_H
#define SNESRECOMP_SA1_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "saveload.h"

typedef struct Sa1 Sa1;

/* Optional host presentation observation. Called immediately before a matched
 * instruction, never for idle WAI/STP cycles or interrupt entry. Memory views
 * are borrowed, read-only, and valid only during the call. Observers must not
 * execute guest code or re-enter the chip. Host configuration survives reset
 * and state load, and is deliberately excluded from serialized guest state. */
typedef struct Sa1Observation {
  uint32_t pc;
  uint64_t master_clock;
  const uint8_t *iram, *bwram;
  size_t iram_size, bwram_size;
  uint16_t x;
} Sa1Observation;
typedef void (*Sa1Observer)(void *context, const Sa1Observation *observation);
bool sa1_set_observer(Sa1 *sa1, const uint32_t *addresses, size_t count,
                     Sa1Observer observer, void *context);

Sa1 *sa1_create(uint8_t *rom, uint32_t rom_size,
                uint8_t *bwram, uint32_t bwram_size);
void sa1_destroy(Sa1 *sa1);
void sa1_reset(Sa1 *sa1);
/* The H/V timer counts the console's scanlines: 262 on NTSC (the default),
 * 312 on PAL. */
void sa1_set_pal_timing(Sa1 *sa1, bool pal);
void sa1_set_cpu_bus_address(Sa1 *sa1, uint32_t address);
void sa1_sync(Sa1 *sa1, uint64_t master_clock);
void sa1_saveload(Sa1 *sa1, SaveLoadInfo *sli);

uint8_t sa1_cpu_read(Sa1 *sa1, uint8_t bank, uint16_t address,
                     uint8_t open_bus);
void sa1_cpu_write(Sa1 *sa1, uint8_t bank, uint16_t address, uint8_t data);

/* Stable host pointer for pointer-oriented generated helpers. Returns NULL
 * for MMIO and switched interrupt-vector bytes. */
uint8_t *sa1_cpu_memory_ptr(Sa1 *sa1, uint8_t bank, uint16_t address);

bool sa1_cpu_irq_pending(const Sa1 *sa1);
uint64_t sa1_instructions_executed(const Sa1 *sa1);
uint64_t sa1_master_clock(const Sa1 *sa1);

#endif
