#include "beam_frame_driver.h"
#include "cpu_state.h"
#include "common_rtl.h"
#include "snes/saveload.h"
#include "snes/ppu.h"
#include <string.h>

typedef struct {
  uint32_t magic, size, resume_pc, wai_halted;
} BeamFrameState;
#define BEAM_STATE_MAGIC 0x31534642u /* BFS1 */

static bool beam_state_valid(const BeamFrameState *s) {
  return s->magic == BEAM_STATE_MAGIC && s->size == sizeof(*s) &&
      s->wai_halted <= 1 && s->resume_pc <= 0xffffffu &&
      (!s->resume_pc || cpu_pc24_resumable(s->resume_pc));
}

const char *snes_beam_frame_driver_state_identity(void) {
  return "beam-frame-state-v1";
}

bool snes_beam_frame_driver_state_preflight(const void *data, size_t size) {
  if (!data || size < 8) return false;
  uint32_t expected_version, version;
  size_t guest = RtlSnapshotGuestSize(&expected_version);
  size_t execution = RtlExecutionStateSize();
  size_t raster = ppu_rasterStateSize();
  size_t extra = sizeof(BeamFrameState) + execution + raster;
  memcpy(&version, (const uint8_t *)data + 4, sizeof version);
  if (!guest || guest > SIZE_MAX - extra || version != expected_version ||
      size != guest + extra) return false;
  BeamFrameState state;
  const uint8_t *chunk = (const uint8_t *)data + guest;
  memcpy(&state, chunk, sizeof state);
  return beam_state_valid(&state) &&
      RtlExecutionStateValid(chunk + sizeof state, execution) &&
      ppu_rasterStateValid(chunk + sizeof state + execution, raster);
}

void snes_beam_frame_driver_state_save(SaveLoadInfo *sli) {
  BeamFrameState state = {BEAM_STATE_MAGIC, sizeof state, 0, 0};
  bool wai;
  snes_beam_frame_driver_get_position(&state.resume_pc, &wai);
  state.wai_halted = wai;
  sli->func(sli, &state, sizeof state);
  RtlSaveExecutionState(sli);
  if (!ppu_rasterStateSave(sli)) RtlStateStreamFail(sli);
}

void snes_beam_frame_driver_state_load(SaveLoadInfo *sli, uint32_t version) {
  (void)version;
  BeamFrameState state;
  sli->func(sli, &state, sizeof state);
  if (!beam_state_valid(&state) || !RtlLoadExecutionState(sli) ||
      !ppu_rasterStateLoad(sli)) {
    RtlStateStreamFail(sli);
    return;
  }
  snes_beam_frame_driver_set_position(state.resume_pc, state.wai_halted != 0);
}

void snes_beam_frame_driver_state_loaded(uint32_t version) {
  (void)version;
  /* Restore the existing full CPU/bridge/APU/MMIO residue after the generic
   * loader's beam reconciliation. Do not maintain a partial second copy. */
  RtlApplyExecutionState();
}
