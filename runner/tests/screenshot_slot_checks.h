#ifndef SNES_SCREENSHOT_SLOT_CHECKS_H
#define SNES_SCREENSHOT_SLOT_CHECKS_H
/* A deterministic picture provider isolates user-file/slot association from
 * animation; the packaged runtime separately qualifies its real compositor. */
static uint32_t slot_picture;
static bool snapshot_denied(void) { return false; }
static int slot_picture_compose(uint32_t *dst, size_t capacity, int *w, int *h) {
  if (!capacity) return 0;
  dst[0] = slot_picture; *w = *h = 1; return 1;
}
static void check_slot_picture(int slot, uint32_t expected) {
  char path[256], thumb[280]; uint32_t header[3], pixel;
  RtlSaveSlotPath(slot, path, sizeof(path));
  snprintf(thumb, sizeof(thumb), "%s.thumb", path);
  FILE *file = fopen(thumb, "rb"); check(file != NULL, "user save wrote its preview");
  check(fread(header, sizeof(header), 1, file) == 1 && header[0] == 0x42485453 &&
        header[1] == 128 && header[2] == 112, "preview carries the expected image format");
  bool equal = true;
  for (unsigned i = 0; i < 128u*112u; ++i)
    equal &= fread(&pixel, sizeof(pixel), 1, file) == 1 && pixel == expected;
  check(equal, "preview belongs to the selected slot and picture");
  fclose(file);
}

static void check_screenshot_slot_files(void) {
  snes_savestate_menu_set_compose(slot_picture_compose);
  uint64 preview_clock = g_cpu.master_cycles;
  slot_picture = 0xffff0000u; HandleCommand(kKeys_Save, true);
  check_slot_picture(0, slot_picture);
  slot_picture = 0xff0000ffu; HandleCommand(kKeys_Save + 1, true);
  check_slot_picture(1, slot_picture);
  slot_picture = 0xff00ff00u; HandleCommand(kKeys_Save, true);
  check_slot_picture(0, slot_picture);
  check_slot_picture(1, 0xff0000ffu);
  check(g_cpu.master_cycles == preview_clock, "paired file saves do not advance the guest");
  const RtlGameInfo *accepted_info = g_rtl_game_info;
  RtlGameInfo denied_info = *accepted_info;
  denied_info.snapshot_allowed = snapshot_denied;
  g_rtl_game_info = &denied_info;
  slot_picture = 0xffffffffu;
  check(!snes_savestate_menu_write_slot(0), "declined native save reports failure");
  g_rtl_game_info = accepted_info;
  check_slot_picture(0, 0xff00ff00u);
  HandleCommand(kKeys_Load, true);
  check_slot_picture(0, 0xff00ff00u); check_slot_picture(1, 0xff0000ffu);
  check(RtlSaveSnapshot("internal.sav"), "ordinary internal snapshot still saves");
  FILE *internal_preview = fopen("internal.sav.thumb", "rb");
  check(internal_preview == NULL, "internal snapshots create no preview work");
  remove("internal.sav");
  snes_savestate_menu_set_compose(NULL);
  for (int slot = 0; slot < 2; ++slot) {
    char path[256], thumb[280]; RtlSaveSlotPath(slot, path, sizeof(path));
    snprintf(thumb, sizeof(thumb), "%s.thumb", path); remove(thumb); remove(path);
  }
}
#endif
