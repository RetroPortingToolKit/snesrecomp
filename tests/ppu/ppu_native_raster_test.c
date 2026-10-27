/* Native image ownership must not remove CPU-visible PPU line work. The
 * captured-image entry must reproduce the reference pixels without touching
 * the live PPU. Reuse the established synthetic renderer fixtures. */
#define main composition_fixture_main
#include "ppu_composition_regression_test.c"
#undef main
#include "snes/raster_jobs.h"

static uint32_t reference_pixels[256*224], captured_pixels[256*224];

static int render_jobs(Ppu *reference) {
  static SnesRenderColorCache cache;
  uint32_t rng=0x178bc06fu;
  for (unsigned sample=0; sample<64; ++sample) {
    ppu_reset(reference); fill_render_case(reference,&rng);
    PpuSetExtraSpace(reference,0);
    reference->bgmode=(uint8_t)(1|(rng_next(&rng)&0xf8));
    reference->inidisp=(uint8_t)(sample&15);
    reference->screenEnabled[0]=7; reference->screenEnabled[1]=0;
    reference->screenWindowed[0]=reference->screenWindowed[1]=0;
    reference->cgwsel=reference->cgadsub=reference->mosaic=reference->setini=0;
    for (unsigned layer=0; layer<3; ++layer) {
      reference->hScroll[layer]=(uint16_t)rng_next(&rng);
      reference->vScroll[layer]=(uint16_t)rng_next(&rng);
      reference->bgXsc[layer]=(uint8_t)rng_next(&rng);
    }
    reference->bgTileAdr=(uint16_t)rng_next(&rng);
    PpuBeginDrawing(reference,(uint8_t*)reference_pixels,256*4,kPpuRenderFlags_NewRenderer);
    ppu_runLine(reference,0);
    const uint32_t *rgb=SnesRenderBrightness(&cache,sample&15);
    for (unsigned line=1; line<=8; ++line) {
      uint16_t rows[3][332];
      for (unsigned layer=0; layer<3; ++layer) {
        rows[layer][0]=rows[layer][331]=0xa55a;
        if (!SnesRenderMode1TileRow(reference,reference->vram,layer,-37,(int)line,330,rows[layer]+1) ||
            rows[layer][0]!=0xa55a || rows[layer][331]!=0xa55a) return 1;
      }
      ppu_runLine(reference,(int)line);
      for (unsigned x=0; x<256; ++x) {
        uint16_t top=0x500;
        for (unsigned layer=0; layer<3; ++layer)
          if (rows[layer][x+38]>top) top=rows[layer][x+38];
        if (rgb[reference->cgram[top&255]&0x7fff]!=reference_pixels[(line-1)*256+x]) {
          fprintf(stderr,"shared tile job differs from full PPU sample=%u line=%u x=%u\n",sample,line,x);
          return 1;
        }
      }
    }
  }
  SnesRenderOamIndex index;
  uint16_t oam[256]={0}; uint8_t high[32]={0};
  oam[0]=oam[254]=0x0123; oam[1]=oam[255]=0x4567;
  if (!SnesRenderIndexOam(&index,oam,high,0,128)) return 1;
  const SnesRenderOamBinding *match=SnesRenderFindOam(&index,35,1,0x4567);
  if (!match || match->slots[0]!=1 || match->slots[1]!=(UINT64_C(1)<<63)) return 1;
  if (SnesRenderIndexOam(&index,oam,high,128,1)) return 1;
  return 0;
}

int main(void) {
  Ppu *reference=ppu_init(), *state=ppu_init(), *captured=ppu_init();
  uint32_t rng=0x81bded4fu;
  for (unsigned sample=0;sample<16;++sample) {
    ppu_reset(reference);
    fill_render_case(reference,&rng);
    PpuSetExtraSpace(reference,0);
    reference->bgmode=1;
    reference->setini=0;
    reference->mosaic=0;
    reference->inidisp=sample==15 ? 128 : 15;
    PpuBeginDrawing(reference,(uint8_t*)reference_pixels,256*4,
        kPpuRenderFlags_NewRenderer|(sample&1 ? kPpuRenderFlags_NoSpriteLimits : 0));
    *state=*reference; *captured=*reference;
    captured->renderBuffer=(uint8_t*)captured_pixels;
    for (unsigned line=0;line<=224;++line) {
      if (line && !(line&15u)) {
        uint16_t scroll=(uint16_t)rng_next(&rng), tile=(uint16_t)rng_next(&rng);
        reference->hScroll[0]=state->hScroll[0]=captured->hScroll[0]=scroll;
        reference->vram[0x400]=state->vram[0x400]=captured->vram[0x400]=tile;
      }
      ppu_runLine(reference,(int)line);
      ppu_runLineState(state,(int)line);
      ppu_drawCapturedLine(captured,(int)line,true);
      if (reference->rangeOver!=state->rangeOver || reference->timeOver!=state->timeOver ||
          reference->evenFrame!=state->evenFrame || reference->mosaicStartLine!=state->mosaicStartLine ||
          reference->lineHasSprites!=state->lineHasSprites || reference->oamAdr!=state->oamAdr ||
          reference->oamInHigh!=state->oamInHigh || reference->oamSecondWrite!=state->oamSecondWrite ||
          memcmp(reference->oam,state->oam,sizeof(reference->oam)) ||
          memcmp(reference->vram,state->vram,sizeof(reference->vram))) {
        fprintf(stderr,"native raster state mismatch sample=%u line=%u\n",sample,line);
        return 1;
      }
      if (line && memcmp(reference_pixels+(line-1)*256,captured_pixels+(line-1)*256,256*4)) {
        fprintf(stderr,"captured raster pixels mismatch sample=%u line=%u\n",sample,line);
        return 1;
      }
    }
  }
  if (render_jobs(reference)) return 1;
  ppu_free(reference); ppu_free(state); ppu_free(captured);
  puts("NATIVE RASTER CONTRACT PASSED");
  return 0;
}
