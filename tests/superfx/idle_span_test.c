/* Exact stopped-clock scheduling contract. Include the core so the scalar
 * oracle uses the same instruction/buffer semantics, but its original sync
 * loop is kept independently below. No ROM-derived input is needed. */
#include "../../runner/src/snes/superfx.c"

static void reference_sync(SuperFx *f, uint64_t master) {
  if (!f) return;
  if (master < f->master_clock) {
    f->master_clock = master; f->clock_credit = 0; return;
  }
  f->clock_credit += (int64_t)(master - f->master_clock);
  f->master_clock = master;
  unsigned guard = 0;
  while (f->clock_credit >= 6 && guard++ < 2000000) {
    run_one(f);
    if (f->presentation && f->presentation->pending)
      finish_presentation_replay(f);
    if (f->ws_replay_pending) finish_widescreen_replay(f);
  }
}

static uint32_t seed = 0x6fd4e913;
static uint32_t rng(void) {
  seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
  return seed;
}

static void compare(SuperFx *a, SuperFx *b, unsigned test) {
  SuperFx copy = *b;
  copy.ram = a->ram;
  copy.presentation = a->presentation;
  if (memcmp(a, &copy, sizeof(*a)) ||
      memcmp(a->ram, b->ram, a->ram_size)) {
    fprintf(stderr, "idle span mismatch case %u, credit %lld/%lld\n", test,
            (long long)a->clock_credit, (long long)b->clock_credit);
    exit(1);
  }
}

typedef struct CallbackContext {
  SuperFx *owner;
  unsigned calls;
  int64_t credit;
  uint8_t byte;
} CallbackContext;

static void complete(void *context, const SuperFx *result) {
  CallbackContext *c = context;
  if (!result) abort();
  c->calls++;
  c->credit = c->owner->clock_credit;
  c->byte = c->owner->ram[13];
  /* A host callback may start another task. It must run before idle drain. */
  c->owner->sfr |= SFR_G;
  c->owner->pipeline = 0; /* STOP, followed by normal halted clocks. */
}

int main(void) {
  superfx_sync(NULL, 123);
  uint8_t rom[65536], ram_a[65536], ram_b[65536];
  for (size_t i = 0; i < sizeof(rom); i++) rom[i] = (uint8_t)rng();
  SuperFx *a = superfx_create(rom, sizeof(rom), ram_a, sizeof(ram_a));
  SuperFx *b = superfx_create(rom, sizeof(rom), ram_b, sizeof(ram_b));
  if (!a || !b) abort();
  unsigned cases = 0;
  for (unsigned i = 0; i < 24000; i++) {
    superfx_reset(a); superfx_reset(b);
    uint8_t *begin = (uint8_t *)&a->r;
    size_t length = offsetof(SuperFx, instruction_count) - offsetof(SuperFx, r);
    for (size_t j = 0; j < length; j++) begin[j] = (uint8_t)rng();
    /* Bool storage and inactive pipeline fields need not be random: only the
     * architectural halt, buffers, credit and memory are consumed here. */
    a->sfr &= (uint16_t)~SFR_G;
    a->romcl = i % 25;
    a->ramcl = (i / 25) % 25;
    a->clock_credit = (int)(rng() % 200) - 100;
    a->master_clock = 10000;
    for (size_t j = 0; j < sizeof(ram_a); j++) ram_a[j] = (uint8_t)(j + i);
    memcpy(ram_b, ram_a, sizeof(ram_a));
    *b = *a; b->ram = ram_b;
    uint64_t master = i % 31 == 0 ? 9999 : 10000 + rng() % 2048;
    reference_sync(a, master); superfx_sync(b, master);
    compare(a, b, cases++);
    /* Exercise a continuation and already-drained sync at the same clock. */
    reference_sync(a, master + 173); superfx_sync(b, master + 173);
    compare(a, b, cases++);
  }
  const int64_t credits[] = { -7, -1, 0, 1, 5, 6, 7, 11, 12,
      11999993, 11999999, 12000000, 12000001, 12000005, 12000006,
      24000000, INT64_MAX - 100 };
  for (size_t i = 0; i < sizeof(credits) / sizeof(*credits); i++) {
    for (unsigned pending = 0; pending < 4; pending++) {
      superfx_reset(a); superfx_reset(b);
      a->clock_credit = credits[i]; a->master_clock = 100;
      a->romcl = pending & 1 ? 13 : 0;
      a->ramcl = pending & 2 ? 19 : 0;
      a->sfr |= SFR_R | SFR_IRQ; a->irq_pending = true;
      a->ramar = 13; a->ramdr = 0xa7;
      memset(ram_a, 0x3d, sizeof(ram_a)); memcpy(ram_b, ram_a, sizeof(ram_a));
      *b = *a; b->ram = ram_b;
      reference_sync(a, 100); superfx_sync(b, 100);
      compare(a, b, cases++);
      unsigned ah = superfx_cpu_read_io(a, 0x3031);
      unsigned bh = superfx_cpu_read_io(b, 0x3031);
      if (ah != bh || a->irq_pending || b->irq_pending) abort();
      compare(a, b, cases++);
    }
  }
  for (unsigned credit = 0; credit < 32; credit++) {
    superfx_reset(a); superfx_reset(b);
    a->clock_credit = credit; a->ramcl = 6; a->ramar = 13; a->ramdr = 0xc9;
    memset(ram_a, 0, sizeof(ram_a)); memcpy(ram_b, ram_a, sizeof(ram_a));
    *b = *a; b->ram = ram_b;
    CallbackContext ac = {a, 0, 0, 0}, bc = {b, 0, 0, 0};
    SuperFxPresentationReplay ap = {0}, bp = {0};
    ap.clone = *a; bp.clone = *b;
    ap.pending = bp.pending = true;
    ap.complete = bp.complete = complete;
    ap.context = &ac; bp.context = &bc;
    a->presentation = &ap; b->presentation = &bp;
    reference_sync(a, 0); superfx_sync(b, 0);
    if (ac.calls != bc.calls || ac.credit != bc.credit || ac.byte != bc.byte ||
        ap.pending != bp.pending) abort();
    compare(a, b, cases++);
    a->presentation = b->presentation = NULL;
  }
  superfx_destroy(a); superfx_destroy(b);
  printf("Super FX stopped-clock differential: %u cases PASS\n", cases);
  return 0;
}
