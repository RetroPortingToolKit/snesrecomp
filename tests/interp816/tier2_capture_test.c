#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tier2_capture.h"

#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); \
                   fputc('\n', stderr); return 1; } \
} while (0)

static void set_env_value(const char *name, const char *value) {
#ifdef _WIN32
    if (value)
        _putenv_s(name, value);
    else
        _putenv_s(name, "");
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

static int run_disabled_case(void) {
    const char *manifest = "tier2_capture_disabled.json";
    const char *journal = "tier2_capture_disabled.jsonl";
    remove(manifest);
    remove(journal);

    set_env_value("SNESRECOMP_TIER2_MANIFEST", manifest);
    set_env_value("SNESRECOMP_TIER2_JOURNAL", NULL);
    set_env_value("SNESRECOMP_TIER2_CAPTURE", NULL);
    set_env_value("SNESRECOMP_TIER2", NULL);

    CHECK(strcmp(tier2_capture_manifest_path("QA Game"), manifest) == 0,
          "explicit manifest path changed");
    CHECK(strcmp(tier2_capture_journal_path("QA Game"), journal) == 0,
          "derived journal path changed");
    CHECK(tier2_capture_append_discovery(
              "QA Game", 0x807000, 0xC00000, "M1X1", "dispatch", 1, 1),
          "disabled capture should be a successful no-op");
    FILE *disabled = fopen(journal, "r");
    CHECK(disabled == NULL, "disabled capture created journal");
    puts("tier2_capture_test disabled: PASS");
    return 0;
}

static int checkpoint_calls;
static void checkpoint(void) {
    checkpoint_calls++;
    tier2_capture_write_cost_checkpoint("Lifecycle");
}

static int run_lifecycle_case(void) {
    const char *path = "tier2_lifecycle.json";
    const char *rom = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    remove(path);
    set_env_value("SNESRECOMP_TIER2_MANIFEST", path);
    set_env_value("SNESRECOMP_TIER2_JOURNAL", NULL);
    set_env_value("SNESRECOMP_TIER2_CAPTURE", NULL);
    set_env_value("SNESRECOMP_TIER2", NULL);
    tier2_capture_set_identity(rom, "main", rom, "sa1");
    tier2_capture_set_checkpoint_hook(checkpoint);
    tier2_capture_manifest_path("Lifecycle"); /* inspect while disabled */
    tier2_capture_set_selection(1);
    tier2_capture_instruction(0xc08000, 1, 0, 3);
    tier2_capture_cpu_instruction(1, 0xc08000, 1, 0, 5);
    tier2_capture_set_selection(0);
    CHECK(checkpoint_calls == 1, "turning capture off must seal its last costs");
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "disable did not write the checkpoint");
    char data[4096]; size_t count = fread(data, 1, sizeof data - 1, f);
    data[count] = 0; fclose(f);
    CHECK(strstr(data, "\"processor\":\"sa1\"") && strstr(data, "\"processor\":\"snes_cpu\""),
          "processors lost their separate instruction costs");
    CHECK(!strstr(data, "\"build_digest\":\"\""), "executable digest was not captured");
    char capture_id[160], archive[512];
    char *id = strstr(data, "\"capture_id\":\"");
    CHECK(id && sscanf(id, "\"capture_id\":\"%159[^\"]", capture_id) == 1,
          "missing session id");
    tier2_capture_set_identity(rom, "main", rom, "sa1");
    snprintf(archive, sizeof archive, "%s.%s.previous.json", path, capture_id);
    f = fopen(archive, "rb");
    CHECK(f != NULL, "same-ROM reset lost the old checkpoint");
    fclose(f);
    tier2_capture_set_selection(1);
    tier2_capture_instruction(0xc09000, 3, 0, 2);
    tier2_capture_set_selection(0);
    f = fopen(path, "rb"); CHECK(f != NULL, "second session checkpoint missing");
    count = fread(data, 1, sizeof data - 1, f); data[count] = 0; fclose(f);
    CHECK(!strstr(data, "\"processor\":\"sa1\""), "old costs leaked into new session");
    remove(path); remove(archive);
    puts("tier2_capture_test lifecycle: PASS");
    return 0;
}

int main(int argc, char **argv) {
    const char *manifest = "tier2_capture_contract.json";
    const char *journal = "tier2_capture_contract.jsonl";
    if (argc == 2 && strcmp(argv[1], "disabled") == 0)
        return run_disabled_case();
    if (argc == 2 && strcmp(argv[1], "lifecycle") == 0)
        return run_lifecycle_case();
    CHECK(argc == 1, "usage: tier2_capture_test [disabled]");
    remove(manifest);
    remove(journal);

    set_env_value("SNESRECOMP_TIER2_MANIFEST", manifest);
    set_env_value("SNESRECOMP_TIER2_JOURNAL", journal);
    set_env_value("SNESRECOMP_TIER2_CAPTURE", NULL);
    set_env_value("SNESRECOMP_TIER2", NULL);

    CHECK(strcmp(tier2_capture_manifest_path("QA Game"), manifest) == 0,
          "explicit manifest path changed");
    CHECK(strcmp(tier2_capture_journal_path("QA Game"), journal) == 0,
          "explicit journal path changed");

    FILE *seed = fopen(journal, "w");
    CHECK(seed != NULL, "cannot seed %s", journal);
    fputs("{\"sentinel\":true}\n", seed);
    CHECK(fclose(seed) == 0, "cannot close seed journal");

    CHECK(tier2_capture_append_discovery(
              "QA Game", 0x808000, 0xC01234, "M1X1", "call_gap", 1, 12),
          "first append failed");
    tier2_capture_close();              /* model a session boundary */
    CHECK(tier2_capture_append_discovery(
              "QA Game", 0x808100, 0xC05678, "M0X1", "bank_miss", 0, 34),
          "second-session append failed");
    CHECK(tier2_capture_append_discovery(
              "QA Game", 0x808200, 0xC09ABC, "M1X0", "dispatch", -1, 56),
          "pre-execution append failed");
    tier2_capture_close();

    FILE *in = fopen(journal, "r");
    CHECK(in != NULL, "cannot reopen journal");
    char line[1024];
    int lines = 0;
    int discoveries = 0;
    int pending = 0;
    while (fgets(line, sizeof line, in)) {
        lines++;
        size_t n = strlen(line);
        CHECK(n && line[n - 1] == '\n', "line %d is torn", lines);
        if (strstr(line, "snesrecomp tier2 discovery v2")) discoveries++;
        if (strstr(line, "\"pending_hits\":1")) pending++;
    }
    fclose(in);
    CHECK(lines == 4, "append contract lost prior data: got %d lines", lines);
    CHECK(discoveries == 3, "expected 3 complete discoveries, got %d", discoveries);
    CHECK(pending == 1, "crash-window discovery was not marked pending");
    remove(journal);

    puts("tier2_capture_test: PASS");
    return 0;
}
