#ifndef SNES_DSP1_H
#define SNES_DSP1_H

#include <stdint.h>

/* Standalone builds retain the instruction-level floor by default. CMake
 * resolves SNESRECOMP_DSP1_IMPL once and supplies this numeric definition. */
#ifndef SNESRECOMP_DSP1_HLE
#define SNESRECOMP_DSP1_HLE 0
#endif
#if SNESRECOMP_DSP1_HLE != 0 && SNESRECOMP_DSP1_HLE != 1
#error "SNESRECOMP_DSP1_HLE must be 0 (LLE) or 1 (HLE)"
#endif

struct SaveLoadInfo;

typedef struct Dsp1 Dsp1;

Dsp1 *dsp1_create(void);
void dsp1_destroy(Dsp1 *d);
void dsp1_reset(Dsp1 *d);
void dsp1_sync(Dsp1 *d, uint64_t master_clock);

uint8_t dsp1_read(Dsp1 *d, uint16_t addr);
void dsp1_write(Dsp1 *d, uint16_t addr, uint8_t value);

uint8_t dsp1_read_data_ram(Dsp1 *d, uint16_t addr);
void dsp1_write_data_ram(Dsp1 *d, uint16_t addr, uint8_t value);

int dsp1_load_firmware(Dsp1 *d, const char *rom_path);
/* Identifies the fixed build choice, independent of firmware availability.
 * dsp1_load_firmware returns firmware-loaded, not backend-ready; an HLE build
 * ignores firmware inputs and therefore returns zero, as before. */
const char *dsp1_build_implementation(void);
int dsp1_firmware_loaded(const Dsp1 *d);
int dsp1_hle_active(const Dsp1 *d);
int dsp1_hle_failed(const Dsp1 *d);
uint8_t dsp1_hle_failed_command(const Dsp1 *d);
uint64_t dsp1_instructions_executed(const Dsp1 *d);
uint64_t dsp1_host_reads(const Dsp1 *d);
uint64_t dsp1_host_writes(const Dsp1 *d);
uint64_t dsp1_command_count(const Dsp1 *d, uint8_t command);

void dsp1_saveload(Dsp1 *d, struct SaveLoadInfo *sli);

#endif /* SNES_DSP1_H */
