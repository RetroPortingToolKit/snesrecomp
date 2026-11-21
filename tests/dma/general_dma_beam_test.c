/* Real general DMA producer interleaved with the CPU-owned HDMA/PPU. */
#define SNES_TEST_CPU_MASTER_NOW 1
#define SNES_TEST_PICTURE_NO_MAIN 1
#include "frame_render_hdma_test.c"
static Snes s;
static uint8_t values[8], sources[8];
static uint16_t lines[8], positions[8];
static unsigned writes;
static bool irq_probe=true;
static void written(uint32_t address,uint8_t value) {
  (void)address;REQUIRE(writes<8);values[writes]=value;sources[writes]=g_ppu_wlog_src;
  lines[writes]=s.vPos;positions[writes]=s.hPos;++writes;
}
static void setup(unsigned line,unsigned h) {
  memset(&s,0,sizeof s);memset(&p,0,sizeof p);memset(ram,0,sizeof ram);memset(&g_cpu,0,sizeof g_cpu);
  g_snes=&s;g_ppu=&p;s.ram=ram;s.ppu=&p;s.dma=dma_init(&s);REQUIRE(s.dma);
  dma_reset(s.dma);ppu_reset(&p);ppu_rasterReset();p.inidisp=15;s.vPos=line;s.hPos=h;
  s.beamMasterLast=g_cpu.master_cycles=line*1364+h;charged=0;g_snes_dma_charged=0;writes=0;
  snes_set_master_clock_charge_hook(charge_cpu);snes_set_ppu_beam_enabled(&s,true);
  ppu_rasterBeginPicture(&p);ppu_set_vram_write_log_hook(written);
}
static void finish(void) {dma_free(s.dma);ppu_set_vram_write_log_hook(NULL);}
static void upload(bool legacy,bool no_host) {
  setup(10,1060);s.hdmaBeamOff=legacy;
  const uint8_t table[]={0x81,0xa0,0};memcpy(ram+0x100,table,sizeof table);
  channel(0,0,0x100,0x18);dma_startDma(s.dma,1,true);dma_initHdma(s.dma);
  ram[0x200]=0xe1;ram[0x201]=0xe2;ram[0x202]=0xe3;channel(1,0,0x200,0x18);
  dma_write(s.dma,0x4315,3);dma_write(s.dma,0x4316,0);
  uint64_t begin=g_cpu.master_cycles;
  /* IRQ while CPU is halted must latch without making later DMA bus effects
   * occur at the stopped comparator or skipping the following HDMA request. */
  s.hIrqEnabled=irq_probe;s.hTimer=275;s.autoJoyTimer=4000;
  if(no_host)snes_set_master_clock_charge_hook(NULL);
  snes_writeReg(&s,0x420b,2);
  REQUIRE(s.inIrq==irq_probe && !s.dma->dmaBusy && !s.dma->channel[1].dmaActive && !s.dma->dmaTimer);
  if(legacy) {
    REQUIRE(writes==3 && values[0]==0xe1 && values[1]==0xe2 && values[2]==0xe3);
    REQUIRE(positions[0]==1101 && positions[1]==1101 && positions[2]==1101);
    /* Legacy precharge retains IRQ debt exactly as before. */
    REQUIRE(charged==48 && g_cpu.master_cycles==begin+48 && s.beamMasterLast<g_cpu.master_cycles);
    REQUIRE(s.autoJoyTimer==3952);
  } else {
    printf("general/HDMA bus: %02x@%u %02x@%u %02x@%u %02x@%u\n",
        values[0],positions[0],values[1],positions[1],values[2],positions[2],values[3],positions[3]);
    REQUIRE(writes==4 && values[0]==0xe1 && values[1]==0xe2 && values[2]==0xa0 && values[3]==0xe3);
    REQUIRE(sources[0]==kPpuWlogDma && sources[1]==kPpuWlogDma && sources[2]==kPpuWlogHdma && sources[3]==kPpuWlogDma);
    REQUIRE(lines[0]==10 && lines[3]==10 && positions[0]<1104 && positions[1]<1104 && positions[2]==1104 && positions[3]>1104);
    REQUIRE(p.vram[0]==0xe1 && p.vram[1]==0xe2 && p.vram[2]==0xa0 && p.vram[3]==0xe3);
    REQUIRE(g_snes_dma_charged==48 && s.beamMasterLast==begin+48+34);
    REQUIRE(s.autoJoyTimer==3918);
    REQUIRE(no_host ? charged==0 : (charged==82 && g_cpu.master_cycles==s.beamMasterLast));
  }
  finish();
}
static void field_read(void) {
  setup(224,1332);s.autoJoyRead=true;
  p.vramReadBuffer=0x1234;p.vramPointer=3;p.vramIncrement=32;
  p.vram[3]=0x5678;p.vram[35]=0x9abc;
  channel(0,0x80,0x200,0x39);dma_write(s.dma,0x4305,2);dma_write(s.dma,0x4306,0);
  uint64_t begin=g_cpu.master_cycles;snes_writeReg(&s,0x420b,1);
  REQUIRE(ram[0x200]==0x34 && ram[0x201]==0x78);
  REQUIRE(s.vPos==225 && s.hPos==10 && g_cpu.master_cycles==begin+42);
  REQUIRE(s.autoJoyTimer==4214 && p.vramPointer==67 && p.vramReadBuffer==0x9abc);
  /* Complete this next field, whose baseline must retain precisely the first
   * read's latch step. The second read is ordered history after the cut. */
  park(262*1364-s.hPos);REQUIRE(ppu_rasterReplayReady());
  ppu_rasterRenderBegin(&p);
  printf("field-cut read latch: pointer=%u buffer=%04x (expected35/5678)\n",p.vramPointer,p.vramReadBuffer);
  REQUIRE(p.vramPointer==35 && p.vramReadBuffer==0x5678);
  ppu_rasterApplyLine(&p,0);REQUIRE(p.vramPointer==67 && p.vramReadBuffer==0x9abc);
  ppu_rasterRenderEnd(&p);REQUIRE(p.vramPointer==67 && p.vramReadBuffer==0x9abc);
  finish();
}
static void phase_controls(void) {
  const char *align=getenv("SNESRECOMP_DMA_ALIGN");bool aligned=!(align && align[0]=='0');
  for(unsigned phase=0;phase<8;++phase) {
    setup(10,phase);uint64_t begin=g_cpu.master_cycles;
    ram[0x200]=0xa1;ram[0x300]=0xb0;ram[0x2ff]=0xb1;ram[0x2fe]=0xb2;
    channel(0,0,0x200,0x18);dma_write(s.dma,0x4305,1);dma_write(s.dma,0x4306,0);
    channel(1,(phase&1)?0x10:0x08,0x300,0x18);dma_write(s.dma,0x4315,3);dma_write(s.dma,0x4316,0);
    uint64_t expected=12+48;
    if(aligned) {expected=8-((begin+30)&7)+8+48;expected+=6-expected%6;}
    snes_writeReg(&s,0x420b,3);
    REQUIRE(g_cpu.master_cycles==begin+expected && s.beamMasterLast==g_cpu.master_cycles);
    REQUIRE(charged==expected && g_snes_dma_charged==expected);
    REQUIRE(writes==4 && p.vram[0]==0xa1 && p.vram[1]==0xb0);
    REQUIRE(p.vram[2]==((phase&1)?0xb1:0xb0) && p.vram[3]==((phase&1)?0xb2:0xb0));
    uint64_t before=g_cpu.master_cycles;snes_writeReg(&s,0x420b,0);
    REQUIRE(g_cpu.master_cycles==before+(aligned?0:12));finish();
  }
}
int main(int argc,char **argv) {
  if(argc==2 && !strcmp(argv[1],"--order-only")) {irq_probe=false;upload(false,false);puts("DMA order: PASS independent of IRQ preemption");return 0;}
  if(argc==2 && !strcmp(argv[1],"--legacy-only")) {upload(true,false);puts("legacy DMA: PASS unchanged bulk/IRQ-debt contract");return 0;}
  if(argc==2 && !strcmp(argv[1],"--field-only")) {field_read();puts("DMA field read: PASS ordered latch at field cut");return 0;}
  if(argc==2 && !strcmp(argv[1],"--phase-controls")) {phase_controls();puts("DMA phases: PASS eight alignments, multichannel/fixed/decrement, zero mask");return 0;}
  upload(false,false);upload(true,false);upload(false,true);
  irq_probe=false;upload(false,false);field_read();phase_controls();
  snes_set_master_clock_charge_hook(NULL);
  puts("general DMA beam: PASS actual payload/HDMA order, CPU halt/IRQ, legacy debt, hookless time and field-cut read latch");return 0;
}
