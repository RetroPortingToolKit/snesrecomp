#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "tier2_capture.h"
#include "../sha256.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <process.h>
#define capture_pid _getpid
#define capture_mkdir(p) _mkdir(p)
#else
#include <unistd.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#define capture_pid getpid
#define capture_mkdir(p) mkdir(p, 0700)
#endif

#ifndef SNESRECOMP_EXPOSE_COVERAGE_MOD
#define SNESRECOMP_EXPOSE_COVERAGE_MOD 0
#endif
#define PATH_CAP 1024
#define COST_CAP 131072u
static char s_manifest[PATH_CAP], s_journal_path[PATH_CAP], s_capture_id[160];
static char s_rom[65], s_module[128] = "main", s_program[65], s_mapper[32] = "unknown";
static char s_build[65];
static char s_executable_digest[65];
static FILE *s_journal;
static int s_paths_ready, s_paths_disabled, s_close_registered, s_journal_failed;
static int s_preserve_explicit;
static void (*s_checkpoint_hook)(void);
void tier2_capture_set_checkpoint_hook(void (*hook)(void)) { s_checkpoint_hook = hook; }
static int s_default_enabled, s_selection, s_config = -1, s_launch = -1;
static int s_exposed = SNESRECOMP_EXPOSE_COVERAGE_MOD, s_initialized;
static const char *s_source = "default";
static uint64_t s_sequence, s_dropped_costs;
int g_tier2_capture_active;
static const char *(*s_entry_probe)(uint32_t, uint8_t);
void tier2_capture_set_entry_probe(const char *(*probe)(uint32_t, uint8_t)) { s_entry_probe = probe; }
const char *tier2_capture_entry_reason(uint32_t pc, uint8_t mx) {
    return s_entry_probe ? s_entry_probe(pc, mx) : "exact_entry_unavailable";
}

typedef struct {
    uint32_t pc;
    uint8_t mx, emulation, used, processor;
    uint64_t instructions, cycles;
} Cost;
static Cost *s_costs;

static int parse_bool(const char *s) {
    if (!s || !*s) return -1;
    char lower[8]; size_t n = strlen(s);
    if (n >= sizeof lower) return -1;
    for (size_t i = 0; i <= n; ++i) lower[i] = (char)tolower((unsigned char)s[i]);
    s = lower;
    if (!strcmp(s, "1") || !strcmp(s, "true") || !strcmp(s, "on") ||
        !strcmp(s, "yes")) return 1;
    if (!strcmp(s, "0") || !strcmp(s, "false") || !strcmp(s, "off") ||
        !strcmp(s, "no")) return 0;
    fprintf(stderr, "[coverage] invalid boolean '%s'; override ignored\n", s);
    return -1;
}

static void refresh(void) {
    int enabled = s_default_enabled || s_selection;
    s_source = s_selection ? "mod" : "default";
    if (s_config >= 0) { enabled = s_config; s_source = "config"; }
    const char *v = getenv("SNESRECOMP_TIER2_CAPTURE");
    if (!v || !*v) v = getenv("SNESRECOMP_TIER2");
    int env = parse_bool(v);
    if (env >= 0) { enabled = env; s_source = "environment"; }
    else {
        v = getenv("SNESRECOMP_TIER2_JOURNAL");
        if (v && *v && strcmp(v, "0")) { enabled = 1; s_source = "journal environment"; }
    }
    if (s_launch >= 0) { enabled = s_launch; s_source = "launch"; }
    if (g_tier2_capture_active && !enabled) {
        if (s_checkpoint_hook) s_checkpoint_hook();
        tier2_capture_flush();
    }
    /* The launcher may inspect the destination before capture is enabled.
     * That inspection must not prevent creation of the directories later. */
    if (enabled && s_paths_disabled) {
        s_paths_ready = s_paths_disabled = 0;
    }
    g_tier2_capture_active = enabled;
    s_initialized = 1;
}

void tier2_capture_set_default_enabled(int enabled) {
    s_default_enabled = !!enabled;
    refresh();
}
void tier2_capture_configure(int exposed, int config_enabled, int launch_enabled) {
    s_exposed = SNESRECOMP_EXPOSE_COVERAGE_MOD || exposed;
    s_config = config_enabled;
    s_launch = launch_enabled;
    refresh();
}
void tier2_capture_set_selection(int enabled) { s_selection = !!enabled; refresh(); }
int tier2_capture_exposed(void) { return s_exposed; }
int tier2_capture_enabled(void) { if (!s_initialized) refresh(); return g_tier2_capture_active; }
int tier2_capture_has_identity(void) { return s_rom[0] != 0; }
const char *tier2_capture_setting_source(void) { return s_source; }
uint64_t tier2_capture_next_sequence(void) { return ++s_sequence; }
uint64_t tier2_capture_dropped_costs(void) { return s_dropped_costs; }
int tier2_capture_journal_failed(void) { return s_journal_failed; }

static void safe_text(const char *s, char *out, size_t cap, int filename) {
    size_t n = 0;
    if (!s) s = "";
    for (; *s && n + 1 < cap; ++s) {
        unsigned char c = (unsigned char)*s;
        int ok = filename ? ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-')
                          : (c >= 32 && c != '"' && c != '\\');
        out[n++] = ok ? (char)c : '_';
    }
    out[n] = 0;
}

void tier2_capture_set_build_digest(const char *digest) {
    safe_text(digest, s_build, sizeof s_build, 0);
}

static void identify_executable(void) {
    if (s_executable_digest[0]) return;
    char path[PATH_CAP];
#ifdef _WIN32
    DWORD count = GetModuleFileNameA(NULL, path, sizeof path);
    if (!count || count >= sizeof path) return;
#elif defined(__APPLE__)
    uint32_t count = sizeof path;
    if (_NSGetExecutablePath(path, &count)) return;
#else
    ssize_t count = readlink("/proc/self/exe", path, sizeof path - 1);
    if (count <= 0 || (size_t)count >= sizeof path - 1) return;
    path[count] = 0;
#endif
    FILE *f = fopen(path, "rb");
    if (!f) return;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return; }
    long size = ftell(f);
    if (size <= 0 || size > 256 * 1024 * 1024 || fseek(f, 0, SEEK_SET)) {
        fclose(f); return;
    }
    uint8_t *bytes = (uint8_t *)malloc((size_t)size);
    if (!bytes) { fclose(f); return; }
    if (fread(bytes, 1, (size_t)size, f) == (size_t)size) {
        uint8_t digest[32];
        sha256_compute(bytes, (size_t)size, digest);
        for (unsigned i = 0; i < 32; ++i)
            snprintf(s_executable_digest + 2*i, 3, "%02x", digest[i]);
    }
    free(bytes);
    fclose(f);
}

void tier2_capture_set_identity(const char *rom, const char *module,
                               const char *program, const char *mapper) {
    /* A session must never contain observations for two images. The caller
     * seals/reset its tables before changing an already established identity. */
    { /* A new machine is a new capture even when the image is unchanged. */
        tier2_capture_close();
        /* Explicit output names otherwise overwrite the last session's
         * exclusive costs on reset/rematch. Journals alone cannot restore
         * those costs. Default paths are already unique per session. */
        const char *explicit_path = getenv("SNESRECOMP_TIER2_MANIFEST");
        if (s_rom[0] && s_paths_ready && s_manifest[0] && explicit_path && *explicit_path) {
            FILE *previous = fopen(s_manifest, "rb");
            if (previous) {
                char archive[PATH_CAP];
                fclose(previous);
                if (snprintf(archive, sizeof archive, "%s.%s.previous.json", s_manifest, s_capture_id) >= (int)sizeof archive ||
                    rename(s_manifest, archive)) {
                    fprintf(stderr, "[coverage] could not archive prior checkpoint: %s\n", s_manifest);
                    s_preserve_explicit = 1;
                }
            }
        }
        s_paths_ready = 0;
        s_manifest[0] = s_journal_path[0] = 0;
        free(s_costs); s_costs = NULL; s_dropped_costs = 0;
    }
    safe_text(rom, s_rom, sizeof s_rom, 0);
    safe_text(module ? module : "main", s_module, sizeof s_module, 0);
    safe_text(program, s_program, sizeof s_program, 0);
    safe_text(mapper, s_mapper, sizeof s_mapper, 0);
}

static void init_paths(const char *title) {
    if (s_paths_ready) return;
    int enabled = tier2_capture_enabled();
    s_paths_ready = 1;
    s_paths_disabled = !enabled;
    if (enabled) identify_executable();
    s_journal_failed = 0;
    static unsigned session;
    char id[80];
    safe_text(title ? title : "unknown", id, sizeof id, 1);
    snprintf(s_capture_id, sizeof s_capture_id, "%s_%lld_p%ld_s%u",
             id, (long long)time(NULL), (long)capture_pid(), ++session);
    const char *path = getenv("SNESRECOMP_TIER2_MANIFEST");
    if (path && *path) {
        if (s_preserve_explicit) {
            if (snprintf(s_manifest, sizeof s_manifest, "%s.%s.json", path, s_capture_id) >= (int)sizeof s_manifest) goto bad_path;
            s_preserve_explicit = 0;
        } else {
            if (strlen(path) >= sizeof s_manifest) goto bad_path;
            strcpy(s_manifest, path);
        }
    } else {
        char root[PATH_CAP], dir[PATH_CAP];
        const char *base = getenv("SNESRECOMP_COVERAGE_DIR");
        if (base && *base) snprintf(root, sizeof root, "%s", base);
        else {
#ifdef _WIN32
            base = getenv("LOCALAPPDATA");
            snprintf(root, sizeof root, "%s%s", base && *base ? base : ".",
                     "/snesrecomp-coverage");
#else
            base = getenv("XDG_STATE_HOME");
            if (base && *base) snprintf(root, sizeof root, "%s/snesrecomp-coverage", base);
            else {
                base = getenv("HOME");
                snprintf(root, sizeof root, "%s/.snesrecomp-coverage", base && *base ? base : ".");
            }
#endif
        }
        if (enabled && capture_mkdir(root) && errno != EEXIST) goto bad_path;
        if (snprintf(dir, sizeof dir, "%s/%s", root, id) >= (int)sizeof dir) goto bad_path;
        if (enabled && capture_mkdir(dir) && errno != EEXIST) goto bad_path;
        if (snprintf(s_manifest, sizeof s_manifest, "%s/%s.json", dir, s_capture_id)
                >= (int)sizeof s_manifest) goto bad_path;
    }
    path = getenv("SNESRECOMP_TIER2_JOURNAL");
    if (path && *path && strcmp(path, "0")) {
        if (strlen(path) >= sizeof s_journal_path) goto bad_path;
        strcpy(s_journal_path, path);
    } else {
        size_t n = strlen(s_manifest);
        if (n >= 5 && !strcmp(s_manifest + n - 5, ".json")) n -= 5;
        if (n + 7 >= sizeof s_journal_path) goto bad_path;
        memcpy(s_journal_path, s_manifest, n);
        strcpy(s_journal_path + n, ".jsonl");
    }
    fprintf(stderr, "[coverage] %s (%s); export: %s\n",
            enabled ? "enabled" : "disabled", s_source, s_manifest);
    return;
bad_path:
    s_manifest[0] = s_journal_path[0] = 0;
    s_journal_failed = 1;
    fprintf(stderr, "[coverage] cannot create capture directory/path\n");
}
const char *tier2_capture_manifest_path(const char *title) { init_paths(title); return s_manifest; }
const char *tier2_capture_journal_path(const char *title) { init_paths(title); return s_journal_path; }

void tier2_capture_write_header(FILE *f, const char *title, int journal) {
    char safe[128];
    init_paths(title);
    safe_text(title, safe, sizeof safe, 0);
    fprintf(f, "{\"schema\":\"snesrecomp tier2 %s v2\",\"capture_id\":\"%s\","
               "\"rom_title\":\"%s\",\"sequence\":%llu,\"identity\":{"
               "\"rom_sha256\":\"%s\",\"module_id\":\"%s\","
               "\"program_digest\":\"%s\",\"build_digest\":\"%s\","
               "\"generation_digest\":\"%s\",\"mapper\":\"%s\"},",
            journal ? "discovery" : "coverage", s_capture_id, safe,
            (unsigned long long)s_sequence, s_rom, s_module, s_program,
            s_executable_digest, s_build, s_mapper);
}
FILE *tier2_capture_journal(const char *title) {
    if (!tier2_capture_enabled()) return NULL;
    init_paths(title);
    if (s_journal_failed) return NULL;
    if (!s_journal) {
        s_journal = fopen(s_journal_path, "a");
        if (!s_journal) {
            fprintf(stderr, "[coverage] cannot open journal: %s\n", s_journal_path);
            s_journal_failed = 1;
            return NULL;
        }
        setvbuf(s_journal, NULL, _IOFBF, 65536);
        if (!s_close_registered) { atexit(tier2_capture_close); s_close_registered = 1; }
    }
    return s_journal;
}
void tier2_capture_flush(void) {
    if (s_journal && fflush(s_journal)) {
        fprintf(stderr, "[coverage] journal flush failed\n");
        s_journal_failed = 1;
    }
}
void tier2_capture_close(void) {
    if (s_journal) {
        if (fclose(s_journal)) { s_journal_failed = 1; fprintf(stderr, "[coverage] journal close failed\n"); }
        s_journal = NULL;
    }
}
int tier2_capture_replace(const char *temp, const char *path) {
#ifdef _WIN32
    return MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(temp, path) == 0;
#endif
}

void tier2_capture_instruction(uint32_t pc, uint8_t mx, uint8_t emulation, unsigned cycles) {
    tier2_capture_cpu_instruction(0, pc, mx, emulation, cycles);
}
void tier2_capture_cpu_instruction(uint8_t processor, uint32_t pc, uint8_t mx, uint8_t emulation, unsigned cycles) {
    if (!g_tier2_capture_active) return;
    if (!s_costs) {
        s_costs = (Cost *)calloc(COST_CAP, sizeof(Cost));
        if (!s_costs) { ++s_dropped_costs; return; }
    }
    uint32_t h = (pc * 2654435761u + mx * 17u + emulation + processor * 97u) & (COST_CAP - 1);
    /* Bounded probing, including on full tables. Costs are exclusive executed
     * opcodes, never inclusive nested calls, so AOT bounces cannot double count. */
    for (unsigned probe = 0; probe < 32; ++probe, h = (h + 1) & (COST_CAP - 1)) {
        Cost *c = &s_costs[h];
        if (!c->used) { c->used = 1; c->pc = pc; c->mx = mx; c->emulation = emulation; c->processor = processor; }
        if (c->pc == pc && c->mx == mx && c->emulation == emulation && c->processor == processor) {
            c->instructions++; c->cycles += cycles; return;
        }
    }
    ++s_dropped_costs;
}
void tier2_capture_write_costs(FILE *f) {
    fprintf(f, "\"dropped_cost_samples\":%llu,\"costs\":[",
            (unsigned long long)s_dropped_costs);
    int comma = 0;
    for (unsigned i = 0; s_costs && i < COST_CAP; ++i) {
        const Cost *c = &s_costs[i];
        if (!c->used) continue;
        fprintf(f, "%s{\"record_kind\":\"instruction\",\"site_pc24\":0,"
                "\"processor\":\"%s\",\"target_pc24\":\"0x%06X\",\"entry_mx\":\"M%uX%u\","
                "\"emulation\":%u,\"interpreted_instructions\":%llu,\"guest_cycles\":%llu}",
                comma++ ? "," : "", c->processor ? "sa1" : "snes_cpu", c->pc, c->mx >> 1, c->mx & 1, c->emulation,
                (unsigned long long)c->instructions, (unsigned long long)c->cycles);
    }
    fprintf(f, "],");
}

int tier2_capture_write_cost_checkpoint(const char *title) {
    if (!tier2_capture_enabled()) return 1;
    const char *path = tier2_capture_manifest_path(title);
    char temp[PATH_CAP];
    if (!*path || snprintf(temp, sizeof temp, "%s.tmp", path) >= (int)sizeof temp) return 0;
    FILE *f = fopen(temp, "w");
    if (!f) return 0;
    tier2_capture_next_sequence();
    tier2_capture_write_header(f, title, 0);
    tier2_capture_write_costs(f);
    fputs("\"capture_scope\":\"instruction_costs_only\",\"discoveries\":[],"
          "\"checkpoint_complete\":true}\n", f);
    int failed = ferror(f);
    if (fclose(f)) failed = 1;
    if (failed || !tier2_capture_replace(temp, path)) {
        ++s_journal_failed;
        return 0;
    }
    return 1;
}

/* Legacy entry point retained for third-party hosts. These are observations,
 * not verified completions; the bridge uses cumulative v2 rows instead. */
int tier2_capture_append_discovery(const char *title, uint32_t site, uint32_t target,
        const char *mx, const char *kind, int outcome, int32_t frame) {
    if (!tier2_capture_enabled()) return 1;
    FILE *f = tier2_capture_journal(title);
    if (!f) return 0;
    tier2_capture_next_sequence();
    tier2_capture_write_header(f, title, 1);
    fprintf(f, "\"row\":{\"site_pc24\":\"0x%06X\",\"target_pc24\":\"0x%06X\","
               "\"entry_mx\":\"%s\",\"site_kind\":\"%s\",\"observed_hits\":%d,"
               "\"bail_hits\":%d,\"pending_hits\":%d,\"first_frame\":%d,\"last_frame\":%d}}\n",
            site & 0xFFFFFFu, target & 0xFFFFFFu, mx, kind, outcome > 0,
            outcome == 0, outcome < 0, (int)frame, (int)frame);
    return !ferror(f);
}
