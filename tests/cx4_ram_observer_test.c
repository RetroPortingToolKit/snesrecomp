/* Synthetic HG51B execution: observe full values without replacing microcode. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "snes/cx4.h"

static unsigned calls;
static uint32_t instruction, value;
static uint16_t address;
static unsigned lane;
static void observed(void *context, uint32_t pc, uint16_t addr,
                     unsigned byte, uint32_t full) {
  assert(context == &calls);
  calls++; instruction=pc; address=addr; lane=byte; value=full;
}

static void run(Cx4 *c) {
  cx4_write(c,0x7f49,0);cx4_write(c,0x7f4a,0x80);cx4_write(c,0x7f4b,0);
  cx4_write(c,0x7f4f,0);
  for (uint64_t clock=128;clock<65536;clock+=128) {
    cx4_sync(c,clock);
    if (!(cx4_read(c,0x7f5e)&0xc0)) return;
  }
  assert(!"synthetic DSP did not halt");
}

int main(void) {
  uint8_t rom[32768]={0};
  /* LD A,3; SHL A,8; ST RAM,A; LD A,0; WRRAM byte0,A; HALT. */
  const uint16_t positive[]={0x6403,0xdc08,0xe00c,0x6400,0xe800,0xfc00};
  for (unsigned i=0;i<6;i++) {
    rom[i*2]=(uint8_t)positive[i];rom[i*2+1]=(uint8_t)(positive[i]>>8);
  }
  Cx4 *c=cx4_create(rom,sizeof rom,NULL,0);assert(c);
  cx4_set_ram_write_observer(c,observed,&calls);
  run(c);
  assert(calls==1 && instruction==0x8008 && address==0 && lane==0 && value==768);
  assert(cx4_read(c,0x6000)==0); /* Guest still sees only the written byte. */
  cx4_write(c,0x6000,0x52);
  assert(calls==2 && instruction==UINT32_MAX && value==0x52);
  assert(cx4_read(c,0x6000)==0x52);
  cx4_set_ram_write_observer(c,NULL,NULL);
  cx4_write(c,0x6000,0x34);assert(calls==2);
  cx4_reset(c);
  /* LD A,0; SUB A,1 gives the original 24-bit signed -1. */
  rom[0]=0;rom[1]=0x64;rom[2]=1;rom[3]=0x94;
  cx4_set_ram_write_observer(c,observed,&calls);
  run(c);
  assert(calls==3 && instruction==0x8008 && value==0xffffff);
  assert(cx4_read(c,0x6000)==0xff);
  cx4_destroy(c);
  puts("cx4_ram_observer_test: PASS");
  return 0;
}
