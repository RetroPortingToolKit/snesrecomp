/* Dependency-light core of the two-peer region agreement check. The barrier
 * fixture (region_agreement_test.c) drives the real host admit path when the
 * recomp-net headers are available; this one runs anywhere a C compiler does
 * and pins the fold, the verdict and the player-facing copy. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "netplay/snes_netplay_region.h"

#define SAME_ROM_FP 0x5a17c0deu

static int has(const char *text, const char *needle) {
  return text && strstr(text, needle) != NULL;
}

int main(void) {
  SnesNetplayRegionState a, b;

  snes_netplay_region_state_init(&a);
  snes_netplay_region_state_init(&b);
  snes_netplay_region_set_local(&a, 1, kSnesRegionSourceOverride, SAME_ROM_FP);
  snes_netplay_region_set_local(&b, 0, kSnesRegionSourceHeader, SAME_ROM_FP);

  assert(snes_netplay_region_fold_content_fp(SAME_ROM_FP, 1) !=
         snes_netplay_region_fold_content_fp(SAME_ROM_FP, 0));
  assert(snes_netplay_region_folded_fp(&a) !=
         snes_netplay_region_folded_fp(&b));
  /* An override to the header's own region is not a different identity. */
  assert(snes_netplay_region_fold_content_fp(SAME_ROM_FP, 1) ==
         snes_netplay_region_fold_content_fp(SAME_ROM_FP, 1));

  snes_netplay_region_set_peer(&a, snes_netplay_region_folded_fp(&b),
                               b.source);
  snes_netplay_region_set_peer(&b, snes_netplay_region_folded_fp(&a),
                               a.source);
  assert(!snes_netplay_region_check(&a));
  assert(!snes_netplay_region_check(&b));
  assert(snes_netplay_region_refused(&a) && snes_netplay_region_refused(&b));
  assert(!strcmp(snes_netplay_region_reason(&a), "region_mismatch"));
  assert(!strcmp(snes_netplay_region_reason(&b), "region_mismatch"));
  assert(has(snes_netplay_region_message(&a), "PAL (from override)"));
  assert(has(snes_netplay_region_message(&a), "NTSC (from header)"));
  assert(has(snes_netplay_region_message(&b), "PAL (from override)"));
  assert(has(snes_netplay_region_message(&b), "NTSC (from header)"));
  printf("%s\n", snes_netplay_region_message(&a));

  /* Same ROM, same region, different source: admitted. */
  snes_netplay_region_set_local(&b, 1, kSnesRegionSourceHeader, SAME_ROM_FP);
  snes_netplay_region_set_peer(&a, snes_netplay_region_folded_fp(&b),
                               b.source);
  snes_netplay_region_set_peer(&b, snes_netplay_region_folded_fp(&a),
                               a.source);
  assert(snes_netplay_region_check(&a));
  assert(snes_netplay_region_check(&b));
  assert(!snes_netplay_region_refused(&a));
  assert(!snes_netplay_region_refused(&b));
  assert(snes_netplay_region_reason(&a) == NULL);

  /* Env source is named too. */
  snes_netplay_region_set_local(&a, 1, kSnesRegionSourceEnv, SAME_ROM_FP);
  snes_netplay_region_set_local(&b, 0, kSnesRegionSourceHeader, SAME_ROM_FP);
  snes_netplay_region_set_peer(&a, snes_netplay_region_folded_fp(&b),
                               b.source);
  assert(!snes_netplay_region_check(&a));
  assert(has(snes_netplay_region_message(&a), "PAL (from SNESRECOMP_REGION)"));

  puts("region agreement core: PASS");
  return 0;
}
