#include "snes/hd_jobs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECON_CHECK(e) do { if (!(e)) { fprintf(stderr, "reconstruction check failed at %d: %s\n", __LINE__, #e); exit(1); } } while (0)

static SnesHdFrame *projection_fixture_sampling(unsigned flags, bool quantized, bool split, unsigned samples) {
  SnesHdFrame *f = SnesHdFrameCreate(288, 64);
  static uint16_t vram[32768];
  for (unsigned i = 0; i < 32768; ++i) vram[i] = (uint16_t)(((i * 37 % 255 + 1) << 8) | (i * 19 & 255));
  int source = SnesHdFrameAddPicture(f, vram); RECON_CHECK(source >= 0);
  for (unsigned y = 0; y < 64; ++y) {
    SnesHdLine j = {0};
    j.kind = kSnesHdReconstructed; j.raster_line = y + 1;
    j.source = source; j.active_left = j.screen_left = -16; j.active_right = 272;
    j.brightness = 15; j.control = flags & 3; j.composition = flags >> 2;
    j.matrix[0] = 16384.0 / (y + 21); j.matrix[1] = -8192.0 / (y + 21);
    j.matrix[2] = 0; j.matrix[3] = 4096.0 / (y + 21);
    if (quantized) for (unsigned c = 0; c < 4; ++c) j.matrix[c] = round(j.matrix[c]);
    j.matrix[4] = 128; j.matrix[5] = 192; j.matrix[6] = 12; j.matrix[7] = -20;
    if (split && y >= 32) j.camera = 1;
    for (unsigned x = 0; x < 288; ++x) {
      j.visibility[x] = x % 4 == 0 ? 96 : x & 1 ? 127 : 123;
      j.native[x] = 0x123456;
      if (x % 3 == 0) { j.object[x] = 0xa401; j.object_color[x] = 0x3e0; }
    }
    for (unsigned c = 0; c < 256; ++c) j.palette[c] = (uint16_t)(c * 127 & 32767);
    RECON_CHECK(SnesHdFrameSetLine(f, y, &j));
  }
  RECON_CHECK(SnesHdFrameSetSampling(f,samples));
  RECON_CHECK(SnesHdFrameSeal(f)); return f;
}
static SnesHdFrame *projection_fixture(unsigned flags, bool quantized, bool split) {
  return projection_fixture_sampling(flags,quantized,split,1);
}
static void test_projection_geometry(void) {
  SnesHdFrame *f = projection_fixture(0, false, false);
  SnesHdView v; RECON_CHECK(SnesHdFrameView(f, &v));
  RECON_CHECK(v.lines[20].perspective);
  for (unsigned row = 0; row < 64; ++row) for (unsigned sub = 0; sub < 8; ++sub) {
    double t[4], y = row + 1 + sub / 8.0 - .5;
    RECON_CHECK(SnesHdFrameTransform(f, row, sub / 8.0, t));
    double a = 16384 / (y + 20), b = -8192 / (y + 20), d = 4096 / (y + 20);
    RECON_CHECK(fabs(t[2] - a) < 1e-9 && t[3] == 0);
    RECON_CHECK(fabs(t[0] - (128*256 + a*(-116) + b*(y-212) - .5*a)) < 1e-8);
    RECON_CHECK(fabs(t[1] - (192*256 + d*(y-212))) < 1e-8);
  }
  double t[4]; RECON_CHECK(!SnesHdFrameTransform(f, 0, NAN, t));
  SnesHdFrameDestroy(f);
  f = projection_fixture(0, true, true); RECON_CHECK(SnesHdFrameView(f, &v));
  RECON_CHECK(v.lines[16].perspective && v.lines[48].perspective);
  SnesHdFrameDestroy(f);
}
static void test_nonperspective(void) {
  SnesHdFrame *f = SnesHdFrameCreate(1, 16);
  static uint16_t vram[32768]; RECON_CHECK(SnesHdFrameAddPicture(f, vram) == 0);
  for (unsigned y = 0; y < 16; ++y) {
    SnesHdLine j = {0}; j.kind = kSnesHdReconstructed;
    j.raster_line = y+1; j.active_right = 1; j.matrix[0] = y & 1 ? -256 : 256;
    j.matrix[3] = 256; RECON_CHECK(SnesHdFrameSetLine(f, y, &j));
  }
  RECON_CHECK(SnesHdFrameSeal(f)); SnesHdView v; RECON_CHECK(SnesHdFrameView(f, &v));
  RECON_CHECK(!v.lines[8].perspective);
  double t[4]; RECON_CHECK(SnesHdFrameTransform(f, 8, .5, t)); RECON_CHECK(t[2] == 256);
  SnesHdFrameDestroy(f);
}
static void test_flat_quantization_and_copy(void) {
  SnesHdFrame *f = SnesHdFrameCreate(1, 16);
  static uint16_t vram[32768]; RECON_CHECK(SnesHdFrameAddPicture(f, vram) == 0);
  for (unsigned y = 0; y < 16; ++y) {
    SnesHdLine j = {0}; j.kind = kSnesHdReconstructed; j.raster_line = y+1;
    j.active_right = 1; j.matrix[0] = y % 4 == 0 || y % 4 == 3 ? 255 : 256;
    RECON_CHECK(SnesHdFrameSetLine(f, y, &j));
  }
  RECON_CHECK(SnesHdFrameSeal(f)); double t[4];
  RECON_CHECK(SnesHdFrameTransform(f, 8, .5, t)); RECON_CHECK(t[2] > 254 && t[2] < 257);
  SnesHdFrame *copy = SnesHdFrameCreate(1, 1);
  RECON_CHECK(SnesHdFrameCopyLine(copy, 0, f, 8, 0) && SnesHdFrameSeal(copy));
  SnesHdView v; RECON_CHECK(SnesHdFrameView(copy, &v)); RECON_CHECK(!v.lines[0].perspective);
  SnesHdFrameDestroy(copy); SnesHdFrameDestroy(f);
}
static void test_horizon_edges(void) {
  for(unsigned pole=0;pole<2;++pole) {
    SnesHdFrame *f=SnesHdFrameCreate(1,64);static uint16_t vram[32768];
    RECON_CHECK(SnesHdFrameAddPicture(f,vram)==0);
    for(unsigned y=0;y<64;++y) {
      SnesHdLine j={0};j.kind=kSnesHdReconstructed;j.raster_line=y+1;j.active_right=1;
      j.matrix[0]=4096.0/(y+(pole?.25:21));j.matrix[3]=256;
      /* Edge rows may contain horizon/table artifacts; do not use them to
       * judge an otherwise continuous plane. Interior rows must still fit. */
      if(!pole && (y<2 || y>=62))j.matrix[0]*=1.08;
      RECON_CHECK(SnesHdFrameSetLine(f,y,&j));
    }
    RECON_CHECK(SnesHdFrameSeal(f));SnesHdView v;RECON_CHECK(SnesHdFrameView(f,&v));
    RECON_CHECK(pole ? !v.lines[0].perspective : v.lines[20].perspective!=0);
    if(!pole) {
      double t[4];RECON_CHECK(SnesHdFrameTransform(f,0,.25,t));
      RECON_CHECK(fabs(t[2]-4096/20.75)<1e-8);
    }
    SnesHdFrameDestroy(f);
  }
}
int main(void) {
  test_projection_geometry(); test_nonperspective(); test_flat_quantization_and_copy(); test_horizon_edges();
  puts("Mode 7 reconstruction: analytic perspective, quantized coefficients, camera splits, zero/sign-changing coefficients passed");
  return 0;
}
