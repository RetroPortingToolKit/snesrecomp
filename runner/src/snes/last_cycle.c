/* The length of a 65816 instruction's last bus cycle, for the beam frame
 * driver's NMI recognition (beam_frame_driver.c): the NMI is taken after an
 * instruction only if that instruction starts a cycle at V=225 H=6 or later
 * (MesenCE SnesCpu::DetectNmiSignalEdge).
 *
 * The last cycle is an internal operation (6 master clocks), a code fetch, a
 * stack access or the instruction's last data access (the region's speed: 6
 * for I/O and FastROM, 8 for WRAM and SlowROM, 12 for the joypad ports).
 * Called after the instruction ran, with the registers it left: an index or D
 * that the instruction itself changed is not undone, which can only matter for
 * the region of an indexed or direct-page address. */
#include "snes/last_cycle.h"

#include "common_cpu_infra.h"
#include "cpu_state.h"
#include "snes/snes.h"

extern CpuState g_cpu;

enum Mode {
  IO,    /* internal operation last: implied, accumulator, transfers, REP/SEP, XBA, BRA/BRL, RTS, MVN/MVP */
  IMM,   /* operand fetch last: immediate, WDM, JMP abs/long */
  STK,   /* stack access last: pushes, pulls, JSR/JSL, RTL, RTI */
  VEC,   /* BRK/COP: the vector read from bank $00 */
  BR,    /* conditional branch: internal when taken, the operand fetch when not */
  DP, DPX, DPY, DPI, DPIX, DPIY, DPIL, DPILY, ABS, ABSX, ABSY, LONG, LONGX, SR, SRIY,
  JMPI,  /* JMP (abs) / JML [abs]: the pointer read from bank $00 */
  JMPIX, /* JMP (abs,X) / JSR (abs,X): the pointer read from the program bank */
};

static uint8_t mode_of(uint8_t op) {
  switch (op) {
    case 0x00: case 0x02: return VEC;
    case 0x08: case 0x0B: case 0x20: case 0x22: case 0x28: case 0x2B: case 0x40: case 0x48: case 0x4B:
    case 0x5A: case 0x62: case 0x68: case 0x6B: case 0x7A: case 0x8B: case 0xAB: case 0xD4: case 0xDA:
    case 0xF4: case 0xFA: return STK;
    case 0x10: case 0x30: case 0x50: case 0x70: case 0x90: case 0xB0: case 0xD0: case 0xF0: return BR;
    case 0x09: case 0x29: case 0x42: case 0x49: case 0x4C: case 0x5C: case 0x69: case 0x89: case 0xA0:
    case 0xA2: case 0xA9: case 0xC0: case 0xC9: case 0xE0: case 0xE9: return IMM;
    case 0x6C: case 0xDC: return JMPI;
    case 0x7C: case 0xFC: return JMPIX;
    case 0x96: case 0xB6: return DPY;
    case 0xBE: return ABSY;
    case 0x14: case 0x04: case 0x24: case 0x64: case 0x84: case 0x86: case 0xA4: case 0xA6: case 0xC4: case 0xE4:
      return DP;
    case 0x34: case 0x74: case 0x94: case 0xB4: return DPX;
    case 0x0C: case 0x1C: case 0x2C: case 0x8C: case 0x8E: case 0x9C: case 0xAC: case 0xAE: case 0xCC: case 0xEC:
      return ABS;
    case 0x3C: case 0x9E: case 0xBC: return ABSX;
  }
  const uint8_t lo = op & 0x0F, odd_row = (op >> 4) & 1;
  switch (lo) {
    case 0x1: return odd_row ? DPIY : DPIX;
    case 0x2: return odd_row ? DPI : IO;  /* even rows: COP/JSL/WDM/PER/BRL/REP/SEP, caught above or internal */
    case 0x3: return odd_row ? SRIY : SR;
    case 0x5: case 0x6: return odd_row ? DPX : DP;
    case 0x7: return odd_row ? DPILY : DPIL;
    case 0x9: return odd_row ? ABSY : IMM;
    case 0xD: case 0xE: return odd_row ? ABSX : ABS;
    case 0xF: return odd_row ? LONGX : LONG;
    default: return IO;
  }
}

/* WSEL region speed of one bus access. */
static unsigned speed(uint32_t bank, uint32_t addr) {
  bank &= 0xFF;
  addr &= 0xFFFF;
  if (bank >= 0x7E && bank <= 0x7F) return 8;
  if (bank & 0x40) return (bank & 0x80) && g_memsel ? 6 : 8;
  if (addr < 0x2000) return 8;
  if (addr < 0x4000) return 6;
  if (addr < 0x4200) return 12;
  if (addr < 0x6000) return 6;
  if (addr < 0x8000) return 8;
  return (bank & 0x80) && g_memsel ? 6 : 8;
}

/* A read with no side effect: I/O registers (some clear on read) count as 0. */
static uint8_t peek(uint32_t bank, uint32_t addr) {
  bank &= 0xFF;
  addr &= 0xFFFF;
  if (!(bank & 0x40) && addr >= 0x2000 && addr < 0x6000) return 0;
  return snes_read(g_snes, (bank << 16) | addr);
}
static uint32_t peek16(uint32_t bank, uint32_t addr) { return peek(bank, addr) | (uint32_t)peek(bank, addr + 1) << 8; }

unsigned snes_last_cycle_clocks(uint32_t pc24, uint32_t next_pc24) {
  const uint32_t pb = pc24 >> 16, pc = pc24 & 0xFFFF;
  const uint8_t op = peek(pb, pc);
  const uint32_t o8 = peek(pb, pc + 1), o16 = peek16(pb, pc + 1), o24 = o16 | (uint32_t)peek(pb, pc + 3) << 16;
  const uint32_t d = g_cpu.D, db = g_cpu.DB, x = g_cpu.X, y = g_cpu.Y, s = g_cpu.S;
  switch (mode_of(op)) {
    case IO: return 6;
    case IMM: return speed(pb, pc + 1);
    case STK: return speed(0, s);
    case VEC: return speed(0, 0xFFE7);
    case BR: return next_pc24 == ((pb << 16) | ((pc + 2) & 0xFFFF)) ? speed(pb, pc + 1) : 6;
    case DP: return speed(0, d + o8);
    case DPX: return speed(0, d + o8 + x);
    case DPY: return speed(0, d + o8 + y);
    case DPI: return speed(db, peek16(0, d + o8));
    case DPIX: return speed(db, peek16(0, d + o8 + x));
    case DPIY: return speed(db + ((peek16(0, d + o8) + y) >> 16), peek16(0, d + o8) + y);
    case DPIL: return speed(peek(0, d + o8 + 2), peek16(0, d + o8));
    case DPILY: return speed(peek(0, d + o8 + 2) + ((peek16(0, d + o8) + y) >> 16), peek16(0, d + o8) + y);
    case ABS: return speed(db, o16);
    case ABSX: return speed(db + ((o16 + x) >> 16), o16 + x);
    case ABSY: return speed(db + ((o16 + y) >> 16), o16 + y);
    case LONG: return speed(o24 >> 16, o24);
    case LONGX: return speed((o24 + x) >> 16, o24 + x);
    case SR: return speed(0, s + o8);
    case SRIY: return speed(db + ((peek16(0, s + o8) + y) >> 16), peek16(0, s + o8) + y);
    case JMPI: return speed(0, o16 + 1);
    case JMPIX: return speed(pb, o16 + x + 1);
  }
  return 6;
}
