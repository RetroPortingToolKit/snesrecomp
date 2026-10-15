/*
 * config_roundtrip_test.c — config.ini must give back what the launcher put in.
 *
 * Every setting the pre-boot launcher edits is applied to g_config and then
 * written by WriteConfigFile. When a key is missing from that writer the value
 * still LOOKS applied for the rest of the session and silently reverts on the
 * next launch, which is indistinguishable from "the launcher forgets
 * everything". Fullscreen spent that whole life unwritten; so did the
 * per-player input sources, and VSync could not even represent Adaptive.
 *
 * These are round-trip assertions, not writer assertions: parse a file, change
 * the value the way the host does, write, parse again, compare. A future key
 * added to the launcher and forgotten in WriteConfigFile fails here.
 */
#include "config.h"
#include "display_aspect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The host supplies Die(); the config code only calls it on allocation
 * failure and on a kvs lookup that names a key the table does not have --
 * both of which this harness wants to see as a hard, visible stop. */
void Die(const char *error) {
  fprintf(stderr, "FAIL: Die(%s)\n", error ? error : "(null)");
  exit(1);
}

static int g_failures;

#define CHECK(cond, ...)                                                      \
  do {                                                                        \
    if (!(cond)) {                                                            \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                    \
      fprintf(stderr, __VA_ARGS__);                                           \
      fprintf(stderr, "\n");                                                  \
      g_failures++;                                                           \
    }                                                                         \
  } while (0)

static const char *kPath = "config_roundtrip_test.ini";

static void write_file(const char *body) {
  FILE *f = fopen(kPath, "wb");
  if (!f) {
    fprintf(stderr, "FAIL: cannot create %s\n", kPath);
    exit(1);
  }
  fputs(body, f);
  fclose(f);
}

static char *read_file(void) {
  FILE *f = fopen(kPath, "rb");
  static char buf[16384];
  size_t n = 0;
  if (!f) return NULL;
  n = fread(buf, 1, sizeof(buf) - 1, f);
  buf[n] = '\0';
  fclose(f);
  return buf;
}

/* The shape a shipped config.ini has: sections, comments, and keys this test
 * does not touch, all of which must survive a rewrite. */
static const char *kBaseIni =
    "# snesrecomp config\n"
    "[General]\n"
    "SkipLauncher = 0\n"
    "Autosave = 1\n"            /* never written; must be preserved verbatim */
    "\n"
    "[Graphics]\n"
    "WindowScale = 3\n"
    "Fullscreen = 0\n"
    "# a comment inside the section\n"
    "IgnoreAspectRatio = 0\n"
    "VSync = 1\n"
    "\n"
    "[Sound]\n"
    "Volume = 100\n";

static void test_fullscreen_roundtrip(void) {
  write_file(kBaseIni);
  ParseConfigFile(kPath);
  CHECK(g_config.fullscreen == 0, "seeded fullscreen: got %d", g_config.fullscreen);

  /* What the host does with the launcher's Display row. */
  g_config.fullscreen = 1;
  WriteConfigFile(kPath);

  g_config.fullscreen = 0xEE;   /* poison: a stale value must not survive */
  ParseConfigFile(kPath);
  CHECK(g_config.fullscreen == 1,
        "borderless fullscreen did not survive the rewrite: got %d",
        g_config.fullscreen);

  /* Exclusive is a distinct third value, not a bool. */
  g_config.fullscreen = 2;
  WriteConfigFile(kPath);
  g_config.fullscreen = 0xEE;
  ParseConfigFile(kPath);
  CHECK(g_config.fullscreen == 2, "exclusive fullscreen: got %d",
        g_config.fullscreen);

  /* The rest of the file is still there. */
  const char *out = read_file();
  CHECK(out && strstr(out, "Autosave = 1"), "unwritten key was dropped");
  CHECK(out && strstr(out, "# a comment inside the section"),
        "comment was dropped");
}

static void test_player_sources_roundtrip(void) {
  write_file(kBaseIni);
  ParseConfigFile(kPath);
  /* No [Controller] section: the host must be told so it can fall back to the
   * older EnableGamepadN spelling instead of to the seeded default. */
  CHECK(!ConfigHasPlayerSource(0), "SourceP1 reported present in a file without it");
  CHECK(!ConfigHasPlayerSource(1), "SourceP2 reported present in a file without it");

  /* Player 1 on a gamepad, player 2 on the keyboard -- the assignment
   * EnableGamepad1/2 cannot express, and the one two people sharing one
   * keyboard-and-one-pad actually use. */
  g_config.player_src[0] = 2;
  g_config.player_src[1] = 1;
  WriteConfigFile(kPath);

  g_config.player_src[0] = g_config.player_src[1] = 0xEE;
  ParseConfigFile(kPath);
  CHECK(ConfigHasPlayerSource(0) && ConfigHasPlayerSource(1),
        "SourceP1/SourceP2 were not written");
  CHECK(g_config.player_src[0] == 2, "player 1 source: got %d", g_config.player_src[0]);
  CHECK(g_config.player_src[1] == 1, "player 2 source: got %d (keyboard is 1)",
        g_config.player_src[1]);

  /* "Player 1: None" is also representable and must not read back as keyboard. */
  g_config.player_src[0] = 0;
  WriteConfigFile(kPath);
  g_config.player_src[0] = 0xEE;
  ParseConfigFile(kPath);
  CHECK(g_config.player_src[0] == 0, "player 1 none: got %d", g_config.player_src[0]);
}

static void test_vsync_tristate(void) {
  write_file(kBaseIni);
  ParseConfigFile(kPath);
  CHECK(g_config.vsync == kSnesVSync_On, "legacy VSync = 1: got %d", g_config.vsync);

  g_config.vsync = kSnesVSync_Adaptive;
  WriteConfigFile(kPath);
  g_config.vsync = 0xEE;
  ParseConfigFile(kPath);
  CHECK(g_config.vsync == kSnesVSync_Adaptive,
        "Adaptive collapsed on the round trip: got %d", g_config.vsync);

  g_config.vsync = kSnesVSync_Off;
  WriteConfigFile(kPath);
  g_config.vsync = 0xEE;
  ParseConfigFile(kPath);
  CHECK(g_config.vsync == kSnesVSync_Off, "Off: got %d", g_config.vsync);

  /* Every boolean spelling an existing config.ini may hold still means what
   * it meant before VSync grew a third value. */
  static const struct { const char *text; uint8 want; } kLegacy[] = {
    { "0", kSnesVSync_Off },      { "1", kSnesVSync_On },
    { "false", kSnesVSync_Off },  { "true", kSnesVSync_On },
    { "off", kSnesVSync_Off },    { "on", kSnesVSync_On },
    { "no", kSnesVSync_Off },     { "yes", kSnesVSync_On },
    { "adaptive", kSnesVSync_Adaptive },
    { "2", kSnesVSync_Adaptive },
  };
  for (size_t i = 0; i < sizeof(kLegacy) / sizeof(kLegacy[0]); i++) {
    char body[512];
    snprintf(body, sizeof(body), "[Graphics]\nVSync = %s\n", kLegacy[i].text);
    write_file(body);
    g_config.vsync = 0xEE;
    ParseConfigFile(kPath);
    CHECK(g_config.vsync == kLegacy[i].want, "VSync = %s: got %d, want %d",
          kLegacy[i].text, g_config.vsync, kLegacy[i].want);
  }
}

/* A config.ini with none of these sections must still come back complete:
 * WriteConfigFile appends what it owns rather than losing it. */
static void test_written_from_nothing(void) {
  write_file("# empty\n");
  ParseConfigFile(kPath);
  g_config.fullscreen = 1;
  g_config.player_src[0] = 1;
  g_config.player_src[1] = 2;
  g_config.vsync = kSnesVSync_Adaptive;
  WriteConfigFile(kPath);

  g_config.fullscreen = 0xEE;
  g_config.player_src[0] = g_config.player_src[1] = 0xEE;
  g_config.vsync = 0xEE;
  ParseConfigFile(kPath);
  CHECK(g_config.fullscreen == 1, "fullscreen from a bare file: got %d",
        g_config.fullscreen);
  CHECK(g_config.player_src[0] == 1 && g_config.player_src[1] == 2,
        "sources from a bare file: got %d/%d", g_config.player_src[0],
        g_config.player_src[1]);
  CHECK(g_config.vsync == kSnesVSync_Adaptive, "vsync from a bare file: got %d",
        g_config.vsync);
}

/* [Rewind] is opt-in per host. A host that does not offer the launcher rows
 * must leave the file exactly as it found it -- that is the promise that lets
 * the keys be added to the shared writer without touching any existing port. */
static void test_rewind_opt_in(void) {
  /* Not opted in: a file with no rewind keys does not grow them... */
  write_file(kBaseIni);
  ParseConfigFile(kPath);
  WriteConfigFile(kPath);
  const char *out = read_file();
  CHECK(out && !strstr(out, "[Rewind]"),
        "a host that did not opt in still wrote a [Rewind] section");

  /* ...and a file that already has them keeps them byte for byte, including
   * a value this build would never have chosen. */
  write_file("[Rewind]\nEnabled = 1\nDepth = 137\nInterval = 9\n");
  ParseConfigFile(kPath);
  WriteConfigFile(kPath);
  out = read_file();
  CHECK(out && strstr(out, "Depth = 137"),
        "an existing rewind value was rewritten by a host that did not opt in");

  /* Opted in: the three rows round-trip like any other setting. */
  ConfigEnableRewindKeys();
  write_file(kBaseIni);
  ParseConfigFile(kPath);
  g_config.rewind_enabled = true;
  g_config.rewind_depth = 120;
  g_config.rewind_interval = 4;
  WriteConfigFile(kPath);

  g_config.rewind_enabled = false;
  g_config.rewind_depth = g_config.rewind_interval = -1;
  ParseConfigFile(kPath);
  CHECK(g_config.rewind_enabled, "rewind enable did not survive");
  CHECK(g_config.rewind_depth == 120, "rewind depth: got %d", g_config.rewind_depth);
  CHECK(g_config.rewind_interval == 4, "rewind interval: got %d",
        g_config.rewind_interval);

  /* Off is a real value, not an absent one. */
  g_config.rewind_enabled = false;
  WriteConfigFile(kPath);
  g_config.rewind_enabled = true;
  ParseConfigFile(kPath);
  CHECK(!g_config.rewind_enabled, "rewind off did not survive");
}

static void test_video_settings_roundtrip(void) {
  write_file("[Graphics]\nDisplayAspect = SquarePixels\nShader = assets/shaders/crt-soft.glslp\n");
  ParseConfigFile(kPath);
  CHECK(g_config.display_aspect == kSnesDisplayAspect_SquarePixels8x7,
        "pixel aspect alias not parsed");
  CHECK(g_config.shader && !strcmp(g_config.shader, "assets/shaders/crt-soft.glslp"),
        "shader preset not parsed");
  g_config.display_aspect = kSnesDisplayAspect_SquareFrame1x1;
  g_config.shader = "assets/shaders/warm-composite.glslp";
  WriteConfigFile(kPath);
  ParseConfigFile(kPath);
  CHECK(g_config.display_aspect == kSnesDisplayAspect_SquareFrame1x1,
        "pixel aspect did not survive");
  CHECK(g_config.shader && !strcmp(g_config.shader, "assets/shaders/warm-composite.glslp"),
        "shader preset did not survive");
  g_config.shader = NULL;
  WriteConfigFile(kPath);
  ParseConfigFile(kPath);
  CHECK(!g_config.shader, "cleared shader did not survive");
}

/* Multitap seats (players 3..). A two-seat host must not grow keys it does not
 * route; a five-seat host round-trips source, gamepad claim and a per-seat
 * [GamepadMap] ControlsPn line. */
static void test_multitap_seats_roundtrip(void) {
  ConfigSetPadSeats(2);
  write_file(kBaseIni);
  ParseConfigFile(kPath);
  CHECK(g_config.player_src[2] == 0 && !g_config.enable_gamepad[2],
        "seat 3 must start with no device");
  WriteConfigFile(kPath);
  char *out = read_file();
  CHECK(out && !strstr(out, "SourceP3") && !strstr(out, "EnableGamepad3"),
        "a two-seat host wrote multitap keys");

  ConfigSetPadSeats(5);
  g_config.player_src[2] = 1;  /* keyboard */
  g_config.player_src[3] = 2;  /* gamepad */
  g_config.enable_gamepad[3] = true;
  g_config.player_src[4] = 0;
  WriteConfigFile(kPath);
  out = read_file();
  CHECK(out && strstr(out, "SourceP5") && !strstr(out, "SourceP6"),
        "five-seat host must write exactly seats 3..5");
  g_config.player_src[2] = g_config.player_src[3] = g_config.player_src[4] = 0xEE;
  g_config.enable_gamepad[3] = false;
  ParseConfigFile(kPath);
  CHECK(ConfigHasPlayerSource(2) && ConfigHasPlayerSource(4), "SourceP3/P5 not read back");
  CHECK(g_config.player_src[2] == 1, "seat 3 source: got %d", g_config.player_src[2]);
  CHECK(g_config.player_src[3] == 2 && g_config.enable_gamepad[3],
        "seat 4 gamepad: src %d enable %d", g_config.player_src[3], g_config.enable_gamepad[3]);
  CHECK(g_config.player_src[4] == 0, "seat 5 none: got %d", g_config.player_src[4]);

  /* Default pad layout for seat 3, and an explicit ControlsP4 line. */
  CHECK(FindCmdForGamepadButton(kGamepadBtn_Start + 2 * kGamepadBtn_Count, 0) ==
            ConfigSeatControlsBase(2) + 5,
        "seat 3 Start does not map to its own Start");
  write_file("[GamepadMap]\nControlsP4 = DpadUp, DpadDown, DpadLeft, DpadRight, Start, Back, B, A, Y, X, Lb, Rb\n");
  ConfigReloadGamepadMap(kPath);
  CHECK(FindCmdForGamepadButton(kGamepadBtn_Back + 3 * kGamepadBtn_Count, 0) ==
            ConfigSeatControlsBase(3) + 5,
        "ControlsP4 Start on Back was not applied to seat 4");
  CHECK(FindCmdForGamepadButton(kGamepadBtn_Start + 2 * kGamepadBtn_Count, 0) ==
            ConfigSeatControlsBase(2) + 5,
        "seat 3 lost its default layout when seat 4 was configured");
  CHECK(ConfigSeatForControl(ConfigSeatControlsBase(4) + 11) == 4,
        "seat 5 R does not belong to seat 5");
  ConfigSetPadSeats(2);
}

int main(void) {
  test_fullscreen_roundtrip();
  test_player_sources_roundtrip();
  test_vsync_tristate();
  test_written_from_nothing();
  test_video_settings_roundtrip();
  test_multitap_seats_roundtrip();
  /* Last: it flips the process-wide opt-in and never flips it back. */
  test_rewind_opt_in();
  remove(kPath);
  if (g_failures) {
    fprintf(stderr, "config round-trip: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("config round-trip: ok\n");
  return 0;
}
