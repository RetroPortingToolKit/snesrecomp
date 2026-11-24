/* See headless_capture.h. Pure C, no SDL: the host wires it in. */
#include "headless_capture.h"

#include "../host_args.h"

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define HC_MKDIR(p) _mkdir(p)
#define HC_SETENV(k, v) _putenv_s((k), (v))
#else
#include <sys/stat.h>
#include <sys/types.h>
#define HC_MKDIR(p) mkdir((p), 0755)
#define HC_SETENV(k, v) setenv((k), (v), 1)
#endif

#define HC_SIM_HZ 60.0988
#define HC_AUDIO_RATE 48000

static struct {
  bool enabled;
  bool failed;
  char out_dir[1024];
  int width, height;
  long from, to;
  int every;
  long exit_frame;
  FILE *wav;
  uint64_t wav_frames;
  uint64_t png_count;
  uint8_t *canvas;     /* RGB24 */
  uint8_t *png_buf;
  size_t png_cap;
  /* Input schedule. */
  struct Step { long frame; int pad; uint16_t buttons; } *steps;
  int step_count;
  int next_step;
  uint16_t held[4];
} g_hc;

/* ---- helpers ---- */

static uint32_t hc_crc_table[256];
static int hc_crc_ready;
static uint32_t hc_crc(uint32_t crc, const uint8_t *p, size_t n) {
  if (!hc_crc_ready) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int j = 0; j < 8; j++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      hc_crc_table[i] = c;
    }
    hc_crc_ready = 1;
  }
  crc = ~crc;
  while (n--) crc = hc_crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

static void put32be(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void write_chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
  uint8_t head[8];
  put32be(head, (uint32_t)n);
  memcpy(head + 4, type, 4);
  fwrite(head, 1, 8, f);
  uint32_t crc = hc_crc(0, (const uint8_t *)type, 4);
  if (n) {
    fwrite(data, 1, n, f);
    crc = hc_crc(crc, data, n);
  }
  uint8_t tail[4];
  put32be(tail, crc);
  fwrite(tail, 1, 4, f);
}

/* RGB24 -> PNG using stored (uncompressed) deflate blocks: no zlib needed, and
 * a frame is written in one pass. Files are large; they are an intermediate
 * that tools/frames_to_raw.py packs into a lossless video. */
static bool write_png(const char *path, const uint8_t *rgb, int w, int h) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  fwrite(sig, 1, 8, f);
  uint8_t ihdr[13];
  put32be(ihdr, (uint32_t)w);
  put32be(ihdr + 4, (uint32_t)h);
  ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  write_chunk(f, "IHDR", ihdr, 13);

  const size_t row = (size_t)w * 3 + 1;
  const size_t raw_len = row * (size_t)h;
  /* zlib stream: 2 header bytes, stored blocks, adler32. */
  const size_t blocks = (raw_len + 65534) / 65535;
  const size_t z_len = 2 + raw_len + blocks * 5 + 4;
  if (g_hc.png_cap < z_len) {
    uint8_t *nb = (uint8_t *)realloc(g_hc.png_buf, z_len);
    if (!nb) { fclose(f); return false; }
    g_hc.png_buf = nb;
    g_hc.png_cap = z_len;
  }
  uint8_t *z = g_hc.png_buf;
  size_t zo = 0;
  z[zo++] = 0x78; z[zo++] = 0x01;
  uint32_t a = 1, b = 0;
  size_t done = 0;
  size_t in_block = 0;
  uint8_t *block_hdr = NULL;
  for (int y = 0; y < h; y++) {
    for (size_t x = 0; x <= (size_t)w * 3; x++) {
      if (in_block == 0) {
        size_t remain = raw_len - done;
        size_t take = remain > 65535 ? 65535 : remain;
        block_hdr = z + zo;
        block_hdr[0] = (done + take == raw_len) ? 1 : 0;
        block_hdr[1] = (uint8_t)(take & 0xFF);
        block_hdr[2] = (uint8_t)(take >> 8);
        block_hdr[3] = (uint8_t)(~take & 0xFF);
        block_hdr[4] = (uint8_t)((~take >> 8) & 0xFF);
        zo += 5;
        in_block = take;
      }
      uint8_t v = (x == 0) ? 0 : rgb[(size_t)y * (size_t)w * 3 + (x - 1)];
      z[zo++] = v;
      a = (a + v) % 65521u;
      b = (b + a) % 65521u;
      in_block--;
      done++;
    }
  }
  uint8_t ad[4];
  put32be(ad, (b << 16) | a);
  memcpy(z + zo, ad, 4);
  zo += 4;
  write_chunk(f, "IDAT", z, zo);
  write_chunk(f, "IEND", NULL, 0);
  const bool wrote = !ferror(f);
  return fclose(f) == 0 && wrote;
}

/* ---- WAV ---- */

static bool wav_header(FILE *f, uint32_t data_bytes) {
  uint8_t h[44];
  memcpy(h, "RIFF", 4);
  uint32_t v = 36 + data_bytes;
  h[4] = v & 0xFF; h[5] = (v >> 8) & 0xFF; h[6] = (v >> 16) & 0xFF; h[7] = (v >> 24) & 0xFF;
  memcpy(h + 8, "WAVEfmt ", 8);
  uint32_t fmt_len = 16;
  memcpy(h + 16, &fmt_len, 4);
  uint16_t pcm = 1, ch = 2, bits = 16, align = 4;
  uint32_t rate = HC_AUDIO_RATE, bps = HC_AUDIO_RATE * 4;
  memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2);
  memcpy(h + 24, &rate, 4); memcpy(h + 28, &bps, 4);
  memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4);
  memcpy(h + 40, &data_bytes, 4);
  return fseek(f, 0, SEEK_SET) == 0 && fwrite(h, 1, sizeof(h), f) == sizeof(h);
}

/* ---- input schedule ---- */

static uint16_t parse_buttons(const char *s) {
  static const struct { const char *name; uint16_t bit; } kNames[] = {
    {"B", 0x001}, {"Y", 0x002}, {"SELECT", 0x004}, {"START", 0x008},
    {"UP", 0x010}, {"DOWN", 0x020}, {"LEFT", 0x040}, {"RIGHT", 0x080},
    {"A", 0x100}, {"X", 0x200}, {"L", 0x400}, {"R", 0x800},
  };
  uint16_t m = 0;
  while (*s) {
    while (*s == '+' || *s == ' ' || *s == '\t') s++;
    size_t n = 0;
    while (s[n] && s[n] != '+' && s[n] != ' ' && s[n] != '\t' && s[n] != '\r' &&
           s[n] != '\n') n++;
    if (!n) break;
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
      size_t l = strlen(kNames[i].name);
      if (l == n) {
        size_t k = 0;
        while (k < n && toupper((unsigned char)s[k]) == kNames[i].name[k]) k++;
        if (k == n) { m |= kNames[i].bit; break; }
      }
    }
    s += n;
  }
  return m;
}

static int cmp_step(const void *x, const void *y) {
  const struct Step *a = (const struct Step *)x, *b = (const struct Step *)y;
  if (a->frame != b->frame) return a->frame < b->frame ? -1 : 1;
  return a->pad - b->pad;
}

static bool load_input(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  int cap = 64;
  g_hc.steps = (struct Step *)malloc((size_t)cap * sizeof(struct Step));
  if (!g_hc.steps) {
    fclose(f);
    return false;
  }
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    char *c = strchr(line, '#');
    if (c) *c = 0;
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '\n' || *p == '\r') continue;
    char *end;
    long frame = strtol(p, &end, 10);
    if (end == p) continue;
    p = end;
    while (*p == ' ' || *p == '\t') p++;
    int pad = 0;
    if (isdigit((unsigned char)p[0]) && p[1] == ':') {
      pad = p[0] - '1';
      if (pad < 0 || pad > 3) continue;
      p += 2;
    }
    while (*p == ' ' || *p == '\t') p++;
    uint16_t b = (*p == '-') ? 0 : parse_buttons(p);
    if (g_hc.step_count == cap) {
      cap *= 2;
      struct Step *grown = (struct Step *)realloc(g_hc.steps, (size_t)cap * sizeof(struct Step));
      if (!grown) {
        fclose(f);
        return false;
      }
      g_hc.steps = grown;
    }
    g_hc.steps[g_hc.step_count++] = (struct Step){frame, pad, b};
  }
  const bool read = !ferror(f);
  if (fclose(f) != 0 || !read) return false;
  qsort(g_hc.steps, (size_t)g_hc.step_count, sizeof(struct Step), cmp_step);
  return true;
}

/* ---- public ---- */

bool HeadlessCapture_Configure(const struct SnesrecompHostArgs *args) {
  const char *out = args ? args->capture_out : NULL;
  const char *wav = args ? args->capture_wav : NULL;
  const char *in = args ? args->input_script : NULL;
  const char *env_out = getenv("SNESRECOMP_CAPTURE_OUT");
  const char *env_wav = getenv("SNESRECOMP_CAPTURE_WAV");
  if (!out && env_out && *env_out) out = env_out;
  if (!wav && env_wav && *env_wav) wav = env_wav;
  g_hc.width = args && args->capture_width ? args->capture_width : 1920;
  g_hc.height = args && args->capture_height ? args->capture_height : 1080;
  g_hc.from = args ? args->capture_from : -1;
  g_hc.to = args ? args->capture_to : -1;
  g_hc.every = args && args->capture_every > 0 ? args->capture_every : 1;
  g_hc.exit_frame = args ? args->exit_at_frame : -1;
  if (in && !load_input(in)) {
    fprintf(stderr, "capture: could not read input script '%s'\n", in);
    g_hc.failed = true;
    return false;
  }
  if (!out && !wav) {
    /* Input scripting and --exit-at-frame alone do not imply capture mode. */
    return false;
  }
  g_hc.enabled = true;
  if (out) {
    if (strlen(out) >= sizeof(g_hc.out_dir) ||
        (HC_MKDIR(out) != 0 && errno != EEXIST)) {
      fprintf(stderr, "capture: cannot create frame directory '%s'\n", out);
      g_hc.failed = true;
      return true;
    }
    memcpy(g_hc.out_dir, out, strlen(out) + 1);
    g_hc.canvas = (uint8_t *)malloc((size_t)g_hc.width * (size_t)g_hc.height * 3);
    if (!g_hc.canvas) {
      fprintf(stderr, "capture: cannot allocate %dx%d frame canvas\n",
              g_hc.width, g_hc.height);
      g_hc.failed = true;
      return true;
    }
  }
  if (wav) {
    g_hc.wav = fopen(wav, "wb");
    if (!g_hc.wav || !wav_header(g_hc.wav, 0)) {
      fprintf(stderr, "capture: cannot start WAV '%s'\n", wav);
      g_hc.failed = true;
      return true;
    }
  }
  return true;
}

bool HeadlessCapture_Enabled(void) { return g_hc.enabled; }
bool HeadlessCapture_Failed(void) { return g_hc.failed; }

void HeadlessCapture_SelectVideoDriver(bool opengl) {
  if (!opengl) {
    HC_SETENV("SDL_VIDEODRIVER", "dummy");
  }
#if defined(__linux__) && !defined(__ANDROID__)
  else if (!getenv("SDL_VIDEODRIVER") && !getenv("DISPLAY") &&
           !getenv("WAYLAND_DISPLAY")) {
    HC_SETENV("SDL_VIDEODRIVER", "offscreen");
  }
#endif
}

void HeadlessCapture_OutputSize(int *width, int *height) {
  if (width) *width = g_hc.width;
  if (height) *height = g_hc.height;
}

int HeadlessCapture_AudioRate(void) { return HC_AUDIO_RATE; }
bool HeadlessCapture_AudioEnabled(void) { return g_hc.enabled; }

static bool frame_in_range(uint32_t frame) {
  if (g_hc.from >= 0 && (long)frame < g_hc.from) return false;
  if (g_hc.to >= 0 && (long)frame > g_hc.to) return false;
  return true;
}

bool HeadlessCapture_FrameWanted(uint32_t frame) {
  const long base = g_hc.from >= 0 ? g_hc.from : 1;
  return g_hc.canvas && frame_in_range(frame) && ((long)frame - base) % g_hc.every == 0;
}

void HeadlessCapture_Present(const uint8_t *bgra, int pitch, int w, int h,
                             int vx, int vy, int vw, int vh, uint32_t frame) {
  if (!HeadlessCapture_FrameWanted(frame)) return;
  if (!bgra || w <= 0 || h <= 0) {
    fprintf(stderr, "capture: no presented pixels for frame %u\n", frame);
    g_hc.failed = true;
    return;
  }
  const int W = g_hc.width, H = g_hc.height;
  memset(g_hc.canvas, 0, (size_t)W * (size_t)H * 3);
  if (vw <= 0 || vh <= 0) { vx = 0; vy = 0; vw = W; vh = H; }
  for (int y = 0; y < vh; y++) {
    int oy = vy + y;
    if (oy < 0 || oy >= H) continue;
    int sy = (int)(((int64_t)y * h) / vh);
    const uint32_t *srow = (const uint32_t *)(bgra + (ptrdiff_t)sy * pitch);
    uint8_t *drow = g_hc.canvas + (size_t)oy * (size_t)W * 3;
    for (int x = 0; x < vw; x++) {
      int ox = vx + x;
      if (ox < 0 || ox >= W) continue;
      uint32_t p = srow[(int)(((int64_t)x * w) / vw)];
      drow[ox * 3 + 0] = (uint8_t)(p >> 16);
      drow[ox * 3 + 1] = (uint8_t)(p >> 8);
      drow[ox * 3 + 2] = (uint8_t)p;
    }
  }
  char path[1200];
  snprintf(path, sizeof(path), "%s/frame_%06u.png", g_hc.out_dir, frame);
  if (write_png(path, g_hc.canvas, W, H)) {
    g_hc.png_count++;
  } else {
    fprintf(stderr, "capture: failed to write frame '%s'\n", path);
    g_hc.failed = true;
  }
}

int HeadlessCapture_AudioFramesFor(uint32_t frame) {
  int64_t b = (int64_t)((double)HC_AUDIO_RATE * (double)frame / HC_SIM_HZ + 0.5);
  int64_t a = (int64_t)((double)HC_AUDIO_RATE * (double)(frame - 1) / HC_SIM_HZ + 0.5);
  return (int)(b - a);
}

void HeadlessCapture_Audio(const int16_t *samples, int frames, uint32_t frame) {
  if (!g_hc.wav || !frame_in_range(frame)) return;
  if (fwrite(samples, 4, (size_t)frames, g_hc.wav) != (size_t)frames) {
    fprintf(stderr, "capture: WAV sample write failed\n");
    g_hc.failed = true;
    return;
  }
  g_hc.wav_frames += (uint64_t)frames;
}

static void hc_advance(uint32_t frame) {
  while (g_hc.next_step < g_hc.step_count &&
         g_hc.steps[g_hc.next_step].frame <= (long)frame) {
    const struct Step *s = &g_hc.steps[g_hc.next_step++];
    g_hc.held[s->pad] = s->buttons;
  }
}

bool HeadlessCapture_HasInput(void) { return g_hc.step_count > 0; }

uint16_t HeadlessCapture_Pad(unsigned seat, uint32_t frame) {
  hc_advance(frame);
  return seat < 4 ? g_hc.held[seat] : 0;
}

uint32_t HeadlessCapture_InputWord(uint32_t frame) {
  hc_advance(frame);
  return (uint32_t)g_hc.held[0] | ((uint32_t)g_hc.held[1] << 12);
}

uint32_t HeadlessCapture_ExitFrame(void) {
  return g_hc.exit_frame > 0 ? (uint32_t)g_hc.exit_frame : 0;
}

void HeadlessCapture_Finish(void) {
  if (g_hc.wav) {
    const bool header_ok = wav_header(g_hc.wav, (uint32_t)(g_hc.wav_frames * 4));
    if (fclose(g_hc.wav) != 0 || !header_ok) {
      fprintf(stderr, "capture: WAV finalization failed\n");
      g_hc.failed = true;
    }
    g_hc.wav = NULL;
  }
  free(g_hc.steps);
  free(g_hc.canvas);
  free(g_hc.png_buf);
  g_hc.steps = NULL;
  g_hc.canvas = NULL;
  g_hc.png_buf = NULL;
  g_hc.png_cap = 0;
  if (g_hc.enabled)
    fprintf(stderr,
            "capture: %llu frames written, %llu audio samples, present size %dx%d\n",
            (unsigned long long)g_hc.png_count, (unsigned long long)g_hc.wav_frames,
            g_hc.width, g_hc.height);
}
