/* Scene state dump. See state_dump.h. */
#include "state_dump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_rtl.h"
#include "cpu_state.h"
#include "ppu_dma_trace.h"
#include "snes/cart.h"
#include "snes/dma.h"
#include "snes/ppu.h"
#include "snes/snes.h"
#include "snes/superfx.h"
#include "audio_trace.h"

extern uint8_t g_ram[0x20000];
extern Ppu *g_ppu;
extern Snes *g_snes;

static FILE *open_out(const char *dir, const char *tag, const char *suffix,
                      const char *mode) {
  char path[1024];
  snprintf(path, sizeof(path), "%s%s%s%s", dir && dir[0] ? dir : "",
           dir && dir[0] ? "/" : "", tag, suffix);
  FILE *f = fopen(path, mode);
  if (!f) fprintf(stderr, "state_dump: cannot open '%s'\n", path);
  return f;
}

static int write_blob(const char *dir, const char *tag, const char *suffix,
                      const void *data, size_t size) {
  FILE *f = open_out(dir, tag, suffix, "wb");
  if (!f) return -1;
  size_t n = fwrite(data, 1, size, f);
  fclose(f);
  return n == size ? 0 : -1;
}

static void put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t *p, uint32_t v) { put_le16(p, (uint16_t)v); put_le16(p + 2, (uint16_t)(v >> 16)); }

static int write_fb(const char *dir, const char *tag, const uint8_t *pixels,
                    int pitch, int x0, int h) {
  const int w = 256;
  uint8_t *raw = (uint8_t *)malloc((size_t)w * h * 4);
  if (!raw) return -1;
  for (int y = 0; y < h; y++)
    memcpy(raw + (size_t)y * w * 4, pixels + (size_t)y * pitch + (size_t)x0 * 4,
           (size_t)w * 4);
  int rc = write_blob(dir, tag, ".fb.bgrx", raw, (size_t)w * h * 4);

  FILE *f = open_out(dir, tag, ".fb.bmp", "wb");
  if (f) {
    const int row = w * 3;
    uint8_t hdr[54] = { 'B', 'M' };
    put_le32(hdr + 2, 54u + (uint32_t)(row * h));
    put_le32(hdr + 10, 54);
    put_le32(hdr + 14, 40);
    put_le32(hdr + 18, (uint32_t)w);
    put_le32(hdr + 22, (uint32_t)h);
    put_le16(hdr + 26, 1);
    put_le16(hdr + 28, 24);
    put_le32(hdr + 34, (uint32_t)(row * h));
    fwrite(hdr, 1, sizeof(hdr), f);
    uint8_t *line = (uint8_t *)malloc((size_t)row);
    for (int y = h - 1; y >= 0 && line; y--) {
      const uint8_t *s = raw + (size_t)y * w * 4;
      for (int x = 0; x < w; x++) {
        line[x * 3 + 0] = s[x * 4 + 0];
        line[x * 3 + 1] = s[x * 4 + 1];
        line[x * 3 + 2] = s[x * 4 + 2];
      }
      fwrite(line, 1, (size_t)row, f);
    }
    free(line);
    fclose(f);
  } else {
    rc = -1;
  }
  free(raw);
  return rc;
}

static void write_regs(const char *dir, const char *tag, uint32_t frame) {
  FILE *f = open_out(dir, tag, ".regs.json", "w");
  if (!f || !g_ppu) { if (f) fclose(f); return; }
  const Ppu *p = g_ppu;
  fprintf(f, "{\n  \"frame\": %u,\n", frame);
  fprintf(f, "  \"inidisp\": %u, \"obsel\": %u, \"bgmode\": %u, \"mosaic\": %u,\n",
          p->inidisp, p->obsel, p->bgmode, p->mosaic);
  fprintf(f, "  \"bgsc\": [%u, %u, %u, %u], \"bg_tile_adr\": %u,\n",
          p->bgXsc[0], p->bgXsc[1], p->bgXsc[2], p->bgXsc[3], p->bgTileAdr);
  fprintf(f, "  \"hscroll\": [%u, %u, %u, %u], \"vscroll\": [%u, %u, %u, %u],\n",
          p->hScroll[0], p->hScroll[1], p->hScroll[2], p->hScroll[3],
          p->vScroll[0], p->vScroll[1], p->vScroll[2], p->vScroll[3]);
  fprintf(f, "  \"m7sel\": %u, \"setini\": %u, \"m7matrix\": [%d, %d, %d, %d, %d, %d, %d, %d],\n",
          p->m7sel, p->setini, p->m7matrix[0], p->m7matrix[1], p->m7matrix[2],
          p->m7matrix[3], p->m7matrix[4], p->m7matrix[5], p->m7matrix[6],
          p->m7matrix[7]);
  fprintf(f, "  \"fixed_color\": %u, \"windowsel\": %u, \"wbgobjlog\": %u,\n",
          p->fixedColor, (unsigned)p->windowsel, p->wbgobjlog);
  fprintf(f, "  \"window\": [%u, %u, %u, %u],\n", p->window1left,
          p->window1right, p->window2left, p->window2right);
  fprintf(f, "  \"tm\": %u, \"ts\": %u, \"tmw\": %u, \"tsw\": %u,\n",
          p->screenEnabled[0], p->screenEnabled[1], p->screenWindowed[0],
          p->screenWindowed[1]);
  fprintf(f, "  \"cgwsel\": %u, \"cgadsub\": %u,\n", p->cgwsel, p->cgadsub);
  fprintf(f, "  \"hdmaen_last\": %u,\n  \"dma\": [\n", g_snesrecomp_last_hdmaen);
  for (int i = 0; i < 8; i++) {
    const DmaChannel *c = g_snes && g_snes->dma ? &g_snes->dma->channel[i] : NULL;
    if (!c) break;
    fprintf(f, "    {\"ch\": %d, \"mode\": %u, \"indirect\": %d, \"fromB\": %d, "
               "\"bAdr\": %u, \"aBank\": %u, \"aAdr\": %u, \"indBank\": %u, "
               "\"size\": %u, \"hdmaActive\": %d}%s\n",
            i, c->mode, c->indirect, c->fromB, c->bAdr, c->aBank, c->aAdr,
            c->indBank, c->size, c->hdmaActive, i < 7 ? "," : "");
  }
  fprintf(f, "  ]\n}\n");
  fclose(f);
}

static void write_wlog(const char *dir, const char *tag, uint32_t frame) {
  enum { kCap = 1 << 16 };
  PpuWlogEntry *e = (PpuWlogEntry *)malloc(sizeof(PpuWlogEntry) * kCap);
  if (!e) return;
  int lost = 0;
  int n = ppu_wlog_collect(frame, e, kCap, &lost);
  FILE *f = open_out(dir, tag, ".ppuw.tsv", "w");
  if (f) {
    static const char *const src[] = { "cpu", "dma", "hdma", "replay" };
    fprintf(f, "# frame\tline\taddr\tvalue\tsource%s\n",
            lost ? "\t(ring evicted part of this frame)" : "");
    for (int i = 0; i < n; i++)
      fprintf(f, "%u\t%d\t%04X\t%02X\t%s\n", e[i].frame, e[i].line,
              e[i].reg, e[i].val, e[i].src < 4 ? src[e[i].src] : "?");
    fclose(f);
  }
  free(e);
}

static void write_dmas(const char *dir, const char *tag, uint32_t frame) {
  enum { kCap = 4096 };
  PpuDmaInfo *e = (PpuDmaInfo *)malloc(sizeof(PpuDmaInfo) * kCap);
  if (!e) return;
  int lost = 0;
  int n = ppudma_dma_collect(frame, e, kCap, &lost);
  FILE *f = open_out(dir, tag, ".dma.tsv", "w");
  if (f) {
    fprintf(f, "# frame\tline\tphase\tchannel\tdir\tsource\tbreg\tdest\tsize%s\n",
            lost ? "\t(ring evicted part of this frame)" : "");
    for (int i = 0; i < n; i++)
      fprintf(f, "%d\t%d\t%s\t%u\t%s\t%02X:%04X\t21%02X\t%04X\t%u\n",
              e[i].frame, e[i].line, e[i].phase ? "raster" : "cpu",
              (unsigned)e[i].channel, e[i].fromB ? "B2A" : "A2B",
              (unsigned)e[i].aBank, (unsigned)e[i].aAdr, (unsigned)e[i].bAdr,
              (unsigned)e[i].dest, (unsigned)e[i].size);
    fclose(f);
  }
  free(e);
}

int snes_state_dump(const char *dir, const char *tag, const uint8_t *pixels,
                    int pitch_bytes, int x0, int height, uint32_t frame) {
  int rc = 0;
  if (pixels && height > 0)
    rc |= write_fb(dir, tag, pixels, pitch_bytes, x0, height);
  rc |= write_blob(dir, tag, ".wram.bin", g_ram, sizeof(g_ram));
  if (g_ppu) {
    uint8_t buf[0x10000];
    for (int i = 0; i < 0x8000; i++) put_le16(buf + i * 2, g_ppu->vram[i]);
    rc |= write_blob(dir, tag, ".vram.bin", buf, 0x10000);
    for (int i = 0; i < 0x100; i++) put_le16(buf + i * 2, g_ppu->cgram[i]);
    rc |= write_blob(dir, tag, ".cgram.bin", buf, 0x200);
    for (int i = 0; i < 0x100; i++) put_le16(buf + i * 2, g_ppu->oam[i]);
    memcpy(buf + 0x200, g_ppu->highOam, 0x20);
    rc |= write_blob(dir, tag, ".oam.bin", buf, 0x220);
  }
  if (g_snes && g_snes->cart && g_snes->cart->ram && g_snes->cart->ramSize)
    rc |= write_blob(dir, tag, ".sram.bin", g_snes->cart->ram,
                     g_snes->cart->ramSize);
  write_regs(dir, tag, frame);
  write_wlog(dir, tag, frame);
  write_dmas(dir, tag, frame);
  if (g_snes && g_snes->cart && g_snes->cart->superfx) {
    static SuperFxJob jobs[4096];
    int n = superfx_job_log(jobs, 4096);
    FILE *f = open_out(dir, tag, ".gsu.tsv", "w");
    if (f) {
      fprintf(f, "# start_master\tstop_master\tclocks\tpc24\n");
      for (int i = 0; i < n; i++)
        fprintf(f, "%llu\t%llu\t%lld\t%06X\n",
                (unsigned long long)jobs[i].start_master,
                (unsigned long long)jobs[i].stop_master,
                jobs[i].stop_master ? (long long)(jobs[i].stop_master - jobs[i].start_master) : -1LL,
                (unsigned)jobs[i].pc24);
      fclose(f);
    }
  }

  {
    /* Everything the always-on port ring still holds: the recomp half of
     * tools/mesen_scene's apu.tsv. */
    static const char *const kName[] = {
      "?", "?", "?", "?", "cpu_wr", "spc_rd", "spc_wr", "cpu_rd", "cpu_ap",
    };
    static AudioTracePortEvent ev[AUDIO_TRACE_PORT_RING];
    uint64_t oldest = 0, total = 0;
    uint32_t n = audio_trace_copy_port_events(0, AUDIO_TRACE_PORT_RING, ev,
                                              &oldest, &total);
    FILE *af = open_out(dir, tag, ".apu.tsv", "w");
    if (af) {
      fprintf(af, "# frame\tscanline\thclock\tevent\tport\tvalue\tsample\n");
      for (uint32_t i = 0; i < n; i++)
        fprintf(af, "%u\t%u\t%u\t%s\t%u\t%02X\t%llu\n", ev[i].frame,
                ev[i].vpos, ev[i].hpos,
                ev[i].type < sizeof(kName) / sizeof(kName[0]) ? kName[ev[i].type] : "?",
                ev[i].port, ev[i].val, (unsigned long long)ev[i].sample_idx);
      fclose(af);
    }
  }

  FILE *f = open_out(dir, tag, ".info.json", "w");
  if (f) {
    fprintf(f, "{\"frame\": %u, \"fb_width\": 256, \"fb_height\": %d, "
               "\"pixel_format\": \"BGRX8888\", \"source\": \"snesrecomp\", "
               "\"layer_mask\": %u, \"sram_size\": %u, "
               "\"master_clock\": %llu, \"vpos\": %u, \"hpos\": %u}\n",
            frame, pixels ? height : 0, g_snes_ppu_dbg_layer_mask,
            g_snes && g_snes->cart ? g_snes->cart->ramSize : 0,
            (unsigned long long)g_cpu.master_cycles,
            g_snes ? (unsigned)g_snes->vPos : 0u,
            g_snes ? (unsigned)g_snes->hPos : 0u);
    fclose(f);
  }
  return rc;
}
