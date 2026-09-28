/*
 * Nintendo SA-1 coprocessor for SNESRecomp.
 *
 * The instruction engine is interp816 (LakeSnes, MIT). The surrounding SA-1
 * memory map and peripheral behavior are a C adaptation of ares' SA-1 core:
 *
 *   Copyright (c) 2004-2025 ares team, Near et al
 *   ISC license; see THIRD_PARTY_ATTRIBUTION.md.
 *
 * This file deliberately models the chip, not Super Mario RPG. No title
 * addresses or command shortcuts live here.
 */
#include "sa1.h"

#include "interp816.h"
#include "tier2_capture.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
  SA1_IRAM_SIZE = 2048,
  SA1_MASTER_CLOCKS_PER_CPU_CYCLE = 2,
};

typedef struct Sa1Io {
  /* $2200-$220f: CPU control and interrupt communication. */
  bool sa1_irq, sa1_ready, sa1_reset, sa1_nmi;
  uint8_t smeg;
  bool cpu_irq_enable, chdma_irq_enable;
  uint16_t reset_vector, nmi_vector, irq_vector;
  bool cpu_irq, cpu_irq_vector_switch, cpu_nmi_vector_switch;
  uint8_t cmeg;
  bool sa1_irq_enable, timer_irq_enable, dma_irq_enable, sa1_nmi_enable;
  uint16_t snes_nmi_vector, snes_irq_vector;

  /* Timer. */
  bool timer_linear, timer_v_enable, timer_h_enable;
  uint16_t h_count_target, v_count_target;

  /* Super MMC and write protection. */
  bool bank_mode[4];
  uint8_t bank_select[4];
  uint8_t snes_bwram_page;
  bool bwram_bitmap_window;
  uint8_t sa1_bwram_page;
  bool snes_bwram_write_enable, sa1_bwram_write_enable;
  uint8_t bwram_protect;
  uint8_t snes_iram_write_mask, sa1_iram_write_mask;

  /* DMA and character conversion. */
  bool dma_enable, dma_priority, char_conversion_enable;
  bool char_conversion_select, dma_dest_bwram;
  uint8_t dma_source;
  bool char_dma_end;
  uint8_t char_dma_size, char_dma_bpp;
  uint32_t dma_source_address, dma_dest_address;
  uint16_t dma_count;
  bool bitmap_2bpp;
  uint8_t bitmap_registers[16];

  /* Arithmetic and variable-bit reader. */
  bool arithmetic_accumulate, arithmetic_divide;
  uint16_t arithmetic_a, arithmetic_b;
  uint64_t arithmetic_result;
  bool arithmetic_overflow;
  bool variable_auto_increment;
  uint8_t variable_length, variable_bit;
  uint32_t variable_address;

  /* Flag registers. */
  bool cpu_irq_flag, chdma_irq_flag;
  bool sa1_irq_flag, timer_irq_flag, dma_irq_flag, sa1_nmi_flag;
  uint16_t latched_h_count, latched_v_count;
} Sa1Io;

struct Sa1 {
  uint8_t *rom;
  uint32_t rom_size;
  uint8_t *bwram;
  uint32_t bwram_size;
  uint8_t iram[SA1_IRAM_SIZE];
  Interp816 *cpu;
  Sa1Io io;

  uint64_t master_clock;
  uint64_t instructions;
  uint32_t h_counter;
  uint16_t v_counter;
  uint8_t char_dma_line;
  bool bwram_char_dma;
  uint8_t open_bus;
  uint8_t interrupt_vector_kind; /* 1=NMI, 2=IRQ */
  uint32_t extra_cycles;
  uint32_t cpu_bus_address;
  Sa1Observer observer;
  void *observer_context;
  uint32_t observer_addresses[4];
  size_t observer_count;
};

static uint8_t sa1_bus_read(void *opaque, uint32_t address);
static void sa1_bus_write(void *opaque, uint32_t address, uint8_t data);

static bool cpu_bus_owns_rom(const Sa1 *sa1) {
  uint32_t address = sa1->cpu_bus_address;
  return (address & 0x408000u) == 0x008000u ||
         (address & 0xc00000u) == 0xc00000u;
}

static uint32_t mirror_offset(uint32_t address, uint32_t size) {
  if (!size) return 0;
  if ((size & (size - 1)) == 0) return address & (size - 1);
  return address % size;
}

static uint8_t bwram_read_raw(Sa1 *sa1, uint32_t address) {
  if (!sa1->bwram || !sa1->bwram_size) return sa1->open_bus;
  return sa1->bwram[mirror_offset(address, sa1->bwram_size)];
}

static void bwram_write_raw(Sa1 *sa1, uint32_t address, uint8_t data) {
  if (!sa1->bwram || !sa1->bwram_size) return;
  sa1->bwram[mirror_offset(address, sa1->bwram_size)] = data;
}

static bool bwram_write_allowed(const Sa1 *sa1, uint32_t address) {
  if (sa1->io.snes_bwram_write_enable || sa1->io.sa1_bwram_write_enable)
    return true;
  uint32_t protected_size = 0x100u << sa1->io.bwram_protect;
  return (address & 0x3ffffu) >= protected_size;
}

static uint8_t bwram_read_bitmap(Sa1 *sa1, uint32_t address) {
  if (!sa1->io.bitmap_2bpp) {
    uint8_t value = bwram_read_raw(sa1, address >> 1);
    return (uint8_t)((value >> ((address & 1u) * 4u)) & 0x0f);
  }
  uint8_t value = bwram_read_raw(sa1, address >> 2);
  return (uint8_t)((value >> ((address & 3u) * 2u)) & 0x03);
}

static void bwram_write_bitmap(Sa1 *sa1, uint32_t address, uint8_t data) {
  uint32_t physical;
  unsigned shift;
  uint8_t mask;
  if (!sa1->io.bitmap_2bpp) {
    physical = address >> 1;
    shift = (address & 1u) * 4u;
    mask = 0x0f;
  } else {
    physical = address >> 2;
    shift = (address & 3u) * 2u;
    mask = 0x03;
  }
  if (!bwram_write_allowed(sa1, physical)) return;
  uint8_t value = bwram_read_raw(sa1, physical);
  value = (uint8_t)((value & ~(mask << shift)) | ((data & mask) << shift));
  bwram_write_raw(sa1, physical, value);
}

/* Convert a CPU-visible ROM address into the logical 0-4 MiB Super MMC
 * address before applying the four one-megabyte bank selectors. */
static bool rom_logical_offset(uint8_t bank, uint16_t address,
                               uint32_t *offset, bool *low_window) {
  uint8_t canonical = bank & 0x7f;
  if (address >= 0x8000 && canonical < 0x40) {
    uint32_t high_half = (bank & 0x80) ? 0x200000u : 0;
    *offset = high_half | ((uint32_t)canonical << 15)
            | (address & 0x7fffu);
    *low_window = true;
    return true;
  }
  if (bank >= 0xc0) {
    *offset = ((uint32_t)(bank - 0xc0) << 16) | address;
    *low_window = false;
    return true;
  }
  return false;
}

static uint32_t rom_mmc_offset(const Sa1 *sa1, uint32_t logical,
                               bool low_window) {
  unsigned segment = (logical >> 20) & 3u;
  if (low_window && !sa1->io.bank_mode[segment])
    return mirror_offset(logical, sa1->rom_size);
  uint32_t selected = (uint32_t)(sa1->io.bank_select[segment] & 7u) << 20;
  return mirror_offset(selected | (logical & 0x0fffffu), sa1->rom_size);
}

static bool cpu_vector_override(const Sa1 *sa1, uint8_t bank,
                                uint16_t address, uint8_t *value) {
  if ((bank & 0x7f) != 0 || address < 0xffe0 || address >= 0xfff0)
    return false;
  if (sa1->io.cpu_nmi_vector_switch &&
      (address == 0xffea || address == 0xffeb)) {
    *value = (uint8_t)(sa1->io.snes_nmi_vector >>
                       ((address & 1u) * 8u));
    return true;
  }
  if (sa1->io.cpu_irq_vector_switch &&
      (address == 0xffee || address == 0xffef)) {
    *value = (uint8_t)(sa1->io.snes_irq_vector >>
                       ((address & 1u) * 8u));
    return true;
  }
  return false;
}

static uint8_t rom_cpu_read(Sa1 *sa1, uint8_t bank, uint16_t address,
                            uint8_t open_bus) {
  uint8_t vector;
  if (cpu_vector_override(sa1, bank, address, &vector)) return vector;
  uint32_t logical;
  bool low_window;
  if (!sa1->rom || !sa1->rom_size ||
      !rom_logical_offset(bank, address, &logical, &low_window))
    return open_bus;
  return sa1->rom[rom_mmc_offset(sa1, logical, low_window)];
}

static uint8_t *rom_cpu_ptr(Sa1 *sa1, uint8_t bank, uint16_t address) {
  uint8_t ignored;
  if (cpu_vector_override(sa1, bank, address, &ignored)) return NULL;
  uint32_t logical;
  bool low_window;
  if (!sa1->rom || !sa1->rom_size ||
      !rom_logical_offset(bank, address, &logical, &low_window))
    return NULL;
  return &sa1->rom[rom_mmc_offset(sa1, logical, low_window)];
}

static uint8_t iram_read(Sa1 *sa1, uint32_t address) {
  return sa1->iram[address & (SA1_IRAM_SIZE - 1)];
}

static void iram_write(Sa1 *sa1, uint32_t address, uint8_t data,
                       bool from_sa1) {
  unsigned page = (address >> 8) & 7u;
  uint8_t mask = from_sa1 ? sa1->io.sa1_iram_write_mask
                          : sa1->io.snes_iram_write_mask;
  if (mask & (1u << page))
    sa1->iram[address & (SA1_IRAM_SIZE - 1)] = data;
}

static uint8_t variable_read(Sa1 *sa1, uint32_t address) {
  uint8_t bank = (uint8_t)(address >> 16);
  uint16_t lo = (uint16_t)address;
  uint32_t logical;
  bool low_window;
  if (rom_logical_offset(bank, lo, &logical, &low_window))
    return sa1->rom[rom_mmc_offset(sa1, logical, low_window)];
  if (((bank & 0x7f) < 0x40) && lo >= 0x6000 && lo < 0x8000) {
    uint32_t offset = (uint32_t)sa1->io.snes_bwram_page * 0x2000u
                    + (lo & 0x1fffu);
    return bwram_read_raw(sa1, offset);
  }
  if (bank >= 0x40 && bank < 0x50)
    return bwram_read_raw(sa1, ((uint32_t)(bank - 0x40) << 16) | lo);
  if (((bank & 0x7f) < 0x40) &&
      (lo < 0x0800 || (lo >= 0x3000 && lo < 0x3800)))
    return iram_read(sa1, lo);
  return 0xff;
}

static uint8_t variable_port(Sa1 *sa1, bool high) {
  uint32_t bits = (uint32_t)variable_read(sa1, sa1->io.variable_address)
                | ((uint32_t)variable_read(sa1,
                    (sa1->io.variable_address + 1) & 0xffffffu) << 8)
                | ((uint32_t)variable_read(sa1,
                    (sa1->io.variable_address + 2) & 0xffffffu) << 16);
  bits >>= sa1->io.variable_bit;
  uint8_t result = (uint8_t)(bits >> (high ? 8 : 0));
  if (high && sa1->io.variable_auto_increment) {
    unsigned advance = sa1->io.variable_bit + sa1->io.variable_length;
    sa1->io.variable_address =
        (sa1->io.variable_address + (advance >> 3)) & 0xffffffu;
    sa1->io.variable_bit = (uint8_t)(advance & 7u);
  }
  return result;
}

static void update_snes_irq(Sa1 *sa1) {
  (void)sa1;
  /* The line is queried through sa1_cpu_irq_pending(). Keeping it derived
   * avoids stealing the SNES timer IRQ latch from the base console model. */
}

static uint8_t io_read_cpu(Sa1 *sa1, uint16_t address, uint8_t open_bus) {
  if (address == 0x2300) {
    return (uint8_t)((sa1->io.cmeg & 0x0f)
        | (sa1->io.cpu_nmi_vector_switch ? 0x10 : 0)
        | (sa1->io.chdma_irq_flag ? 0x20 : 0)
        | (sa1->io.cpu_irq_vector_switch ? 0x40 : 0)
        | (sa1->io.cpu_irq_flag ? 0x80 : 0));
  }
  return open_bus;
}

static uint8_t io_read_sa1(Sa1 *sa1, uint16_t address) {
  switch (address) {
    case 0x2301:
      return (uint8_t)((sa1->io.smeg & 0x0f)
          | (sa1->io.sa1_nmi_flag ? 0x10 : 0)
          | (sa1->io.dma_irq_flag ? 0x20 : 0)
          | (sa1->io.timer_irq_flag ? 0x40 : 0)
          | (sa1->io.sa1_irq_flag ? 0x80 : 0));
    case 0x2302:
      sa1->io.latched_h_count = (uint16_t)(sa1->h_counter >> 2);
      sa1->io.latched_v_count = sa1->v_counter;
      return (uint8_t)sa1->io.latched_h_count;
    case 0x2303: return (uint8_t)(sa1->io.latched_h_count >> 8);
    case 0x2304: return (uint8_t)sa1->io.latched_v_count;
    case 0x2305: return (uint8_t)(sa1->io.latched_v_count >> 8);
    case 0x2306: return (uint8_t)(sa1->io.arithmetic_result >> 0);
    case 0x2307: return (uint8_t)(sa1->io.arithmetic_result >> 8);
    case 0x2308: return (uint8_t)(sa1->io.arithmetic_result >> 16);
    case 0x2309: return (uint8_t)(sa1->io.arithmetic_result >> 24);
    case 0x230a: return (uint8_t)(sa1->io.arithmetic_result >> 32);
    case 0x230b: return sa1->io.arithmetic_overflow ? 0x80 : 0;
    case 0x230c: return variable_port(sa1, false);
    case 0x230d: return variable_port(sa1, true);
    default: return 0xff;
  }
}

static void timer_advance(Sa1 *sa1, uint32_t clocks) {
  while (clocks--) {
    if (!sa1->io.timer_linear) {
      if (++sa1->h_counter >= 1364) {
        sa1->h_counter = 0;
        if (++sa1->v_counter >= 262) sa1->v_counter = 0;
      }
    } else {
      if (++sa1->h_counter >= 2048) {
        sa1->h_counter = 0;
        sa1->v_counter = (uint16_t)((sa1->v_counter + 1) & 0x01ff);
      }
    }
    bool match = false;
    if (sa1->io.timer_h_enable && sa1->io.timer_v_enable)
      match = sa1->h_counter == ((uint32_t)sa1->io.h_count_target << 2)
           && sa1->v_counter == sa1->io.v_count_target;
    else if (sa1->io.timer_h_enable)
      match = sa1->h_counter == ((uint32_t)sa1->io.h_count_target << 2);
    else if (sa1->io.timer_v_enable)
      match = sa1->h_counter == 0
           && sa1->v_counter == sa1->io.v_count_target;
    if (match) sa1->io.timer_irq_flag = true;
  }
}

static void dma_finish(Sa1 *sa1) {
  sa1->io.dma_irq_flag = true;
}

static uint8_t dma_source_read(Sa1 *sa1, uint32_t address) {
  if (sa1->io.dma_source == 0) {
    uint8_t bank = (uint8_t)(address >> 16);
    return rom_cpu_read(sa1, bank, (uint16_t)address, sa1->open_bus);
  }
  if (sa1->io.dma_source == 1) return bwram_read_raw(sa1, address);
  if (sa1->io.dma_source == 2) return iram_read(sa1, address);
  return sa1->open_bus;
}

static void dma_normal(Sa1 *sa1) {
  uint32_t count = sa1->io.dma_count ? sa1->io.dma_count : 0x10000u;
  /* ares' cycle-level core charges these clocks inside dmaNormal(). They are
   * in addition to the instruction that completed the DMA register write. */
  uint32_t cycles_per_byte =
      sa1->io.dma_source == 0 && !sa1->io.dma_dest_bwram ? 1u : 2u;
  while (count--) {
    uint8_t data = dma_source_read(sa1, sa1->io.dma_source_address++);
    if (sa1->io.dma_dest_bwram) {
      if (bwram_write_allowed(sa1, sa1->io.dma_dest_address))
        bwram_write_raw(sa1, sa1->io.dma_dest_address, data);
    } else {
      /* DMA is not subject to CIWP. */
      sa1->iram[sa1->io.dma_dest_address & 0x07ff] = data;
    }
    sa1->io.dma_source_address &= 0xffffffu;
    sa1->io.dma_dest_address =
        (sa1->io.dma_dest_address + 1u) & 0xffffffu;
    sa1->extra_cycles += cycles_per_byte;
  }
  sa1->io.dma_count = 0;
  dma_finish(sa1);
}

static void dma_cc1_begin(Sa1 *sa1) {
  sa1->bwram_char_dma = true;
  sa1->io.chdma_irq_flag = true;
  update_snes_irq(sa1);
}

static uint8_t dma_cc1_read(Sa1 *sa1, uint32_t address) {
  unsigned char_bytes = 1u << (6u - sa1->io.char_dma_bpp);
  unsigned char_mask = char_bytes - 1u;
  if ((address & char_mask) == 0) {
    unsigned bpp = 2u << (2u - sa1->io.char_dma_bpp);
    unsigned bytes_per_line =
        (8u << sa1->io.char_dma_size) >> sa1->io.char_dma_bpp;
    uint32_t tile = mirror_offset(address - sa1->io.dma_source_address,
                                  sa1->bwram_size) >> (6u - sa1->io.char_dma_bpp);
    uint32_t tile_y = tile >> sa1->io.char_dma_size;
    uint32_t tile_x = tile & ((1u << sa1->io.char_dma_size) - 1u);
    uint32_t source = sa1->io.dma_source_address
                    + tile_y * 8u * bytes_per_line + tile_x * bpp;
    for (unsigned y = 0; y < 8; y++) {
      uint64_t packed = 0;
      for (unsigned byte = 0; byte < bpp; byte++)
        packed |= (uint64_t)bwram_read_raw(sa1, source + byte) << (byte * 8u);
      source += bytes_per_line;
      uint8_t planar[8] = {0};
      for (unsigned x = 0; x < 8; x++) {
        for (unsigned plane = 0; plane < bpp; plane++) {
          planar[plane] |= (uint8_t)((packed & 1u) << (7u - x));
          packed >>= 1;
        }
      }
      for (unsigned byte = 0; byte < bpp; byte++) {
        uint32_t target = sa1->io.dma_dest_address + y * 2u
                        + ((byte & 6u) << 3) + (byte & 1u);
        sa1->iram[target & 0x07ff] = planar[byte];
      }
    }
  }
  return iram_read(sa1, sa1->io.dma_dest_address + (address & char_mask));
}

static void dma_cc2_line(Sa1 *sa1) {
  const uint8_t *source =
      &sa1->io.bitmap_registers[(sa1->char_dma_line & 1u) << 3];
  unsigned bpp = 2u << (2u - sa1->io.char_dma_bpp);
  uint32_t address = sa1->io.dma_dest_address & 0x07ffu;
  address &= ~((1u << (7u - sa1->io.char_dma_bpp)) - 1u);
  address += (sa1->char_dma_line & 8u) * bpp;
  address += (sa1->char_dma_line & 7u) * 2u;
  for (unsigned plane = 0; plane < bpp; plane++) {
    uint8_t output = 0;
    for (unsigned bit = 0; bit < 8; bit++)
      output |= (uint8_t)(((source[bit] >> plane) & 1u) << (7u - bit));
    sa1->iram[(address + ((plane & 6u) << 3) + (plane & 1u)) & 0x07ff] =
        output;
  }
  sa1->char_dma_line = (uint8_t)((sa1->char_dma_line + 1) & 15);
}

static void io_write_shared(Sa1 *sa1, uint16_t address, uint8_t data) {
  switch (address) {
    case 0x2231:
      sa1->io.char_dma_bpp = data & 3u;
      if (sa1->io.char_dma_bpp > 2) sa1->io.char_dma_bpp = 2;
      sa1->io.char_dma_size = (data >> 2) & 7u;
      if (sa1->io.char_dma_size > 5) sa1->io.char_dma_size = 5;
      sa1->io.char_dma_end = (data & 0x80) != 0;
      if (sa1->io.char_dma_end) sa1->bwram_char_dma = false;
      break;
    case 0x2232:
      sa1->io.dma_source_address =
          (sa1->io.dma_source_address & 0xffff00u) | data;
      break;
    case 0x2233:
      sa1->io.dma_source_address =
          (sa1->io.dma_source_address & 0xff00ffu) | ((uint32_t)data << 8);
      break;
    case 0x2234:
      sa1->io.dma_source_address =
          (sa1->io.dma_source_address & 0x00ffffu) | ((uint32_t)data << 16);
      break;
    case 0x2235:
      sa1->io.dma_dest_address =
          (sa1->io.dma_dest_address & 0xffff00u) | data;
      break;
    case 0x2236:
      sa1->io.dma_dest_address =
          (sa1->io.dma_dest_address & 0xff00ffu) | ((uint32_t)data << 8);
      if (sa1->io.dma_enable) {
        if (!sa1->io.char_conversion_enable &&
            !sa1->io.dma_dest_bwram) dma_normal(sa1);
        else if (sa1->io.char_conversion_enable &&
                 sa1->io.char_conversion_select) dma_cc1_begin(sa1);
      }
      break;
    case 0x2237:
      sa1->io.dma_dest_address =
          (sa1->io.dma_dest_address & 0x00ffffu) | ((uint32_t)data << 16);
      if (sa1->io.dma_enable && !sa1->io.char_conversion_enable &&
          sa1->io.dma_dest_bwram) dma_normal(sa1);
      break;
  }
}

static void io_write_cpu(Sa1 *sa1, uint16_t address, uint8_t data) {
  switch (address) {
    case 0x2200: {
      bool was_reset = sa1->io.sa1_reset;
      bool reset = (data & 0x20) != 0;
      if (was_reset && !reset) {
        interp816_reset(sa1->cpu);
        sa1->cpu->pc = sa1->io.reset_vector;
        sa1->cpu->k = 0;
        sa1->cpu->db = 0;
        sa1->cpu->stopped = false;
        sa1->cpu->waiting = false;
        sa1->io.sa1_iram_write_mask = 0;
      }
      sa1->io.smeg = data & 0x0f;
      sa1->io.sa1_nmi = (data & 0x10) != 0;
      sa1->io.sa1_reset = reset;
      sa1->io.sa1_ready = (data & 0x40) != 0;
      sa1->io.sa1_irq = (data & 0x80) != 0;
      if (sa1->io.sa1_irq) sa1->io.sa1_irq_flag = true;
      if (sa1->io.sa1_nmi) sa1->io.sa1_nmi_flag = true;
      break;
    }
    case 0x2201:
      sa1->io.chdma_irq_enable = (data & 0x20) != 0;
      sa1->io.cpu_irq_enable = (data & 0x80) != 0;
      update_snes_irq(sa1);
      break;
    case 0x2202:
      if (data & 0x20) sa1->io.chdma_irq_flag = false;
      if (data & 0x80) sa1->io.cpu_irq_flag = false;
      update_snes_irq(sa1);
      break;
    case 0x2203:
      sa1->io.reset_vector = (sa1->io.reset_vector & 0xff00) | data; break;
    case 0x2204:
      sa1->io.reset_vector = (sa1->io.reset_vector & 0x00ff)
                           | ((uint16_t)data << 8); break;
    case 0x2205:
      sa1->io.nmi_vector = (sa1->io.nmi_vector & 0xff00) | data; break;
    case 0x2206:
      sa1->io.nmi_vector = (sa1->io.nmi_vector & 0x00ff)
                         | ((uint16_t)data << 8); break;
    case 0x2207:
      sa1->io.irq_vector = (sa1->io.irq_vector & 0xff00) | data; break;
    case 0x2208:
      sa1->io.irq_vector = (sa1->io.irq_vector & 0x00ff)
                         | ((uint16_t)data << 8); break;
    case 0x2220: case 0x2221: case 0x2222: case 0x2223: {
      unsigned slot = address - 0x2220;
      sa1->io.bank_select[slot] = data & 7u;
      sa1->io.bank_mode[slot] = (data & 0x80) != 0;
      break;
    }
    case 0x2224: sa1->io.snes_bwram_page = data & 0x1f; break;
    case 0x2226: sa1->io.snes_bwram_write_enable = (data & 0x80) != 0; break;
    case 0x2228: sa1->io.bwram_protect = data & 0x0f; break;
    case 0x2229: sa1->io.snes_iram_write_mask = data; break;
    case 0x2231: case 0x2232: case 0x2233: case 0x2234:
    case 0x2235: case 0x2236: case 0x2237:
      io_write_shared(sa1, address, data); break;
  }
}

static int16_t signed_remainder(int16_t dividend, uint16_t divisor) {
  int32_t rem = dividend % (int32_t)divisor;
  if (rem < 0) rem += divisor;
  return (int16_t)rem;
}

static void arithmetic_execute(Sa1 *sa1) {
  if (!sa1->io.arithmetic_accumulate) {
    if (!sa1->io.arithmetic_divide) {
      int32_t product = (int16_t)sa1->io.arithmetic_a
                      * (int16_t)sa1->io.arithmetic_b;
      sa1->io.arithmetic_result = (uint32_t)product;
      sa1->io.arithmetic_b = 0;
    } else {
      uint16_t divisor = sa1->io.arithmetic_b;
      if (!divisor) {
        sa1->io.arithmetic_result = 0;
      } else {
        int16_t dividend = (int16_t)sa1->io.arithmetic_a;
        uint16_t remainder = (uint16_t)signed_remainder(dividend, divisor);
        int16_t quotient = (int16_t)((dividend - (int32_t)remainder) /
                                     (int32_t)divisor);
        sa1->io.arithmetic_result =
            ((uint32_t)remainder << 16) | (uint16_t)quotient;
      }
      sa1->io.arithmetic_a = 0;
      sa1->io.arithmetic_b = 0;
    }
  } else {
    int64_t add = (int16_t)sa1->io.arithmetic_a
                * (int16_t)sa1->io.arithmetic_b;
    uint64_t result = sa1->io.arithmetic_result + add;
    sa1->io.arithmetic_overflow = (result & ~UINT64_C(0xffffffffff)) != 0;
    sa1->io.arithmetic_result = result & UINT64_C(0xffffffffff);
    sa1->io.arithmetic_b = 0;
  }
}

static void io_write_sa1(Sa1 *sa1, uint16_t address, uint8_t data) {
  switch (address) {
    case 0x2209:
      sa1->io.cmeg = data & 0x0f;
      sa1->io.cpu_nmi_vector_switch = (data & 0x10) != 0;
      sa1->io.cpu_irq_vector_switch = (data & 0x40) != 0;
      sa1->io.cpu_irq = (data & 0x80) != 0;
      if (sa1->io.cpu_irq) sa1->io.cpu_irq_flag = true;
      update_snes_irq(sa1);
      break;
    case 0x220a:
      sa1->io.sa1_nmi_enable = (data & 0x10) != 0;
      sa1->io.dma_irq_enable = (data & 0x20) != 0;
      sa1->io.timer_irq_enable = (data & 0x40) != 0;
      sa1->io.sa1_irq_enable = (data & 0x80) != 0;
      break;
    case 0x220b:
      if (data & 0x10) sa1->io.sa1_nmi_flag = false;
      if (data & 0x20) sa1->io.dma_irq_flag = false;
      if (data & 0x40) sa1->io.timer_irq_flag = false;
      if (data & 0x80) sa1->io.sa1_irq_flag = false;
      break;
    case 0x220c:
      sa1->io.snes_nmi_vector = (sa1->io.snes_nmi_vector & 0xff00) | data; break;
    case 0x220d:
      sa1->io.snes_nmi_vector = (sa1->io.snes_nmi_vector & 0x00ff)
                              | ((uint16_t)data << 8); break;
    case 0x220e:
      sa1->io.snes_irq_vector = (sa1->io.snes_irq_vector & 0xff00) | data; break;
    case 0x220f:
      sa1->io.snes_irq_vector = (sa1->io.snes_irq_vector & 0x00ff)
                              | ((uint16_t)data << 8); break;
    case 0x2210:
      sa1->io.timer_h_enable = (data & 1) != 0;
      sa1->io.timer_v_enable = (data & 2) != 0;
      sa1->io.timer_linear = (data & 0x80) != 0;
      break;
    case 0x2211: sa1->h_counter = sa1->v_counter = 0; break;
    case 0x2212:
      sa1->io.h_count_target = (sa1->io.h_count_target & 0xff00) | data; break;
    case 0x2213:
      sa1->io.h_count_target = (sa1->io.h_count_target & 0x00ff)
                             | ((uint16_t)data << 8); break;
    case 0x2214:
      sa1->io.v_count_target = (sa1->io.v_count_target & 0xff00) | data; break;
    case 0x2215:
      sa1->io.v_count_target = (sa1->io.v_count_target & 0x00ff)
                             | ((uint16_t)data << 8); break;
    case 0x2225:
      sa1->io.sa1_bwram_page = data & 0x7f;
      sa1->io.bwram_bitmap_window = (data & 0x80) != 0;
      break;
    case 0x2227: sa1->io.sa1_bwram_write_enable = (data & 0x80) != 0; break;
    case 0x222a: sa1->io.sa1_iram_write_mask = data; break;
    case 0x2230:
      sa1->io.dma_source = data & 3u;
      sa1->io.dma_dest_bwram = (data & 4) != 0;
      sa1->io.char_conversion_select = (data & 0x10) != 0;
      sa1->io.char_conversion_enable = (data & 0x20) != 0;
      sa1->io.dma_priority = (data & 0x40) != 0;
      sa1->io.dma_enable = (data & 0x80) != 0;
      if (!sa1->io.dma_enable) sa1->char_dma_line = 0;
      break;
    case 0x2231: case 0x2232: case 0x2233: case 0x2234:
    case 0x2235: case 0x2236: case 0x2237:
      io_write_shared(sa1, address, data); break;
    case 0x2238:
      sa1->io.dma_count = (sa1->io.dma_count & 0xff00) | data; break;
    case 0x2239:
      sa1->io.dma_count = (sa1->io.dma_count & 0x00ff)
                        | ((uint16_t)data << 8); break;
    case 0x223f: sa1->io.bitmap_2bpp = (data & 0x80) != 0; break;
    case 0x2250:
      sa1->io.arithmetic_divide = (data & 1) != 0;
      sa1->io.arithmetic_accumulate = (data & 2) != 0;
      if (sa1->io.arithmetic_accumulate) sa1->io.arithmetic_result = 0;
      break;
    case 0x2251:
      sa1->io.arithmetic_a = (sa1->io.arithmetic_a & 0xff00) | data; break;
    case 0x2252:
      sa1->io.arithmetic_a = (sa1->io.arithmetic_a & 0x00ff)
                           | ((uint16_t)data << 8); break;
    case 0x2253:
      sa1->io.arithmetic_b = (sa1->io.arithmetic_b & 0xff00) | data; break;
    case 0x2254:
      sa1->io.arithmetic_b = (sa1->io.arithmetic_b & 0x00ff)
                           | ((uint16_t)data << 8);
      arithmetic_execute(sa1);
      break;
    case 0x2258: {
      sa1->io.variable_length = data & 0x0f;
      if (!sa1->io.variable_length) sa1->io.variable_length = 16;
      sa1->io.variable_auto_increment = (data & 0x80) != 0;
      if (!sa1->io.variable_auto_increment) {
        unsigned advance = sa1->io.variable_bit + sa1->io.variable_length;
        sa1->io.variable_address =
            (sa1->io.variable_address + (advance >> 3)) & 0xffffffu;
        sa1->io.variable_bit = (uint8_t)(advance & 7u);
      }
      break;
    }
    case 0x2259:
      sa1->io.variable_address =
          (sa1->io.variable_address & 0xffff00u) | data; break;
    case 0x225a:
      sa1->io.variable_address =
          (sa1->io.variable_address & 0xff00ffu) | ((uint32_t)data << 8); break;
    case 0x225b:
      sa1->io.variable_address =
          (sa1->io.variable_address & 0x00ffffu) | ((uint32_t)data << 16);
      sa1->io.variable_bit = 0;
      break;
    default:
      if (address >= 0x2240 && address <= 0x224f) {
        unsigned index = address - 0x2240;
        sa1->io.bitmap_registers[index] = data;
        if ((index == 7 || index == 15) && sa1->io.dma_enable &&
            sa1->io.char_conversion_enable &&
            !sa1->io.char_conversion_select) dma_cc2_line(sa1);
      }
      break;
  }
}

static uint8_t bwram_read_cpu(Sa1 *sa1, uint8_t bank, uint16_t address) {
  uint32_t offset;
  if (((bank & 0x7f) < 0x40) && address >= 0x6000 && address < 0x8000)
    offset = (uint32_t)sa1->io.snes_bwram_page * 0x2000u
           + (address & 0x1fffu);
  else
    offset = ((uint32_t)(bank & 0x0f) << 16) | address;
  return sa1->bwram_char_dma ? dma_cc1_read(sa1, offset)
                             : bwram_read_raw(sa1, offset);
}

static void bwram_write_cpu(Sa1 *sa1, uint8_t bank, uint16_t address,
                            uint8_t data) {
  uint32_t offset;
  if (((bank & 0x7f) < 0x40) && address >= 0x6000 && address < 0x8000)
    offset = (uint32_t)sa1->io.snes_bwram_page * 0x2000u
           + (address & 0x1fffu);
  else
    offset = ((uint32_t)(bank & 0x0f) << 16) | address;
  if (bwram_write_allowed(sa1, offset)) bwram_write_raw(sa1, offset, data);
}

static uint8_t bwram_read_sa1_window(Sa1 *sa1, uint16_t address) {
  uint32_t offset = (uint32_t)sa1->io.sa1_bwram_page * 0x2000u
                  + (address & 0x1fffu);
  return sa1->io.bwram_bitmap_window
      ? bwram_read_bitmap(sa1, offset) : bwram_read_raw(sa1, offset);
}

static void bwram_write_sa1_window(Sa1 *sa1, uint16_t address, uint8_t data) {
  uint32_t offset = (uint32_t)sa1->io.sa1_bwram_page * 0x2000u
                  + (address & 0x1fffu);
  if (sa1->io.bwram_bitmap_window) bwram_write_bitmap(sa1, offset, data);
  else if (bwram_write_allowed(sa1, offset)) bwram_write_raw(sa1, offset, data);
}

uint8_t sa1_cpu_read(Sa1 *sa1, uint8_t bank, uint16_t address,
                     uint8_t open_bus) {
  if (!sa1) return open_bus;
  sa1->open_bus = open_bus;
  uint8_t canonical = bank & 0x7f;
  uint8_t result = open_bus;
  if (canonical < 0x40 && address >= 0x2200 && address < 0x2400)
    result = io_read_cpu(sa1, address, open_bus);
  else if (canonical < 0x40 && address >= 0x3000 && address < 0x3800)
    result = iram_read(sa1, address);
  else if ((canonical < 0x40 && address >= 0x6000 && address < 0x8000) ||
           (bank >= 0x40 && bank < 0x50))
    result = bwram_read_cpu(sa1, bank, address);
  else
    result = rom_cpu_read(sa1, bank, address, open_bus);
  sa1->open_bus = result;
  return result;
}

void sa1_cpu_write(Sa1 *sa1, uint8_t bank, uint16_t address, uint8_t data) {
  if (!sa1) return;
  sa1->open_bus = data;
  uint8_t canonical = bank & 0x7f;
  if (canonical < 0x40 && address >= 0x2200 && address < 0x2400)
    io_write_cpu(sa1, address, data);
  else if (canonical < 0x40 && address >= 0x3000 && address < 0x3800)
    iram_write(sa1, address, data, false);
  else if ((canonical < 0x40 && address >= 0x6000 && address < 0x8000) ||
           (bank >= 0x40 && bank < 0x50))
    bwram_write_cpu(sa1, bank, address, data);
}

static uint8_t sa1_bus_read(void *opaque, uint32_t address) {
  Sa1 *sa1 = (Sa1 *)opaque;
  uint8_t bank = (uint8_t)(address >> 16);
  uint16_t lo = (uint16_t)address;
  uint8_t canonical = bank & 0x7f;
  uint8_t result = sa1->open_bus;

  if (bank == 0 && sa1->interrupt_vector_kind &&
      (lo == 0xffea || lo == 0xffeb || lo == 0xffee || lo == 0xffef ||
       lo == 0xfffa || lo == 0xfffb || lo == 0xfffe || lo == 0xffff)) {
    uint16_t vector = sa1->interrupt_vector_kind == 1
                    ? sa1->io.nmi_vector : sa1->io.irq_vector;
    result = (uint8_t)(vector >> ((lo & 1u) * 8u));
  } else if (canonical < 0x40 && lo >= 0x2200 && lo < 0x2400) {
    result = io_read_sa1(sa1, lo);
  } else if ((canonical < 0x40) &&
             (lo < 0x0800 || (lo >= 0x3000 && lo < 0x3800))) {
    result = iram_read(sa1, lo);
  } else if (canonical < 0x40 && lo >= 0x6000 && lo < 0x8000) {
    sa1->extra_cycles++;
    result = bwram_read_sa1_window(sa1, lo);
  } else if (bank >= 0x40 && bank < 0x60) {
    sa1->extra_cycles++;
    result = bwram_read_raw(sa1, ((uint32_t)(bank - 0x40) << 16) | lo);
  } else if (bank >= 0x60 && bank < 0x70) {
    sa1->extra_cycles++;
    result = bwram_read_bitmap(sa1, ((uint32_t)(bank - 0x60) << 16) | lo);
  } else {
    if (cpu_bus_owns_rom(sa1)) sa1->extra_cycles++;
    result = rom_cpu_read(sa1, bank, lo, sa1->open_bus);
  }
  sa1->open_bus = result;
  return result;
}

static void sa1_bus_write(void *opaque, uint32_t address, uint8_t data) {
  Sa1 *sa1 = (Sa1 *)opaque;
  uint8_t bank = (uint8_t)(address >> 16);
  uint16_t lo = (uint16_t)address;
  uint8_t canonical = bank & 0x7f;
  sa1->open_bus = data;
  if (canonical < 0x40 && lo >= 0x2200 && lo < 0x2400)
    io_write_sa1(sa1, lo, data);
  else if ((canonical < 0x40) &&
           (lo < 0x0800 || (lo >= 0x3000 && lo < 0x3800)))
    iram_write(sa1, lo, data, true);
  else if (canonical < 0x40 && lo >= 0x6000 && lo < 0x8000) {
    sa1->extra_cycles++;
    bwram_write_sa1_window(sa1, lo, data);
  } else if (bank >= 0x40 && bank < 0x60) {
    sa1->extra_cycles++;
    uint32_t offset = ((uint32_t)(bank - 0x40) << 16) | lo;
    if (bwram_write_allowed(sa1, offset)) bwram_write_raw(sa1, offset, data);
  } else if (bank >= 0x60 && bank < 0x70) {
    sa1->extra_cycles++;
    bwram_write_bitmap(sa1, ((uint32_t)(bank - 0x60) << 16) | lo, data);
  } else if ((address & 0x408000u) == 0x008000u ||
             (address & 0xc00000u) == 0xc00000u) {
    if (cpu_bus_owns_rom(sa1)) sa1->extra_cycles++;
  }
}

static void sa1_request_interrupt(Sa1 *sa1) {
  sa1->interrupt_vector_kind = 0;
  if (sa1->io.sa1_nmi_enable && sa1->io.sa1_nmi_flag) {
    sa1->interrupt_vector_kind = 1;
    sa1->cpu->nmiWanted = true;
    return;
  }
  bool irq = (sa1->io.timer_irq_enable && sa1->io.timer_irq_flag)
          || (sa1->io.dma_irq_enable && sa1->io.dma_irq_flag)
          || (sa1->io.sa1_irq_enable && sa1->io.sa1_irq_flag);
  if (irq) {
    sa1->interrupt_vector_kind = 2;
    sa1->cpu->irqWanted = true;
  }
}

Sa1 *sa1_create(uint8_t *rom, uint32_t rom_size,
                uint8_t *bwram, uint32_t bwram_size) {
  Sa1 *sa1 = (Sa1 *)calloc(1, sizeof(*sa1));
  if (!sa1) return NULL;
  sa1->rom = rom;
  sa1->rom_size = rom_size;
  sa1->bwram = bwram;
  sa1->bwram_size = bwram_size;
  sa1->cpu = interp816_init(sa1, sa1_bus_read, sa1_bus_write);
  if (!sa1->cpu) {
    free(sa1);
    return NULL;
  }
  interp816_set_brk_hook_enabled(sa1->cpu, false);
  sa1_reset(sa1);
  return sa1;
}

void sa1_destroy(Sa1 *sa1) {
  if (!sa1) return;
  interp816_free(sa1->cpu);
  free(sa1);
}

void sa1_reset(Sa1 *sa1) {
  if (!sa1) return;
  memset(&sa1->io, 0, sizeof(sa1->io));
  memset(sa1->iram, 0, sizeof(sa1->iram));
  interp816_reset(sa1->cpu);
  interp816_set_brk_hook_enabled(sa1->cpu, false);
  sa1->io.sa1_reset = true;
  sa1->io.bank_select[0] = 0;
  sa1->io.bank_select[1] = 1;
  sa1->io.bank_select[2] = 2;
  sa1->io.bank_select[3] = 3;
  sa1->io.bwram_protect = 0x0f;
  sa1->io.variable_length = 16;
  sa1->master_clock = 0;
  sa1->instructions = 0;
  sa1->h_counter = 0;
  sa1->v_counter = 0;
  sa1->char_dma_line = 0;
  sa1->bwram_char_dma = false;
  sa1->open_bus = 0xff;
  sa1->interrupt_vector_kind = 0;
  sa1->extra_cycles = 0;
  sa1->cpu_bus_address = 0;
}

void sa1_set_cpu_bus_address(Sa1 *sa1, uint32_t address) {
  if (sa1) sa1->cpu_bus_address = address & 0xffffffu;
}

bool sa1_set_observer(Sa1 *sa1, const uint32_t *addresses, size_t count,
                     Sa1Observer observer, void *context) {
  if (!sa1 || count > 4 || (observer && (!addresses || !count))) return false;
  for (size_t i = 0; observer && i < count; ++i)
    if (addresses[i] > 0xffffff) return false;
  if (observer) memcpy(sa1->observer_addresses, addresses, count * sizeof(*addresses));
  sa1->observer_count = observer ? count : 0;
  sa1->observer_context = observer ? context : NULL;
  sa1->observer = observer;
  return true;
}

static void observe_instruction(Sa1 *sa1) {
  const Interp816 *cpu = sa1->cpu;
  if (cpu->stopped || (cpu->waiting && !cpu->nmiWanted && !cpu->irqWanted) ||
      cpu->nmiWanted || (!cpu->i && cpu->irqWanted)) return;
  uint32_t pc = ((uint32_t)cpu->k << 16) | cpu->pc;
  for (size_t i = 0; i < sa1->observer_count; ++i) {
    if (pc != sa1->observer_addresses[i]) continue;
    Sa1Observation observation = {pc, sa1->master_clock, sa1->iram, sa1->bwram,
                                  sizeof(sa1->iram), sa1->bwram_size, cpu->x};
    sa1->observer(sa1->observer_context, &observation);
    break;
  }
}

void sa1_sync(Sa1 *sa1, uint64_t master_clock) {
  if (!sa1 || master_clock <= sa1->master_clock) return;
  while (sa1->master_clock < master_clock) {
    if (sa1->io.sa1_reset || sa1->io.sa1_ready) {
      sa1->master_clock += SA1_MASTER_CLOCKS_PER_CPU_CYCLE;
      timer_advance(sa1, SA1_MASTER_CLOCKS_PER_CPU_CYCLE);
      continue;
    }
    sa1_request_interrupt(sa1);
    sa1->extra_cycles = 0;
    if (sa1->observer) observe_instruction(sa1);
    uint32_t capture_pc = ((uint32_t)sa1->cpu->k << 16) | sa1->cpu->pc;
    uint8_t capture_mx = ((uint8_t)sa1->cpu->mf << 1) | (uint8_t)sa1->cpu->xf;
    uint8_t capture_e = sa1->cpu->e;
    int cycles = interp816_runOpcode(sa1->cpu);
    if (cycles < 1) cycles = 1;
    if (g_tier2_capture_active)
      tier2_capture_cpu_instruction(1, capture_pc, capture_mx, capture_e, (unsigned)cycles);
    uint32_t clocks =
        ((uint32_t)cycles + sa1->extra_cycles) *
        SA1_MASTER_CLOCKS_PER_CPU_CYCLE;
    sa1->master_clock += clocks;
    timer_advance(sa1, clocks);
    sa1->instructions++;
    sa1->interrupt_vector_kind = 0;
  }
}

uint8_t *sa1_cpu_memory_ptr(Sa1 *sa1, uint8_t bank, uint16_t address) {
  if (!sa1) return NULL;
  uint8_t canonical = bank & 0x7f;
  if (canonical < 0x40 && address >= 0x3000 && address < 0x3800)
    return &sa1->iram[address & 0x07ff];
  if (canonical < 0x40 && address >= 0x6000 && address < 0x8000) {
    uint32_t offset = (uint32_t)sa1->io.snes_bwram_page * 0x2000u
                    + (address & 0x1fffu);
    return sa1->bwram && sa1->bwram_size
        ? &sa1->bwram[mirror_offset(offset, sa1->bwram_size)] : NULL;
  }
  if (bank >= 0x40 && bank < 0x50) {
    uint32_t offset = ((uint32_t)(bank - 0x40) << 16) | address;
    return sa1->bwram && sa1->bwram_size
        ? &sa1->bwram[mirror_offset(offset, sa1->bwram_size)] : NULL;
  }
  return rom_cpu_ptr(sa1, bank, address);
}

bool sa1_cpu_irq_pending(const Sa1 *sa1) {
  return sa1 &&
      ((sa1->io.cpu_irq_enable && sa1->io.cpu_irq_flag) ||
       (sa1->io.chdma_irq_enable && sa1->io.chdma_irq_flag));
}

uint64_t sa1_instructions_executed(const Sa1 *sa1) {
  return sa1 ? sa1->instructions : 0;
}

uint64_t sa1_master_clock(const Sa1 *sa1) {
  return sa1 ? sa1->master_clock : 0;
}

void sa1_saveload(Sa1 *sa1, SaveLoadInfo *sli) {
  if (!sa1 || !sli) return;
  interp816_saveload(sa1->cpu, sli);
  sli->func(sli, &sa1->io, sizeof(sa1->io));
  sli->func(sli, sa1->iram, sizeof(sa1->iram));
  sli->func(sli, &sa1->master_clock, sizeof(sa1->master_clock));
  sli->func(sli, &sa1->instructions, sizeof(sa1->instructions));
  sli->func(sli, &sa1->h_counter, sizeof(sa1->h_counter));
  sli->func(sli, &sa1->v_counter, sizeof(sa1->v_counter));
  sli->func(sli, &sa1->char_dma_line, sizeof(sa1->char_dma_line));
  sli->func(sli, &sa1->bwram_char_dma, sizeof(sa1->bwram_char_dma));
  sli->func(sli, &sa1->open_bus, sizeof(sa1->open_bus));
  sli->func(sli, &sa1->interrupt_vector_kind,
            sizeof(sa1->interrupt_vector_kind));
  sli->func(sli, &sa1->extra_cycles, sizeof(sa1->extra_cycles));
  sli->func(sli, &sa1->cpu_bus_address, sizeof(sa1->cpu_bus_address));
}
