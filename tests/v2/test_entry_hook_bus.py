"""WRAM return-frame sampling must preserve the entry-hook bus state."""
from _helpers import make_lorom_bank0
from test_region_pacing import _emit, run_region_cases
from v2.emit_function import emit_function


def test_paired_entry_preserves_bus_and_real_returns_still_latch(tmp_path):
    observer = r'''
static CpuState entry_state;
static unsigned entry_calls;
void ObserveEntry(const CpuState *cpu, uint32 pc) {
  (void)pc; entry_state = *cpu; entry_calls++;
}
'''
    emitted = observer
    for paced in (False, True):
        for frame, opcode in ((2, 0x60), (3, 0x6B)):
            name = f"Entry{frame}P{int(paced)}"
            code = f"EA {opcode:02X}"
            options = dict(entry_hooks={0x008000: "ObserveEntry"})
            if paced:
                emitted += _emit(code, name, **options)
            else:
                emitted += emit_function(
                    make_lorom_bank0({0x8000: bytes.fromhex(code)}),
                    bank=0, start=0x8000, end=0x8002, entry_m=1, entry_x=1,
                    func_name=name, **options)
    main = r'''
  { RecompReturn (*functions[2][2])(CpuState *) = {
      {Entry2P0_M1X1, Entry3P0_M1X1},
      {Entry2P1_M1X1, Entry3P1_M1X1}};
    const uint8 code[] = {0xea, 0x60};
    for (unsigned fast=0; fast<2; ++fast)
      for (unsigned paced=0; paced<2; ++paced)
        for (unsigned frame=2; frame<=3; ++frame)
          for (unsigned seed=2; seed<=0xa5; seed+=0xa3) {
            CpuState cpu;
            prepare(&cpu, code, sizeof code, 1, 1, fast);
            cpu.S=0x01fb; cpu.host_return_valid=frame; cpu.open_bus=seed;
            g_ram[0x01fc]=0x02; g_ram[0x01fd]=0x80; g_ram[0x01fe]=0x83;
            CpuState before=cpu; entry_calls=0;
            RecompReturn result=functions[paced][frame-2](&cpu);
            if (entry_calls!=1 || entry_state.open_bus!=seed ||
                entry_state.A!=before.A || entry_state.X!=before.X ||
                entry_state.Y!=before.Y || entry_state.S!=before.S ||
                entry_state.D!=before.D || entry_state.DB!=before.DB ||
                entry_state.PB!=before.PB || entry_state.P!=before.P ||
                entry_state.m_flag!=before.m_flag ||
                entry_state.x_flag!=before.x_flag ||
                entry_state.emulation!=before.emulation ||
                entry_state.cycles!=before.cycles ||
                entry_state.master_cycles!=before.master_cycles ||
                entry_state.coprocessor_master_cycles!=before.coprocessor_master_cycles ||
                result!=RECOMP_RETURN_NORMAL || g_other_exits ||
                cpu.S!=before.S+frame ||
                cpu.PB!=before.PB || /* paired caller owns PB restoration */
                cpu.open_bus!=(frame==3 ? 0x83 : 0x80)) {
              fprintf(stderr,"entry/return fast=%u paced=%u frame=%u bus=%02x/%02x S=%04x\n",
                      fast,paced,frame,entry_state.open_bus,seed,cpu.S);
              fails++;
            }
          }
  }
'''
    run_region_cases(tmp_path, cases=[], extra_emitted=emitted, extra_main=main)
