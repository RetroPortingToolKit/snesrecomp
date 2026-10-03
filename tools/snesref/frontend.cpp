/* snesref — minimal SDL2 libretro frontend: a known-good SNES interpreter
 * with recomp debugging instrumentation, used as the differential oracle for
 * the recompiler. Loads a libretro SNES core, plays a ROM with reliable SDL
 * keyboard input, and logs per-frame WRAM changes (same JSON shape as the
 * recomp debug_server's wram_writes_at) to snesref_trace.jsonl.
 *
 *   snesref <libretro-core> <rom.sfc>
 *
 * Deterministic scene capture: SNESREF_SCRIPT (wait/press/poke/until/dump/
 * quit grammar shared with the recomp host) + SNESREF_DUMP_DIR; layer
 * isolation via SNESREF_LAYERS. With the patched snes9x core
 * (libretro/snesref_debug.cpp) dumps also carry CGRAM, OAM, register state
 * and the always-on per-scanline PPU write journal. See README.md.
 *
 * Keys (match the recomp keybinds): arrows=D-pad, Z=B(jump), X=A, A=Y(fire),
 *   S=X, C=L, V=R, Enter=Start, RShift=Select.
 *   Shift+F1-F9 = save state slot       F1-F9 = load state slot
 *   Backspace = clear the WRAM trace    Esc = quit
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <map>
#include "libretro.h"

// ---- portable dynamic-core loading + core function pointers ----
#ifdef _WIN32
using CoreHandle = HMODULE;
#else
using CoreHandle = void*;
#endif

static CoreHandle g_core;

static CoreHandle core_open(const char* path) {
#ifdef _WIN32
    return LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

static void* core_symbol(CoreHandle core, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(core, name));
#else
    dlerror();
    return dlsym(core, name);
#endif
}

static const char* core_error() {
#ifdef _WIN32
    static char message[64];
    snprintf(message, sizeof message, "Windows error %lu",
             static_cast<unsigned long>(GetLastError()));
    return message;
#else
    const char* message = dlerror();
    return message ? message : "unknown dynamic-loader error";
#endif
}

static void core_close(CoreHandle core) {
    if (!core) return;
#ifdef _WIN32
    FreeLibrary(core);
#else
    dlclose(core);
#endif
}

#define LR(sym) static decltype(&sym) p_##sym;
LR(retro_init) LR(retro_deinit) LR(retro_api_version)
LR(retro_get_system_info) LR(retro_get_system_av_info)
LR(retro_set_environment) LR(retro_set_video_refresh)
LR(retro_set_audio_sample) LR(retro_set_audio_sample_batch)
LR(retro_set_input_poll) LR(retro_set_input_state)
LR(retro_set_controller_port_device)
LR(retro_load_game) LR(retro_unload_game) LR(retro_run)
LR(retro_serialize_size) LR(retro_serialize) LR(retro_unserialize)
LR(retro_get_memory_data) LR(retro_get_memory_size)
#undef LR
static void (*p_retro_reset)(void);

// ---- optional snesref debug exports (patched snes9x core; see README) ----
// Resolved with core_symbol(); a stock core simply lacks them and the dump
// skips cgram/oam/regs/ppuw with a warning.
struct SnesrefPpuwEntry {      // must match libretro/snesref_debug.cpp
    uint32_t frame;
    uint16_t vcounter;
    uint16_t hcounter;         // dots 0..339
    uint16_t addr;
    uint8_t  value;
    uint8_t  source;           // 0 cpu, 1 dma, 2 hdma
};
static unsigned (*p_dbg_version)(void);
static void     (*p_dbg_set_frame)(uint32_t);
static size_t   (*p_dbg_cgram)(uint8_t*, size_t);
static size_t   (*p_dbg_oam)(uint8_t*, size_t);
static size_t   (*p_dbg_regs_json)(char*, size_t);
static size_t   (*p_dbg_ppuw_entry_size)(void);
static size_t   (*p_dbg_ppuw_capacity)(void);
static uint64_t (*p_dbg_ppuw_head)(void);
static size_t   (*p_dbg_ppuw_read)(uint64_t, size_t, void*, uint64_t*);

template<class T> static void bind_opt(T& fn, const char* name) {
    fn = reinterpret_cast<T>(core_symbol(g_core, name));
}

template<class T> static void bind(T& fn, const char* name) {
    fn = reinterpret_cast<T>(core_symbol(g_core, name));
    if (!fn) {
        fprintf(stderr, "missing core symbol %s: %s\n", name, core_error());
        exit(2);
    }
}

// ---- video state ----
static SDL_Window*   g_win;
static SDL_Renderer* g_ren;
static SDL_Texture*  g_tex;
static int g_tex_w = 0, g_tex_h = 0;
static retro_pixel_format g_fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static SDL_GameController* g_pad = nullptr;
static bool g_headless = false;

static void open_first_pad() {
    if (g_pad) return;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            g_pad = SDL_GameControllerOpen(i);
            if (g_pad) { printf("[controller: %s]\n", SDL_GameControllerName(g_pad)); fflush(stdout); return; }
        }
    }
}

// ---- WRAM trace ----
// Retargeted 2026-06-02 for the Rangda Bangda blue-eye coordinate-space bug:
// capture the eye AI's CE9A target globals $0BAD/$0BB0 (X's position used as
// the fly target) plus the whole $0E00-$1FFF object table region so the
// flying-eye slot can be located by its marching X position regardless of
// which slot snes9x picks. Goal: read hardware eye [D+0x05] (eye X) and
// $0BAD at the launch frame and compare to the recomp (eye X ~83 screen vs
// $0BAD 5143 level). See MegamanXRecomp/ISSUES.md.
// Per-frame changed-byte trace over low WRAM ($0000-$1FFF: zero page, stack,
// and game-logic state — where first divergences live). Widened from the old
// MMX-specific region for general recomp-vs-oracle first-divergence diffing.
// On the first tick the whole region is emitted as a baseline so the diff tool
// can reconstruct absolute state at any frame. Output file is
// SNESREF_TRACE_FILE (default snesref_trace.jsonl).
#define WRAM_LO 0x00000
#define WRAM_HI 0x01fff
static FILE* g_log;
static uint8_t g_prev[WRAM_HI - WRAM_LO + 1];
static bool    g_primed = false;
static uint32_t g_frame = 0;

struct InputEvent {
    uint64_t start;
    uint64_t duration;
    uint16_t mask;
};
static std::vector<InputEvent> g_input_events;
static bool g_scripted_input = false;
static uint16_t g_scripted_mask = 0;

static bool load_input_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "cannot open input file %s\n", path);
        return false;
    }
    char line[256];
    unsigned line_number = 0;
    while (fgets(line, sizeof(line), f)) {
        line_number++;
        char* comment = strchr(line, '#');
        if (comment) *comment = '\0';
        unsigned long long start = 0, duration = 0;
        unsigned mask = 0;
        if (sscanf(line, " %llu:%llu:%x", &start, &duration, &mask) == 3) {
            if (!duration || mask > 0x0fffu) {
                fprintf(stderr, "invalid input event at %s:%u\n", path, line_number);
                fclose(f);
                return false;
            }
            g_input_events.push_back({start, duration, (uint16_t)mask});
            continue;
        }
        for (const char* p = line; *p; p++) {
            if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
                fprintf(stderr, "invalid input event at %s:%u\n", path, line_number);
                fclose(f);
                return false;
            }
        }
    }
    fclose(f);
    g_scripted_input = true;
    fprintf(stderr, "[input] loaded %zu event(s) from %s\n",
            g_input_events.size(), path);
    return true;
}

static bool initialize_system_ram() {
    const char* value = getenv("SNESREF_WRAM_FILL");
    if (!value || !value[0]) return true;

    char* end = nullptr;
    unsigned long fill = strtoul(value, &end, 0);
    if (!end || *end || fill > 0xff) {
        fprintf(stderr, "invalid SNESREF_WRAM_FILL '%s' (expected 0..255)\n",
                value);
        return false;
    }

    void* ram = p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    size_t size = p_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (!ram || !size) {
        fprintf(stderr,
                "core does not expose RETRO_MEMORY_SYSTEM_RAM; "
                "cannot apply SNESREF_WRAM_FILL\n");
        return false;
    }
    memset(ram, (int)fill, size);
    fprintf(stderr, "[memory] initialized %zu WRAM bytes to 0x%02lx\n",
            size, fill);
    return true;
}

static bool dump_system_ram() {
    const char* path = getenv("SNESREF_WRAM_DUMP");
    if (!path || !path[0]) return true;
    void* ram = p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    size_t size = p_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    FILE* stream = fopen(path, "wb");
    if (!ram || !size || !stream) {
        if (stream) fclose(stream);
        return false;
    }
    bool ok = fwrite(ram, 1, size, stream) == size;
    if (fclose(stream) != 0) ok = false;
    return ok;
}

static uint16_t g_script_frame_mask = 0;   // set by script_tick() each frame

static void update_scripted_input() {
    if (!g_scripted_input) return;
    uint16_t next = g_script_frame_mask;
    for (const InputEvent& event : g_input_events) {
        if (g_frame >= event.start && g_frame - event.start < event.duration)
            next |= event.mask;
    }
    if (next != g_scripted_mask) {
        fprintf(stderr, "[input] frame=%u mask=%03x\n", g_frame, next);
        g_scripted_mask = next;
    }
}

static const char* trace_path() {
    const char* p = getenv("SNESREF_TRACE_FILE");
    return (p && p[0]) ? p : "snesref_trace.jsonl";
}
static void emit(int addr, uint8_t o, uint8_t n) {
    if (!g_log) { g_log = fopen(trace_path(), "a"); if (!g_log) return; }
    fprintf(g_log, "{\"f\":%u,\"adr\":\"0x%05x\",\"old\":\"0x%02x\",\"val\":\"0x%02x\"}\n",
            g_frame, addr, o, n);
}

static void trace_tick() {
    uint8_t* ram = (uint8_t*)p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    size_t sz = p_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (!ram || sz <= WRAM_HI) return;
    if (!g_primed) {
        for (int a=WRAM_LO;a<=WRAM_HI;a++){ g_prev[a-WRAM_LO]=ram[a]; emit(a,0,ram[a]); }
        g_primed=true; return;
    }
    for (int a=WRAM_LO;a<=WRAM_HI;a++){ uint8_t v=ram[a]; if(v!=g_prev[a-WRAM_LO]){ emit(a,g_prev[a-WRAM_LO],v); g_prev[a-WRAM_LO]=v; } }
    if (g_log && (g_frame % 30)==0) fflush(g_log);
}

static void clear_trace() {
    if (g_log) { fclose(g_log); g_log=nullptr; }
    FILE* f=fopen(trace_path(),"w"); if(f) fclose(f);
    g_primed=false;
    printf("[trace cleared]\n"); fflush(stdout);
}

// ---- APU/SPC-RAM trace (co-sim step 1: audio hunt) ----
// bsnes exposes the 64K SPC RAM (dsp.apuram) via the custom retro memory id
// 0x100 (see target-libretro/libretro.cpp COSIM_MEMORY_APURAM). Per-frame
// changed-byte trace in the SAME jsonl shape as WRAM, so it aligns 1:1 against
// the recomp's SNESRECOMP_APURAM_TRACE_FILE via align_diff.py --size 0x10000.
// Env-gated by SNESREF_APURAM_TRACE_FILE (off => zero cost).
#define COSIM_MEMORY_APURAM 0x100
#define APURAM_HI 0x0ffff
static FILE* g_apu_log;
static uint8_t g_apu_prev[APURAM_HI + 1];
static bool    g_apu_primed = false;
static int     g_apu_enabled = -1;

static void apuram_trace_tick() {
    if (g_apu_enabled < 0) {
        const char* p = getenv("SNESREF_APURAM_TRACE_FILE");
        g_apu_enabled = (p && p[0]) ? 1 : 0;
    }
    if (!g_apu_enabled) return;
    uint8_t* ram = (uint8_t*)p_retro_get_memory_data(COSIM_MEMORY_APURAM);
    size_t sz = p_retro_get_memory_size(COSIM_MEMORY_APURAM);
    if (!ram || sz <= APURAM_HI) return;
    if (!g_apu_log) {
        g_apu_log = fopen(getenv("SNESREF_APURAM_TRACE_FILE"), "a");
        if (!g_apu_log) { g_apu_enabled = 0; return; }
    }
    if (!g_apu_primed) {
        for (int a=0;a<=APURAM_HI;a++){ g_apu_prev[a]=ram[a];
            fprintf(g_apu_log, "{\"f\":%u,\"adr\":\"0x%05x\",\"old\":\"0x00\",\"val\":\"0x%02x\"}\n",
                    g_frame, a, ram[a]); }
        g_apu_primed=true; return;
    }
    for (int a=0;a<=APURAM_HI;a++){ uint8_t v=ram[a]; if(v!=g_apu_prev[a]){
        fprintf(g_apu_log, "{\"f\":%u,\"adr\":\"0x%05x\",\"old\":\"0x%02x\",\"val\":\"0x%02x\"}\n",
                g_frame, a, g_apu_prev[a], v); g_apu_prev[a]=v; } }
    if (g_apu_log && (g_frame % 30)==0) fflush(g_apu_log);
}

// ---- S-DSP register-file trace (co-sim audio hunt) ----
// bsnes exposes the live 128-byte DSP register file (VxPITCH etc.) via the
// custom retro memory id 0x101 (COSIM_MEMORY_DSPREGS). Per-frame changed-byte
// trace in the SAME jsonl shape => aligns against the recomp's
// SNESRECOMP_DSPREG_TRACE_FILE via align_diff.py --size 0x80.
// Env-gated by SNESREF_DSPREG_TRACE_FILE.
#define COSIM_MEMORY_DSPREGS 0x101
#define DSPREG_HI 0x7f
static FILE* g_dsp_log;
static uint8_t g_dsp_prev[DSPREG_HI + 1];
static bool    g_dsp_primed = false;
static int     g_dsp_enabled = -1;

static void dspreg_trace_tick() {
    if (g_dsp_enabled < 0) {
        const char* p = getenv("SNESREF_DSPREG_TRACE_FILE");
        g_dsp_enabled = (p && p[0]) ? 1 : 0;
    }
    if (!g_dsp_enabled) return;
    uint8_t* regs = (uint8_t*)p_retro_get_memory_data(COSIM_MEMORY_DSPREGS);
    size_t sz = p_retro_get_memory_size(COSIM_MEMORY_DSPREGS);
    if (!regs || sz <= DSPREG_HI) return;
    if (!g_dsp_log) {
        g_dsp_log = fopen(getenv("SNESREF_DSPREG_TRACE_FILE"), "a");
        if (!g_dsp_log) { g_dsp_enabled = 0; return; }
    }
    if (!g_dsp_primed) {
        for (int a=0;a<=DSPREG_HI;a++){ g_dsp_prev[a]=regs[a];
            fprintf(g_dsp_log, "{\"f\":%u,\"adr\":\"0x%05x\",\"old\":\"0x00\",\"val\":\"0x%02x\"}\n",
                    g_frame, a, regs[a]); }
        g_dsp_primed=true; return;
    }
    for (int a=0;a<=DSPREG_HI;a++){ uint8_t v=regs[a]; if(v!=g_dsp_prev[a]){
        fprintf(g_dsp_log, "{\"f\":%u,\"adr\":\"0x%05x\",\"old\":\"0x%02x\",\"val\":\"0x%02x\"}\n",
                g_frame, a, g_dsp_prev[a], v); g_dsp_prev[a]=v; } }
    if (g_dsp_log && (g_frame % 30)==0) fflush(g_dsp_log);
}

// ---- core options: layer isolation / color math / clipping ----
// SNESREF_LAYERS=<hex>: bit0=BG1 bit1=BG2 bit2=BG3 bit3=BG4 bit4=OBJ (default
// 1F). Mapped to snes9x's snes9x_layer_1..5 (layer_5 = sprites, see
// Settings.BG_Forced bit 4). SNESREF_NO_COLORMATH=1 -> snes9x_gfx_transp
// disabled; SNESREF_NO_CLIP=1 -> snes9x_gfx_clip disabled. Other keys are
// left unanswered so the core uses its defaults.
static unsigned g_layer_mask = 0x1f;
static bool g_no_colormath = false;
static bool g_no_clip = false;

static bool env_flag(const char* name) {
    const char* v = getenv(name);
    return v && v[0] && v[0] != '0';
}

static bool parse_core_option_env() {
    const char* v = getenv("SNESREF_LAYERS");
    if (v && v[0]) {
        char* end = nullptr;
        unsigned long m = strtoul(v, &end, 16);
        if (!end || *end || m > 0x1f) {
            fprintf(stderr, "invalid SNESREF_LAYERS '%s' (expected hex 0..1F)\n", v);
            return false;
        }
        g_layer_mask = (unsigned)m;
    }
    g_no_colormath = env_flag("SNESREF_NO_COLORMATH");
    g_no_clip = env_flag("SNESREF_NO_CLIP");
    return true;
}

// Every option the core declares is answered with its declared default (as
// RetroArch does). Leaving options unanswered is NOT equivalent: snes9x's
// libretro port only initializes some settings from GET_VARIABLE -- e.g.
// Settings.SuperFXClockMultiplier stays 0 without an answer for
// snes9x_overclock_superfx, which freezes the GSU (Yoshi's Island hangs at
// frame 71 with the screen force-blanked).
// SNESREF_CORE_OPTIONS="key=value;key=value" overrides any option generically.
static std::map<std::string, std::string> g_opt_defaults;
static std::map<std::string, std::string> g_opt_overrides;

static void register_option_default(const char* key, const char* def) {
    if (key && def) g_opt_defaults[key] = def;
}

static void register_legacy_variables(const retro_variable* vars) {
    // "Description; default|other|..."
    for (; vars && vars->key; vars++) {
        const char* v = vars->value ? strchr(vars->value, ';') : nullptr;
        if (!v) continue;
        v++;
        while (*v == ' ') v++;
        std::string def(v, strcspn(v, "|"));
        register_option_default(vars->key, def.c_str());
    }
}

template<class Def> static void register_option_defs(const Def* d) {
    for (; d && d->key; d++) {
        const char* def = d->default_value;
        bool found = false;
        for (int i = 0; def && i < RETRO_NUM_CORE_OPTION_VALUES_MAX && d->values[i].value; i++)
            if (!strcmp(d->values[i].value, def)) { found = true; break; }
        if (!found) def = d->values[0].value;   // libretro: fall back to the first value
        register_option_default(d->key, def);
    }
}

static bool parse_core_options_override() {
    const char* s = getenv("SNESREF_CORE_OPTIONS");
    if (!s || !s[0]) return true;
    std::string all(s);
    size_t pos = 0;
    while (pos <= all.size()) {
        size_t end = all.find(';', pos);
        if (end == std::string::npos) end = all.size();
        std::string kv = all.substr(pos, end - pos);
        if (!kv.empty()) {
            size_t eq = kv.find('=');
            if (eq == std::string::npos || eq == 0) {
                fprintf(stderr, "invalid SNESREF_CORE_OPTIONS entry '%s' (expected key=value)\n", kv.c_str());
                return false;
            }
            g_opt_overrides[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
        pos = end + 1;
    }
    return true;
}

static bool answer_variable_impl(retro_variable* var);

// Answers GET_VARIABLE and logs (once per key) every answer that differs
// from the core's declared default, so an override is visibly in effect.
static bool answer_variable(retro_variable* var) {
    bool ok = answer_variable_impl(var);
    if (ok && var && var->key && var->value) {
        static std::map<std::string, std::string> logged;
        auto d = g_opt_defaults.find(var->key);
        if ((d == g_opt_defaults.end() || d->second != var->value) && logged[var->key] != var->value) {
            logged[var->key] = var->value;
            printf("core option %s=%s (default %s)\n", var->key, var->value,
                   d == g_opt_defaults.end() ? "undeclared" : d->second.c_str());
        }
    }
    return ok;
}

static bool answer_variable_impl(retro_variable* var) {
    if (!var || !var->key) return false;
    static const char* const kLayerKeys[5] = {
        "snes9x_layer_1", "snes9x_layer_2", "snes9x_layer_3",
        "snes9x_layer_4", "snes9x_layer_5" };
    for (int i = 0; i < 5; i++) {
        if (!strcmp(var->key, kLayerKeys[i])) {
            var->value = (g_layer_mask & (1u << i)) ? "enabled" : "disabled";
            return true;
        }
    }
    if (!strcmp(var->key, "snes9x_gfx_transp")) {
        var->value = g_no_colormath ? "disabled" : "enabled";
        return true;
    }
    if (!strcmp(var->key, "snes9x_gfx_clip")) {
        var->value = g_no_clip ? "disabled" : "enabled";
        return true;
    }
    auto o = g_opt_overrides.find(var->key);
    if (o != g_opt_overrides.end()) { var->value = o->second.c_str(); return true; }
    auto d = g_opt_defaults.find(var->key);
    if (d != g_opt_defaults.end()) { var->value = d->second.c_str(); return true; }
    return false;
}

// ---- libretro callbacks ----
static bool cb_environment(unsigned cmd, void* data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_VARIABLE: return answer_variable((retro_variable*)data);
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned*)data = 2; return true;
        case RETRO_ENVIRONMENT_SET_VARIABLES:
            register_legacy_variables((const retro_variable*)data); return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
            register_option_defs((const retro_core_option_definition*)data); return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL: {
            const retro_core_options_intl* in = (const retro_core_options_intl*)data;
            if (in) register_option_defs((const retro_core_option_definition*)in->us);
            return true; }
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2: {
            const retro_core_options_v2* v2 = (const retro_core_options_v2*)data;
            if (v2) register_option_defs((const retro_core_option_v2_definition*)v2->definitions);
            return true; }
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
            const retro_core_options_v2_intl* in = (const retro_core_options_v2_intl*)data;
            if (in && in->us) register_option_defs((const retro_core_option_v2_definition*)in->us->definitions);
            return true; }
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY: return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool*)data = true; return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: g_fmt = *(const retro_pixel_format*)data; return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char**)data = "."; return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:   *(const char**)data = "."; return true;
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL: return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: if(data) *(bool*)data=false; return true;
        default: return false;
    }
}
static void ensure_texture(unsigned w, unsigned h) {
    if ((int)w==g_tex_w && (int)h==g_tex_h && g_tex) return;
    if (g_tex) SDL_DestroyTexture(g_tex);
    Uint32 sf = (g_fmt==RETRO_PIXEL_FORMAT_XRGB8888) ? SDL_PIXELFORMAT_ARGB8888
              : (g_fmt==RETRO_PIXEL_FORMAT_RGB565)   ? SDL_PIXELFORMAT_RGB565
              :                                        SDL_PIXELFORMAT_ARGB1555;
    g_tex = SDL_CreateTexture(g_ren, sf, SDL_TEXTUREACCESS_STREAMING, w, h);
    g_tex_w=w; g_tex_h=h;
}
// Raw frame dump for the PPU framebuffer diff vs the recomp. Controlled by env:
//   SNESREF_FRAME_DUMP_DIR + _FROM/_TO/_STEP. Writes <dir>/frame_NNNNNN.raw as
//   256x224 BGRX (XRGB8888 byte order = same as the recomp's dump). g_frame here
//   is the pre-increment frame number (first produced frame = 0).
static void maybe_dump_frame(const void* data, unsigned w, unsigned h, size_t pitch) {
    static int inited = 0;
    static const char* dir = nullptr;
    static long from = -1, to = -1, step = 1;
    if (!inited) {
        inited = 1;
        dir = getenv("SNESREF_FRAME_DUMP_DIR");
        const char* f = getenv("SNESREF_FRAME_DUMP_FROM"); if (f && f[0]) from = atol(f);
        const char* t = getenv("SNESREF_FRAME_DUMP_TO");   if (t && t[0]) to   = atol(t);
        const char* s = getenv("SNESREF_FRAME_DUMP_STEP"); if (s && s[0]) step = atol(s);
        if (step < 1) step = 1;
    }
    static int announced = 0;
    if (!announced) { announced = 1;
        fprintf(stderr, "[framedump] fmt=%d w=%u h=%u pitch=%zu dir=%s\n",
                (int)g_fmt, w, h, pitch, dir); fflush(stderr); }
    if (!dir || !dir[0] || !data) return;
    long fr = (long)g_frame;
    if (fr < from || fr > to || ((fr - from) % step) != 0) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/frame_%06ld.raw", dir, fr);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    int cols = (w < 256) ? (int)w : 256;
    int rows = (h < 224) ? (int)h : 224;
    const unsigned char* p = (const unsigned char*)data;
    unsigned char row[256 * 4];
    for (int y = 0; y < 224; y++) {
        memset(row, 0, sizeof(row));
        if (y < rows) {
            const unsigned char* sr = p + (size_t)y * pitch;
            for (int x = 0; x < cols; x++) {
                unsigned char B, G, R;
                if (g_fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
                    B = sr[x*4+0]; G = sr[x*4+1]; R = sr[x*4+2];  // already BGRX
                } else if (g_fmt == RETRO_PIXEL_FORMAT_RGB565) {
                    unsigned v = sr[x*2+0] | (sr[x*2+1] << 8);
                    unsigned r5=(v>>11)&0x1f, g6=(v>>5)&0x3f, b5=v&0x1f;
                    R=(unsigned char)((r5<<3)|(r5>>2)); G=(unsigned char)((g6<<2)|(g6>>4)); B=(unsigned char)((b5<<3)|(b5>>2));
                } else { // 0RGB1555
                    unsigned v = sr[x*2+0] | (sr[x*2+1] << 8);
                    unsigned r5=(v>>10)&0x1f, g5=(v>>5)&0x1f, b5=v&0x1f;
                    R=(unsigned char)((r5<<3)|(r5>>2)); G=(unsigned char)((g5<<3)|(g5>>2)); B=(unsigned char)((b5<<3)|(b5>>2));
                }
                row[x*4+0]=B; row[x*4+1]=G; row[x*4+2]=R; row[x*4+3]=0;
            }
        }
        fwrite(row, 1, 256 * 4, f);
    }
    fclose(f);
}
// Last presented frame, converted to BGRX (for script `dump`). Kept as a copy
// because the core's buffer is only valid during the callback.
static std::vector<uint8_t> g_fb_bgrx;
static unsigned g_fb_w = 0, g_fb_h = 0;
static uint32_t g_fb_frame = 0;        // frame number (1-based retro_run) it came from
static retro_pixel_format g_fb_src_fmt = RETRO_PIXEL_FORMAT_0RGB1555;

static void capture_frame(const void* data, unsigned w, unsigned h, size_t pitch) {
    g_fb_bgrx.resize((size_t)w * h * 4);
    const unsigned char* p = (const unsigned char*)data;
    for (unsigned y = 0; y < h; y++) {
        const unsigned char* sr = p + (size_t)y * pitch;
        unsigned char* dr = &g_fb_bgrx[(size_t)y * w * 4];
        for (unsigned x = 0; x < w; x++) {
            unsigned char B, G, R;
            if (g_fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
                B = sr[x*4+0]; G = sr[x*4+1]; R = sr[x*4+2];
            } else if (g_fmt == RETRO_PIXEL_FORMAT_RGB565) {
                unsigned v = sr[x*2+0] | (sr[x*2+1] << 8);
                unsigned r5=(v>>11)&0x1f, g6=(v>>5)&0x3f, b5=v&0x1f;
                R=(unsigned char)((r5<<3)|(r5>>2)); G=(unsigned char)((g6<<2)|(g6>>4)); B=(unsigned char)((b5<<3)|(b5>>2));
            } else {
                unsigned v = sr[x*2+0] | (sr[x*2+1] << 8);
                unsigned r5=(v>>10)&0x1f, g5=(v>>5)&0x1f, b5=v&0x1f;
                R=(unsigned char)((r5<<3)|(r5>>2)); G=(unsigned char)((g5<<3)|(g5>>2)); B=(unsigned char)((b5<<3)|(b5>>2));
            }
            dr[x*4+0]=B; dr[x*4+1]=G; dr[x*4+2]=R; dr[x*4+3]=0;
        }
    }
    g_fb_w = w; g_fb_h = h; g_fb_frame = g_frame + 1; g_fb_src_fmt = g_fmt;
}

static void cb_video(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (data && w && h) {
        capture_frame(data, w, h, pitch);
        maybe_dump_frame(data, w, h, pitch);
        if (g_headless) return;
        ensure_texture(w,h);
        SDL_UpdateTexture(g_tex, nullptr, data, (int)pitch);
    }
    if (g_headless) return;
    SDL_RenderClear(g_ren);
    if (g_tex) SDL_RenderCopy(g_ren, g_tex, nullptr, nullptr);
    SDL_RenderPresent(g_ren);
}
// ---- audio capture ----
// Always-on WAV dump of everything the core outputs, from frame 0: the
// ground-truth PCM for differential audio comparison against the recomp's
// audio_trace ring (debug_server `audio_wav`). Header sizes are patched on
// close; the sample rate comes from retro_get_system_av_info.
static FILE*    g_wav;
static uint64_t g_wav_sample_frames; // stereo frames written
static uint32_t g_wav_rate = 32040;

static void wav_open(const char* path, double rate) {
    g_wav = fopen(path, "wb");
    if (!g_wav) { fprintf(stderr, "cannot open %s\n", path); return; }
    g_wav_rate = (uint32_t)(rate + 0.5);
    uint8_t hdr[44] = {0};
    fwrite(hdr, 1, 44, g_wav); // placeholder, patched in wav_close
}
static void wav_close() {
    if (!g_wav) return;
    uint32_t data_bytes = (uint32_t)(g_wav_sample_frames * 4);
    uint32_t riff = 36 + data_bytes, fmt32 = 16, brate = g_wav_rate * 4;
    uint16_t pcm = 1, ch = 2, balign = 4, bits = 16;
    fseek(g_wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, g_wav); fwrite(&riff, 4, 1, g_wav);
    fwrite("WAVEfmt ", 1, 8, g_wav);
    fwrite(&fmt32, 4, 1, g_wav); fwrite(&pcm, 2, 1, g_wav); fwrite(&ch, 2, 1, g_wav);
    fwrite(&g_wav_rate, 4, 1, g_wav); fwrite(&brate, 4, 1, g_wav);
    fwrite(&balign, 2, 1, g_wav); fwrite(&bits, 2, 1, g_wav);
    fwrite("data", 1, 4, g_wav); fwrite(&data_bytes, 4, 1, g_wav);
    fclose(g_wav); g_wav = nullptr;
    printf("[wav closed: %llu frames @ %u Hz]\n",
           (unsigned long long)g_wav_sample_frames, g_wav_rate);
}
static void  cb_audio_sample(int16_t l, int16_t r) {
    if (g_wav) { int16_t s[2] = {l, r}; fwrite(s, 4, 1, g_wav); g_wav_sample_frames++; }
}
static size_t cb_audio_batch(const int16_t* data, size_t frames) {
    if (g_wav && data && frames) { fwrite(data, 4, frames, g_wav); g_wav_sample_frames += frames; }
    return frames;
}
static void  cb_input_poll(void) {}

static int16_t cb_input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)index;
    if (port!=0 || device!=RETRO_DEVICE_JOYPAD) return 0;
    if (g_scripted_input) {
        uint16_t bit = 0;
        switch (id) {
            case RETRO_DEVICE_ID_JOYPAD_B:      bit=0x0001; break;
            case RETRO_DEVICE_ID_JOYPAD_Y:      bit=0x0002; break;
            case RETRO_DEVICE_ID_JOYPAD_SELECT: bit=0x0004; break;
            case RETRO_DEVICE_ID_JOYPAD_START:  bit=0x0008; break;
            case RETRO_DEVICE_ID_JOYPAD_UP:     bit=0x0010; break;
            case RETRO_DEVICE_ID_JOYPAD_DOWN:   bit=0x0020; break;
            case RETRO_DEVICE_ID_JOYPAD_LEFT:   bit=0x0040; break;
            case RETRO_DEVICE_ID_JOYPAD_RIGHT:  bit=0x0080; break;
            case RETRO_DEVICE_ID_JOYPAD_A:      bit=0x0100; break;
            case RETRO_DEVICE_ID_JOYPAD_X:      bit=0x0200; break;
            case RETRO_DEVICE_ID_JOYPAD_L:      bit=0x0400; break;
            case RETRO_DEVICE_ID_JOYPAD_R:      bit=0x0800; break;
            default: return 0;
        }
        return (g_scripted_mask & bit) != 0;
    }
    if (g_headless) return 0;
    const Uint8* ks = SDL_GetKeyboardState(nullptr);
    SDL_Scancode sc; SDL_GameControllerButton gb;
    switch (id) {
        case RETRO_DEVICE_ID_JOYPAD_B:      sc=SDL_SCANCODE_Z;      gb=SDL_CONTROLLER_BUTTON_A; break;             /* jump (PS5 cross) */
        case RETRO_DEVICE_ID_JOYPAD_Y:      sc=SDL_SCANCODE_A;      gb=SDL_CONTROLLER_BUTTON_X; break;             /* fire (PS5 square) */
        case RETRO_DEVICE_ID_JOYPAD_A:      sc=SDL_SCANCODE_X;      gb=SDL_CONTROLLER_BUTTON_B; break;             /* PS5 circle */
        case RETRO_DEVICE_ID_JOYPAD_X:      sc=SDL_SCANCODE_S;      gb=SDL_CONTROLLER_BUTTON_Y; break;             /* PS5 triangle */
        case RETRO_DEVICE_ID_JOYPAD_L:      sc=SDL_SCANCODE_C;      gb=SDL_CONTROLLER_BUTTON_LEFTSHOULDER; break;
        case RETRO_DEVICE_ID_JOYPAD_R:      sc=SDL_SCANCODE_V;      gb=SDL_CONTROLLER_BUTTON_RIGHTSHOULDER; break;
        case RETRO_DEVICE_ID_JOYPAD_START:  sc=SDL_SCANCODE_RETURN; gb=SDL_CONTROLLER_BUTTON_START; break;
        case RETRO_DEVICE_ID_JOYPAD_SELECT: sc=SDL_SCANCODE_RSHIFT; gb=SDL_CONTROLLER_BUTTON_BACK; break;
        case RETRO_DEVICE_ID_JOYPAD_UP:     sc=SDL_SCANCODE_UP;     gb=SDL_CONTROLLER_BUTTON_DPAD_UP; break;
        case RETRO_DEVICE_ID_JOYPAD_DOWN:   sc=SDL_SCANCODE_DOWN;   gb=SDL_CONTROLLER_BUTTON_DPAD_DOWN; break;
        case RETRO_DEVICE_ID_JOYPAD_LEFT:   sc=SDL_SCANCODE_LEFT;   gb=SDL_CONTROLLER_BUTTON_DPAD_LEFT; break;
        case RETRO_DEVICE_ID_JOYPAD_RIGHT:  sc=SDL_SCANCODE_RIGHT;  gb=SDL_CONTROLLER_BUTTON_DPAD_RIGHT; break;
        default: return 0;
    }
    if (ks[sc]) return 1;
    if (g_pad && SDL_GameControllerGetButton(g_pad, gb)) return 1;
    /* analog left-stick as d-pad fallback */
    if (g_pad) {
        const int DZ = 16000;
        if (id==RETRO_DEVICE_ID_JOYPAD_LEFT  && SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTX) < -DZ) return 1;
        if (id==RETRO_DEVICE_ID_JOYPAD_RIGHT && SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTX) >  DZ) return 1;
        if (id==RETRO_DEVICE_ID_JOYPAD_UP    && SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTY) < -DZ) return 1;
        if (id==RETRO_DEVICE_ID_JOYPAD_DOWN  && SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTY) >  DZ) return 1;
    }
    return 0;
}

// ---- state dump (script `dump <tag>`) ----
// Writes, into SNESREF_DUMP_DIR (default cwd), the state of the frame just
// completed: <tag>.fb.bmp/.fb.bgrx/.wram.bin/.vram.bin/.sram.bin/.info.json,
// plus (patched core only) .cgram.bin/.oam.bin/.regs.json/.ppuw.tsv.
static const char* g_core_name = "?";
static const char* g_core_version = "?";

static const char* dump_dir() {
    const char* d = getenv("SNESREF_DUMP_DIR");
    return (d && d[0]) ? d : ".";
}

static bool write_file(const char* tag, const char* ext, const void* data, size_t n) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.%s", dump_dir(), tag, ext);
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "dump: cannot write %s\n", path); return false; }
    bool ok = n == 0 || fwrite(data, 1, n, f) == n;
    if (fclose(f) != 0) ok = false;
    if (!ok) fprintf(stderr, "dump: short write %s\n", path);
    return ok;
}

static bool write_bmp24(const char* tag) {
    const unsigned w = g_fb_w, h = g_fb_h;
    const unsigned row = (w * 3 + 3) & ~3u;
    const uint32_t img = row * h, off = 54, fsz = off + img;
    std::vector<uint8_t> b(fsz, 0);
    auto le32 = [&](size_t o, uint32_t v) { b[o]=(uint8_t)v; b[o+1]=(uint8_t)(v>>8); b[o+2]=(uint8_t)(v>>16); b[o+3]=(uint8_t)(v>>24); };
    auto le16 = [&](size_t o, uint16_t v) { b[o]=(uint8_t)v; b[o+1]=(uint8_t)(v>>8); };
    b[0]='B'; b[1]='M'; le32(2, fsz); le32(10, off);
    le32(14, 40); le32(18, w); le32(22, h); le16(26, 1); le16(28, 24); le32(34, img);
    le32(38, 2835); le32(42, 2835);
    for (unsigned y = 0; y < h; y++) {
        const uint8_t* s = &g_fb_bgrx[(size_t)(h - 1 - y) * w * 4];   // bottom-up
        uint8_t* d = &b[off + (size_t)y * row];
        for (unsigned x = 0; x < w; x++) { d[x*3]=s[x*4]; d[x*3+1]=s[x*4+1]; d[x*3+2]=s[x*4+2]; }
    }
    return write_file(tag, "fb.bmp", b.data(), b.size());
}

static bool dump_memory(const char* tag, const char* ext, unsigned id, size_t* out_size) {
    void* p = p_retro_get_memory_data(id);
    size_t n = p_retro_get_memory_size(id);
    if (out_size) *out_size = p ? n : 0;
    if (!p || !n) { fprintf(stderr, "dump: core exposes no memory id %u (%s) -- skipped\n", id, ext); return false; }
    return write_file(tag, ext, p, n);
}

static const char* fmt_name(retro_pixel_format f) {
    switch (f) {
        case RETRO_PIXEL_FORMAT_XRGB8888: return "XRGB8888";
        case RETRO_PIXEL_FORMAT_RGB565:   return "RGB565";
        default:                          return "0RGB1555";
    }
}

static void dump_ppuw(const char* tag, uint32_t frame, long long* out_count, bool* out_truncated) {
    *out_count = -1; *out_truncated = false;
    if (!p_dbg_ppuw_head || !p_dbg_ppuw_read || !p_dbg_ppuw_entry_size || !p_dbg_ppuw_capacity) {
        fprintf(stderr, "dump: core lacks snesref_dbg_ppuw_* -- %s.ppuw.tsv skipped\n", tag);
        return;
    }
    if (p_dbg_ppuw_entry_size() != sizeof(SnesrefPpuwEntry)) {
        fprintf(stderr, "dump: ppuw entry size mismatch (core %zu, frontend %zu) -- skipped\n",
                p_dbg_ppuw_entry_size(), sizeof(SnesrefPpuwEntry));
        return;
    }
    // Query the always-on ring backward from head for entries tagged `frame`.
    const uint64_t head = p_dbg_ppuw_head();
    const uint64_t cap = p_dbg_ppuw_capacity();
    const uint64_t oldest = head > cap ? head - cap : 0;
    std::vector<SnesrefPpuwEntry> chunk(65536), found;
    uint64_t end = head;
    bool done = false;
    while (!done && end > oldest) {
        uint64_t start = end > oldest + chunk.size() ? end - chunk.size() : oldest;
        uint64_t first = 0;
        size_t n = p_dbg_ppuw_read(start, (size_t)(end - start), chunk.data(), &first);
        if (n == 0) break;
        for (size_t i = n; i-- > 0;) {
            const SnesrefPpuwEntry& e = chunk[i];
            if (e.frame == frame) found.push_back(e);
            else if (e.frame < frame) { done = true; break; }
        }
        end = first;
    }
    if (!done && !found.empty() && end <= oldest && oldest > 0) *out_truncated = true;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.ppuw.tsv", dump_dir(), tag);
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "dump: cannot write %s\n", path); return; }
    fprintf(f, "#frame\tvcounter\thcounter\taddr\tvalue\tsource\n");
    if (*out_truncated) fprintf(f, "#truncated: ring evicted the start of this frame\n");
    static const char* const kSrc[3] = { "cpu", "dma", "hdma" };
    for (size_t i = found.size(); i-- > 0;) {
        const SnesrefPpuwEntry& e = found[i];
        fprintf(f, "%u\t%u\t%u\t%04X\t%02X\t%s\n", e.frame, e.vcounter, e.hcounter, e.addr, e.value,
                e.source < 3 ? kSrc[e.source] : "?");
    }
    fclose(f);
    *out_count = (long long)found.size();
}

static void do_dump(const char* tag) {
    const uint32_t frame = g_frame;   // the frame just completed (1-based)
    bool fb_ok = false;
    if (g_fb_w && g_fb_h && !g_fb_bgrx.empty()) {
        fb_ok = write_file(tag, "fb.bgrx", g_fb_bgrx.data(), g_fb_bgrx.size()) && write_bmp24(tag);
        if (g_fb_frame != frame)
            fprintf(stderr, "dump %s: framebuffer is from frame %u (core duped frame %u)\n", tag, g_fb_frame, frame);
    } else {
        fprintf(stderr, "dump %s: no video frame yet -- fb skipped\n", tag);
    }
    size_t wram_n = 0, vram_n = 0, sram_n = 0;
    dump_memory(tag, "wram.bin", RETRO_MEMORY_SYSTEM_RAM, &wram_n);
    dump_memory(tag, "vram.bin", RETRO_MEMORY_VIDEO_RAM, &vram_n);
    dump_memory(tag, "sram.bin", RETRO_MEMORY_SAVE_RAM, &sram_n);

    bool have_dbg = p_dbg_cgram && p_dbg_oam && p_dbg_regs_json;
    if (have_dbg) {
        uint8_t cg[512]; size_t n = p_dbg_cgram(cg, sizeof cg);
        if (n == sizeof cg) write_file(tag, "cgram.bin", cg, n);
        uint8_t oam[544]; n = p_dbg_oam(oam, sizeof oam);
        if (n == sizeof oam) write_file(tag, "oam.bin", oam, n);
        std::vector<char> js(1 << 16);
        n = p_dbg_regs_json(js.data(), js.size());
        if (n > js.size()) { js.resize(n); n = p_dbg_regs_json(js.data(), js.size()); }
        if (n <= js.size()) write_file(tag, "regs.json", js.data(), n - 1);
    } else {
        fprintf(stderr, "dump %s: core lacks snesref_dbg_* exports -- cgram/oam/regs skipped\n", tag);
    }
    long long ppuw_n = -1; bool trunc = false;
    dump_ppuw(tag, frame, &ppuw_n, &trunc);

    char info[2048];
    int len = snprintf(info, sizeof info,
        "{\n\"tag\":\"%s\",\n\"frame\":%u,\n\"fb_frame\":%u,\n\"fb_width\":%u,\n\"fb_height\":%u,\n"
        "\"fb_core_pixel_format\":\"%s\",\n\"fb_dump_format\":\"BGRX8888\",\n"
        "\"core_name\":\"%s\",\n\"core_version\":\"%s\",\n\"snesref_dbg_version\":%u,\n"
        "\"layer_mask\":\"%02X\",\n\"no_colormath\":%s,\n\"no_clip\":%s,\n"
        "\"wram_size\":%zu,\n\"vram_size\":%zu,\n\"sram_size\":%zu,\n\"ppuw_entries\":%lld,\n\"ppuw_truncated\":%s\n}\n",
        tag, frame, fb_ok ? g_fb_frame : 0, fb_ok ? g_fb_w : 0, fb_ok ? g_fb_h : 0, fmt_name(fb_ok ? g_fb_src_fmt : g_fmt),
        g_core_name, g_core_version, p_dbg_version ? p_dbg_version() : 0,
        g_layer_mask, g_no_colormath ? "true" : "false", g_no_clip ? "true" : "false",
        wram_n, vram_n, sram_n, ppuw_n, trunc ? "true" : "false");
    if (len > 0) write_file(tag, "info.json", info, (size_t)len < sizeof info ? (size_t)len : sizeof info - 1);
    printf("script f=%u dump %s fb=%ux%u %s sram=%zu ppuw=%lld%s\n", frame, tag,
           fb_ok ? g_fb_w : 0, fb_ok ? g_fb_h : 0, fmt_name(fb_ok ? g_fb_src_fmt : g_fmt), sram_n, ppuw_n,
           trunc ? " (truncated)" : "");
    fflush(stdout);
}

// ---- scene-keyed script (SNESREF_SCRIPT) ----
// Grammar and per-frame semantics are shared with the recomp host
// (runner/src/desktop/host_main.c TickScript) so one file drives both:
//   wait N                     N idle frames, accumulated into the next command
//   press <b[+b|,b...]> [N]    hold for N frames (default 1)
//   poke <addr> <hex>          write WRAM bytes on one frame
//   pokefor <addr> <hex> N     write WRAM bytes on N frames
//   forcepoke <addr> <hex>     write WRAM bytes every frame from the next on
//   loadstate N / reset        loadstate: ignored with a warning; reset: retro_reset
//   until <addr> <op> <hex> [timeout]    op == or !=, 8-bit WRAM byte
//   until16 <addr> <op> <hex> [timeout]  16-bit little-endian
//   dump <tag>                 write state of the frame just completed
//   quit                       exit 0
// Hold-type commands (press/poke/pokefor/forcepoke/loadstate/reset) run for
// their hold frames and are followed by ONE idle frame. until (when true),
// dump and quit take zero frames; a false until costs one idle frame per
// re-check. Timeout (default 36000 frames) -> error, exit code 3.
enum ScriptOp { SC_PRESS, SC_POKE, SC_FORCEPOKE, SC_LOADSTATE, SC_RESET, SC_UNTIL, SC_DUMP, SC_QUIT };
struct ScriptCmd {
    ScriptOp op;
    int line;
    int wait;          // idle frames before this command
    int hold;          // hold-type frame count
    uint16_t mask;
    uint32_t addr;
    std::vector<uint8_t> bytes;
    bool is16, ne;
    uint32_t value;
    long timeout;
    std::string text;  // original command text for the log
};
static std::vector<ScriptCmd> g_script;
static size_t g_sc_idx = 0;
static int  g_sc_phase = 1;          // 1 = waiting, 0 = holding
static long g_sc_counter = 0;
static long g_sc_waited = 0;         // frames spent in a false until
static bool g_sc_started = false;
static bool g_sc_end_reported = false;
static int  g_script_exit = -1;      // >=0 -> stop the run with this exit code
static bool g_script_active = false;
struct ForcePoke { uint32_t addr; std::vector<uint8_t> bytes; };
static std::vector<ForcePoke> g_force_pokes;

static bool parse_hex_u32(const char* s, uint32_t* out) {
    if (s[0] == '$') s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (!*s) return false;
    char* end = nullptr;
    unsigned long v = strtoul(s, &end, 16);
    if (!end || *end) return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_buttons(const char* s, uint16_t* out) {
    uint16_t m = 0;
    const char* p = s;
    while (*p) {
        size_t len = strcspn(p, "+,|");
        char part[32];
        if (len == 0 || len >= sizeof part) return false;
        memcpy(part, p, len); part[len] = 0;
        for (char* c = part; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
        static const struct { const char* n; uint16_t b; } kB[] = {
            {"b",0x001},{"y",0x002},{"select",0x004},{"start",0x008},{"up",0x010},{"down",0x020},
            {"left",0x040},{"right",0x080},{"a",0x100},{"x",0x200},{"l",0x400},{"r",0x800} };
        bool ok = false;
        for (auto& k : kB) if (!strcmp(part, k.n)) { m |= k.b; ok = true; break; }
        if (!ok) { fprintf(stderr, "script: unknown button '%s'\n", part); return false; }
        p += len;
        if (*p) p++;
    }
    *out = m;
    return true;
}

static bool parse_hex_bytes(uint32_t addr, const char* hex, std::vector<uint8_t>* out) {
    size_t n = strlen(hex);
    if (n == 0 || (n & 1) || addr + n / 2 > 0x20000u) return false;
    out->clear();
    for (size_t i = 0; i < n; i += 2) {
        char t[3] = { hex[i], hex[i+1], 0 };
        char* end = nullptr;
        unsigned long v = strtoul(t, &end, 16);
        if (!end || *end) return false;
        out->push_back((uint8_t)v);
    }
    return true;
}

static bool load_script(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "script: cannot open '%s'\n", path); return false; }
    char line[512];
    int ln = 0, pending_wait = 0;
    bool ok = true;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char* c = strchr(line, '#'); if (c) *c = 0;
        char tok[5][256] = {};
        int nt = sscanf(line, "%255s %255s %255s %255s %255s", tok[0], tok[1], tok[2], tok[3], tok[4]);
        if (nt < 1) continue;
        for (char* p = tok[0]; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
        ScriptCmd e{};
        e.line = ln; e.hold = 1; e.timeout = 36000;
        { std::string t; for (int i = 0; i < nt; i++) { if (i) t += ' '; t += tok[i]; } e.text = t; }
        const char* cmd = tok[0];
        bool bad = false;
        if (!strcmp(cmd, "wait")) {
            int n = (nt >= 2) ? atoi(tok[1]) : 0;
            if (n < 0) bad = true; else pending_wait += n;
            if (!bad) continue;
        } else if (!strcmp(cmd, "press")) {
            e.op = SC_PRESS;
            if (nt < 2 || !parse_buttons(tok[1], &e.mask)) bad = true;
            if (nt >= 3) e.hold = atoi(tok[2]);
            if (e.hold < 1) bad = true;
        } else if (!strcmp(cmd, "poke") || !strcmp(cmd, "pokefor") || !strcmp(cmd, "forcepoke")) {
            e.op = !strcmp(cmd, "forcepoke") ? SC_FORCEPOKE : SC_POKE;
            if (nt < 3 || !parse_hex_u32(tok[1], &e.addr) || !parse_hex_bytes(e.addr, tok[2], &e.bytes)) bad = true;
            if (!strcmp(cmd, "pokefor")) {
                if (nt < 4) bad = true; else e.hold = atoi(tok[3]);
                if (e.hold < 1) e.hold = 1;
            }
        } else if (!strcmp(cmd, "loadstate")) {
            e.op = SC_LOADSTATE;
        } else if (!strcmp(cmd, "reset")) {
            e.op = SC_RESET;
        } else if (!strcmp(cmd, "until") || !strcmp(cmd, "until16")) {
            e.op = SC_UNTIL;
            e.is16 = !strcmp(cmd, "until16");
            if (nt < 4 || !parse_hex_u32(tok[1], &e.addr) || !parse_hex_u32(tok[3], &e.value)) bad = true;
            else if (!strcmp(tok[2], "==")) e.ne = false;
            else if (!strcmp(tok[2], "!=")) e.ne = true;
            else bad = true;
            if (!bad && e.addr + (e.is16 ? 2u : 1u) > 0x20000u) bad = true;
            if (!bad && e.value > (e.is16 ? 0xffffu : 0xffu)) bad = true;
            if (nt >= 5) { char* end = nullptr; e.timeout = strtol(tok[4], &end, 10); if (!end || *end || e.timeout < 0) bad = true; }
        } else if (!strcmp(cmd, "dump")) {
            e.op = SC_DUMP;
            if (nt < 2) bad = true;
            e.text = tok[1];
            for (const char* p = tok[1]; *p; p++)
                if (*p == '/' || *p == '\\' || *p == ':') bad = true;
            if (!bad) e.text = std::string("dump ") + tok[1];
        } else if (!strcmp(cmd, "quit")) {
            e.op = SC_QUIT;
        } else {
            fprintf(stderr, "script %s:%d: unknown command '%s'\n", path, ln, cmd);
            ok = false; continue;
        }
        if (bad) { fprintf(stderr, "script %s:%d: malformed '%s'\n", path, ln, e.text.c_str()); ok = false; continue; }
        e.wait = pending_wait; pending_wait = 0;
        g_script.push_back(std::move(e));
    }
    fclose(f);
    if (!ok) return false;
    if (pending_wait)
        fprintf(stderr, "script: trailing 'wait %d' has no following command (ignored, as on the recomp host)\n", pending_wait);
    g_sc_idx = 0; g_sc_phase = 1;
    g_sc_counter = g_script.empty() ? 0 : g_script[0].wait;
    g_script_active = true;
    fprintf(stderr, "script: loaded %zu command(s) from %s\n", g_script.size(), path);
    return true;
}

static void script_advance() {
    g_sc_idx++;
    g_sc_phase = 1; g_sc_waited = 0; g_sc_started = false;
    g_sc_counter = g_sc_idx < g_script.size() ? g_script[g_sc_idx].wait : 0;
}

// Called at each frame boundary (before retro_run); returns this frame's mask.
static uint16_t script_tick() {
    uint8_t* ram = (uint8_t*)p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    size_t ram_n = p_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    for (const ForcePoke& fp : g_force_pokes)
        if (ram && fp.addr + fp.bytes.size() <= ram_n) memcpy(ram + fp.addr, fp.bytes.data(), fp.bytes.size());
    for (;;) {
        if (g_sc_idx >= g_script.size()) {
            if (!g_sc_end_reported) {
                g_sc_end_reported = true;
                printf("script f=%u end\n", g_frame); fflush(stdout);
            }
            return 0;
        }
        ScriptCmd& c = g_script[g_sc_idx];
        if (g_sc_phase == 1) {
            if (g_sc_counter > 0) {
                if (g_sc_counter == c.wait) { printf("script f=%u wait %d\n", g_frame, c.wait); fflush(stdout); }
                g_sc_counter--;
                return 0;
            }
            g_sc_phase = 0; g_sc_counter = c.hold; g_sc_started = false;
        }
        switch (c.op) {
            case SC_UNTIL: {
                bool ok = false;
                uint32_t v = 0;
                if (ram && c.addr + (c.is16 ? 2u : 1u) <= ram_n) {
                    v = ram[c.addr] | (c.is16 ? (uint32_t)ram[c.addr + 1] << 8 : 0u);
                    ok = c.ne ? (v != c.value) : (v == c.value);
                }
                if (ok) {
                    printf("script f=%u %s ok (waited %ld)\n", g_frame, c.text.c_str(), g_sc_waited); fflush(stdout);
                    script_advance();
                    continue;
                }
                if (g_sc_waited >= c.timeout) {
                    fprintf(stderr, "script f=%u %s TIMEOUT after %ld frames (value=%0*X)\n",
                            g_frame, c.text.c_str(), g_sc_waited, c.is16 ? 4 : 2, v);
                    printf("script f=%u %s TIMEOUT\n", g_frame, c.text.c_str()); fflush(stdout);
                    g_script_exit = 3;
                    return 0;
                }
                g_sc_waited++;
                return 0;
            }
            case SC_DUMP:
                do_dump(c.text.c_str() + 5);
                script_advance();
                continue;
            case SC_QUIT:
                printf("script f=%u quit\n", g_frame); fflush(stdout);
                g_script_exit = 0;
                return 0;
            default: break;
        }
        // hold-type
        if (g_sc_counter > 0) {
            if (!g_sc_started) { g_sc_started = true; printf("script f=%u %s\n", g_frame, c.text.c_str()); fflush(stdout); }
            g_sc_counter--;
            switch (c.op) {
                case SC_PRESS: return c.mask;
                case SC_POKE:
                    if (ram && c.addr + c.bytes.size() <= ram_n) memcpy(ram + c.addr, c.bytes.data(), c.bytes.size());
                    return 0;
                case SC_FORCEPOKE:
                    g_force_pokes.push_back({ c.addr, c.bytes });
                    return 0;
                case SC_LOADSTATE:
                    fprintf(stderr, "script f=%u: loadstate ignored by snesref (frames still consumed)\n", g_frame);
                    return 0;
                case SC_RESET:
                    if (p_retro_reset) p_retro_reset();
                    else fprintf(stderr, "script f=%u: core has no retro_reset\n", g_frame);
                    return 0;
                default: return 0;
            }
        }
        script_advance();   // trailing idle frame after a hold-type command
        return 0;
    }
}

static bool load_sram_in() {
    const char* path = getenv("SNESREF_SRAM_IN");
    if (!path || !path[0]) return true;
    void* p = p_retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t n = p_retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!p || !n) { fprintf(stderr, "SNESREF_SRAM_IN: core exposes no SAVE_RAM\n"); return false; }
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "SNESREF_SRAM_IN: cannot open %s\n", path); return false; }
    std::vector<uint8_t> buf(n, 0);
    size_t got = fread(buf.data(), 1, n, f);
    int extra = fgetc(f);
    fclose(f);
    if (got != n || extra != EOF)
        fprintf(stderr, "SNESREF_SRAM_IN: file size differs from SAVE_RAM (%zu bytes); loaded %zu\n", n, got);
    memcpy(p, buf.data(), got);
    fprintf(stderr, "[memory] loaded %zu SRAM bytes from %s\n", got, path);
    return true;
}

// ---- save state (9 slots): Shift+Fn = save slot n, Fn = load slot n ----
static void slot_path(int slot, char* out, size_t n) { snprintf(out, n, "mmx_state_%d.bin", slot); }

static void save_state(int slot) {
    size_t n = p_retro_serialize_size(); if(!n) return;
    std::vector<uint8_t> buf(n);
    if (p_retro_serialize(buf.data(), n)) {
        char path[64]; slot_path(slot, path, sizeof path);
        FILE* f=fopen(path,"wb"); if(f){ fwrite(buf.data(),1,n,f); fclose(f); printf("[slot %d SAVED %zu bytes]\n",slot,n); fflush(stdout);} }
}
static void load_state(int slot) {
    char path[64]; slot_path(slot, path, sizeof path);
    FILE* f=fopen(path,"rb"); if(!f){ printf("[slot %d empty]\n",slot); fflush(stdout); return; }
    fseek(f,0,SEEK_END); long fn=ftell(f); fseek(f,0,SEEK_SET);
    size_t need = p_retro_serialize_size();        // size the core expects NOW
    if (fn <= 0) { fclose(f); printf("[slot %d bad file]\n",slot); fflush(stdout); return; }
    size_t bn = ((size_t)fn > need) ? (size_t)fn : need;   // never under-size the buffer
    std::vector<uint8_t> buf(bn, 0);
    fread(buf.data(),1,(size_t)fn,f); fclose(f);
    printf("[slot %d load: file=%ld coreNeeds=%zu]\n",slot,fn,need); fflush(stdout);
    if ((size_t)fn != need)
        printf("[warn slot %d: size mismatch file=%ld need=%zu]\n",slot,fn,need);
    bool ok = p_retro_unserialize(buf.data(), need);   // pass the size the core expects
    printf("[slot %d %s]\n",slot, ok?"LOADED":"unserialize returned FALSE"); fflush(stdout);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: snesref <libretro-core> <rom.sfc>\n");
        return 1;
    }
    const char* corePath = argv[1];
    const char* romPath  = argv[2];
    const char* headless_value = getenv("SNESREF_HEADLESS");
    const bool headless = headless_value && headless_value[0] &&
                          headless_value[0] != '0';
    g_headless = headless;
    { const char* input = getenv("SNESREF_INPUT_FILE");
      if (input && input[0] && !load_input_file(input)) return 6; }
    { const char* script = getenv("SNESREF_SCRIPT");
      if (script && script[0]) {
          if (!load_script(script)) return 6;
          g_scripted_input = true;
      } }
    if (!parse_core_option_env() || !parse_core_options_override()) return 6;

    g_core = core_open(corePath);
    if (!g_core) {
        fprintf(stderr, "cannot load libretro core %s: %s\n",
                corePath, core_error());
        return 2;
    }
    bind(p_retro_init,"retro_init"); bind(p_retro_deinit,"retro_deinit");
    bind(p_retro_api_version,"retro_api_version");
    bind(p_retro_get_system_info,"retro_get_system_info");
    bind(p_retro_get_system_av_info,"retro_get_system_av_info");
    bind(p_retro_set_environment,"retro_set_environment");
    bind(p_retro_set_video_refresh,"retro_set_video_refresh");
    bind(p_retro_set_audio_sample,"retro_set_audio_sample");
    bind(p_retro_set_audio_sample_batch,"retro_set_audio_sample_batch");
    bind(p_retro_set_input_poll,"retro_set_input_poll");
    bind(p_retro_set_input_state,"retro_set_input_state");
    bind(p_retro_set_controller_port_device,"retro_set_controller_port_device");
    bind(p_retro_load_game,"retro_load_game"); bind(p_retro_unload_game,"retro_unload_game");
    bind(p_retro_run,"retro_run");
    bind(p_retro_serialize_size,"retro_serialize_size");
    bind(p_retro_serialize,"retro_serialize"); bind(p_retro_unserialize,"retro_unserialize");
    bind(p_retro_get_memory_data,"retro_get_memory_data");
    bind(p_retro_get_memory_size,"retro_get_memory_size");
    bind_opt(p_retro_reset,"retro_reset");
    bind_opt(p_dbg_version,"snesref_dbg_version");
    bind_opt(p_dbg_set_frame,"snesref_dbg_set_frame");
    bind_opt(p_dbg_cgram,"snesref_dbg_cgram");
    bind_opt(p_dbg_oam,"snesref_dbg_oam");
    bind_opt(p_dbg_regs_json,"snesref_dbg_regs_json");
    bind_opt(p_dbg_ppuw_entry_size,"snesref_dbg_ppuw_entry_size");
    bind_opt(p_dbg_ppuw_capacity,"snesref_dbg_ppuw_capacity");
    bind_opt(p_dbg_ppuw_head,"snesref_dbg_ppuw_head");
    bind_opt(p_dbg_ppuw_read,"snesref_dbg_ppuw_read");
    if (p_dbg_version)
        printf("snesref debug exports: v%u, ppuw ring %zu entries\n", p_dbg_version(),
               p_dbg_ppuw_capacity ? p_dbg_ppuw_capacity() : (size_t)0);
    else
        fprintf(stderr, "warning: core has no snesref_dbg_* exports (stock core); "
                        "cgram/oam/regs/ppuw dumps will be skipped\n");

    p_retro_set_environment(cb_environment);
    p_retro_init();

    retro_system_info si; memset(&si,0,sizeof si); p_retro_get_system_info(&si);
    g_core_name = si.library_name ? si.library_name : "?";
    g_core_version = si.library_version ? si.library_version : "?";
    printf("core: %s %s  need_fullpath=%d\n", g_core_name, g_core_version, si.need_fullpath);
    printf("layers=%02X no_colormath=%d no_clip=%d core_options=%zu declared, %zu overridden\n",
           g_layer_mask, (int)g_no_colormath, (int)g_no_clip, g_opt_defaults.size(), g_opt_overrides.size());
    for (auto& kv : g_opt_overrides)
        if (!g_opt_defaults.count(kv.first))
            fprintf(stderr, "warning: SNESREF_CORE_OPTIONS key '%s' is not declared by the core\n", kv.first.c_str());

    retro_game_info gi; memset(&gi,0,sizeof gi); gi.path=romPath;
    std::vector<uint8_t> rom;
    if (!si.need_fullpath) {
        FILE* f=fopen(romPath,"rb"); if(!f){ fprintf(stderr,"cannot open rom %s\n",romPath); return 3; }
        fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
        rom.resize(n); fread(rom.data(),1,n,f); fclose(f);
        gi.data=rom.data(); gi.size=rom.size();
    }
    p_retro_set_video_refresh(cb_video);
    p_retro_set_audio_sample(cb_audio_sample);
    p_retro_set_audio_sample_batch(cb_audio_batch);
    p_retro_set_input_poll(cb_input_poll);
    p_retro_set_input_state(cb_input_state);
    if (!p_retro_load_game(&gi)) { fprintf(stderr,"retro_load_game failed\n"); return 4; }
    if (!initialize_system_ram()) return 7;
    if (!load_sram_in()) return 7;
    { size_t sn = p_retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
      printf("memory: wram=%zu vram=%zu sram=%zu\n",
             p_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM),
             p_retro_get_memory_size(RETRO_MEMORY_VIDEO_RAM), sn); }
    p_retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    retro_system_av_info av; memset(&av,0,sizeof av); p_retro_get_system_av_info(&av);
    int vw=(int)av.geometry.base_width, vh=(int)av.geometry.base_height;
    if(vw<=0)vw=256;
    if(vh<=0)vh=224;

    printf("core timing: fps=%.4f sample_rate=%.2f\n", av.timing.fps, av.timing.sample_rate);
    { const char* wp = getenv("SNESREF_WAV");
      wav_open(wp && wp[0] ? wp : "snesref_audio.wav",
               av.timing.sample_rate > 0 ? av.timing.sample_rate : 32040.0); }
    long quit_frames = 0;
    { const char* qf = getenv("SNESREF_QUIT_FRAMES");
      if (qf && qf[0]) quit_frames = atol(qf); }
    const char* fast_value = getenv("SNESREF_FAST");
    bool fast = fast_value && fast_value[0] && fast_value[0] != '0';

    SDL_SetMainReady();
    Uint32 sdl_flags = headless
        ? SDL_INIT_TIMER
        : SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK;
    if (SDL_Init(sdl_flags) != 0) { fprintf(stderr,"SDL_Init: %s\n",SDL_GetError()); return 5; }
    if (!headless) {
        open_first_pad();
        g_win = SDL_CreateWindow("snesref (libretro) — Fn load / Shift+Fn save / Backspace clear-trace",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, vw*2, vh*2,
            fast ? SDL_WINDOW_HIDDEN : SDL_WINDOW_RESIZABLE);
        g_ren = SDL_CreateRenderer(g_win, -1,
            fast ? SDL_RENDERER_ACCELERATED
                 : SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!g_ren)
            g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
        if (!g_win || !g_ren) {
            fprintf(stderr, "SDL renderer setup failed: %s\n", SDL_GetError());
            return 5;
        }
        SDL_RenderSetLogicalSize(g_ren, vw, vh);
    }

    printf("RUN. KB: arrows=DPad Z=B(jump) X=A A=Y(fire) S=X C=L V=R Enter=Start RShift=Select\n");
    printf("     Pad: dpad/L-stick, Cross=jump Square=fire Circle=A Triangle=X L1/R1=L/R Start/Select\n");
    printf("     States: Fn=LOAD slot n, Shift+Fn=SAVE slot n (1-9) | Backspace=clear trace | Esc=quit\n");
    fflush(stdout);

    bool running=true;
    Uint64 freq=SDL_GetPerformanceFrequency(), prev=SDL_GetPerformanceCounter();
    const double target = (double)freq / 60.098;
    while (running) {
        if (!headless) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type==SDL_QUIT) running=false;
                else if (e.type==SDL_CONTROLLERDEVICEADDED) open_first_pad();
                else if (e.type==SDL_CONTROLLERDEVICEREMOVED) { if(g_pad){ SDL_GameControllerClose(g_pad); g_pad=nullptr; printf("[controller removed]\n"); fflush(stdout);} open_first_pad(); }
                else if (e.type==SDL_KEYDOWN && e.key.repeat==0) {
                    SDL_Scancode s = e.key.keysym.scancode;
                    if (s==SDL_SCANCODE_ESCAPE) running=false;
                    else if (s==SDL_SCANCODE_BACKSPACE) clear_trace();
                    else if (s>=SDL_SCANCODE_F1 && s<=SDL_SCANCODE_F9) {
                        int slot = (int)(s - SDL_SCANCODE_F1) + 1;
                        if (e.key.keysym.mod & KMOD_SHIFT) save_state(slot); else load_state(slot);
                    }
                }
            }
        }
        if (g_script_active) {
            g_script_frame_mask = script_tick();
            if (g_script_exit >= 0) break;
            if (g_sc_idx >= g_script.size() && headless && quit_frames <= 0 && !getenv("SNESREF_FRAMES")) {
                printf("script f=%u end of script without quit -- exiting (headless)\n", g_frame);
                g_script_exit = 0;
                break;
            }
        }
        update_scripted_input();
        if (p_dbg_set_frame) p_dbg_set_frame(g_frame + 1);   // journal tag = this run's frame number
        p_retro_run();
        g_frame++;
        trace_tick();
        apuram_trace_tick();
        dspreg_trace_tick();
        if (quit_frames > 0 && g_frame >= (uint32_t)quit_frames) running = false;
        // headless self-test: MMX_SELFTEST=1 -> save@200, load@400, quit@600
        { static int st=-1; if(st<0){const char*v=getenv("MMX_SELFTEST"); st=(v&&v[0]&&v[0]!='0')?1:0;}
          if(st){ if(g_frame==200){printf("[selftest] saving slot9 @f200\n");fflush(stdout);save_state(9);}
                  else if(g_frame==400){printf("[selftest] loading slot9 @f400\n");fflush(stdout);load_state(9);}
                  else if(g_frame==410){printf("[selftest] SURVIVED load, still running @f410\n");fflush(stdout);}
                  else if(g_frame>=600){running=false;} } }
        // headless deterministic capture: SNESREF_FRAMES=N -> run N frames, no
        // input (attract/boot is self-driving), then quit. For diff captures.
        { static long fr=-2; if(fr==-2){const char*v=getenv("SNESREF_FRAMES"); fr=(v&&v[0])?atol(v):-1;}
          if(fr>0 && (long)g_frame>=fr){ if(g_log)fflush(g_log); running=false; } }
        // 60fps cap (disabled only for deterministic offline capture).
        if (!fast && !headless) {
            for (;;) {
                Uint64 now=SDL_GetPerformanceCounter();
                double el=(double)(now-prev);
                if (el>=target) { prev=now; break; }
                double rem_ms=(target-el)*1000.0/(double)freq;
                if (rem_ms>1.5) SDL_Delay((Uint32)(rem_ms-1.0)); // else busy-spin
            }
        }
    }
    if (g_log) fflush(g_log);
    if (!dump_system_ram()) {
        fprintf(stderr, "unable to write SNESREF_WRAM_DUMP\n");
    }
    wav_close();
    p_retro_unload_game(); p_retro_deinit();
    SDL_Quit(); core_close(g_core);
    fflush(stdout);
    return g_script_exit > 0 ? g_script_exit : 0;
}
