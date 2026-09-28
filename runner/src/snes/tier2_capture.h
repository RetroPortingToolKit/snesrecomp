#ifndef SNESRECOMP_TIER2_CAPTURE_H
#define SNESRECOMP_TIER2_CAPTURE_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Overrides are tri-state: -1 means absent. Visibility never enables capture. */
void tier2_capture_configure(int exposed, int config_enabled, int launch_enabled);
void tier2_capture_set_selection(int enabled);
int tier2_capture_exposed(void);
const char *tier2_capture_setting_source(void);
extern int g_tier2_capture_active; /* cached hot-loop guard; no getenv per opcode */
void tier2_capture_set_identity(const char *rom_sha256, const char *module_id,
                               const char *program_digest, const char *mapper);
void tier2_capture_write_header(FILE *f, const char *title, int journal);
FILE *tier2_capture_journal(const char *title);
void tier2_capture_flush(void);
int tier2_capture_replace(const char *temp, const char *path);
uint64_t tier2_capture_next_sequence(void);
void tier2_capture_instruction(uint32_t pc, uint8_t mx, uint8_t emulation,
                              unsigned guest_cycles);
void tier2_capture_write_costs(FILE *f);
/* Standalone interpreter hosts without the AOT bridge: costs only, no
 * implied function boundaries or promotable transfers. */
int tier2_capture_write_cost_checkpoint(const char *title);
/* processor: 0 = host 65816, 1 = SA-1 (costs only, separate execution ABI). */
void tier2_capture_cpu_instruction(uint8_t processor, uint32_t pc, uint8_t mx,
                                  uint8_t emulation, unsigned guest_cycles);
uint64_t tier2_capture_dropped_costs(void);
int tier2_capture_journal_failed(void);
void tier2_capture_set_build_digest(const char *digest);
void tier2_capture_set_entry_probe(const char *(*probe)(uint32_t, uint8_t));
/* Seal a session's last observations before an enabled -> disabled change. */
void tier2_capture_set_checkpoint_hook(void (*hook)(void));
const char *tier2_capture_entry_reason(uint32_t pc, uint8_t mx);

const char *tier2_capture_manifest_path(const char *rom_title);
const char *tier2_capture_journal_path(const char *rom_title);
void tier2_capture_set_default_enabled(int enabled);
int tier2_capture_enabled(void);
int tier2_capture_has_identity(void);

/* Compatibility observation API. Buffered writes are flushed by the frame hook. */
int tier2_capture_append_discovery(const char *rom_title,
                                   uint32_t site_pc24,
                                   uint32_t target_pc24,
                                   const char *entry_mx,
                                   const char *site_kind,
                                   int outcome,
                                   int32_t frame);

void tier2_capture_close(void);

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_TIER2_CAPTURE_H */
