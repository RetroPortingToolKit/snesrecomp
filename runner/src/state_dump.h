#ifndef SNESRECOMP_STATE_DUMP_H
#define SNESRECOMP_STATE_DUMP_H

#include <stdint.h>

/* Scene state dump — the recomp half of the snesref `dump <tag>` contract
 * (tools/snesref/README.md). Writes, into `dir`:
 *
 *   <tag>.fb.bmp / <tag>.fb.bgrx   the frame just rasterized (256 x h BGRX;
 *                                  the authentic columns only, so a widescreen
 *                                  margin never shifts the comparison)
 *   <tag>.wram.bin                 131072 bytes
 *   <tag>.vram.bin                 65536 bytes (little-endian words)
 *   <tag>.cgram.bin                512 bytes
 *   <tag>.oam.bin                  544 bytes (low table + high table)
 *   <tag>.sram.bin                 cartridge RAM, whatever size the cart has
 *   <tag>.regs.json                PPU / HDMA register state
 *   <tag>.ppuw.tsv                 the frame's PPU register write journal
 *   <tag>.apu.tsv                  CPU<->SPC port traffic still in the always-on
 *                                  port ring (audio_trace.h)
 *   <tag>.info.json                frame, sizes, layer mask
 *
 * Observer only: reads guest state, never changes it. Returns 0 on success.
 * `pixels` may be NULL (no framebuffer files are written). */
int snes_state_dump(const char *dir, const char *tag, const uint8_t *pixels,
                    int pitch_bytes, int x0, int height, uint32_t frame);

#endif
