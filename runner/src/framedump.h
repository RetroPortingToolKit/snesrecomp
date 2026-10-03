#pragma once
#include <stdint.h>

// Per-frame WRAM dumper (game-agnostic).
// Output layout:
//   <dir>/frame_NNNNNN_wram.bin  — 128 KB recomp WRAM
//   <dir>/frame_NNNNNN.json      — frame + wram_size + crc32
// Per-game decoders should read the .bin for game-specific fields.

typedef void (*FrameDumpCallback)(uint32_t frame, const uint8_t *wram);

extern FrameDumpCallback g_framedump_callback;

void FrameDump_Init(const char *dir);
/* Optional final host framebuffer, including overlays and enhancements.
 * Enabled by SNESRECOMP_FRAMEDUMP_PIXELS=1; uses the same frame interval. */
/* With PIXELS enabled, SNESRECOMP_FRAMEDUMP_VIDEO=1 also captures VRAM,
 * CGRAM and PPU mode at the completed presentation boundary. */
void FrameDump_Present(uint32_t frame, const uint8_t *bgra, uint32_t pitch,
                       uint32_t width, uint32_t height);

/* Explicit host opt-in for PPU resources; no dependency on runner globals. */
struct Ppu;
void FrameDump_Ppu(uint32_t frame, const struct Ppu *ppu);
