#include "content_pack.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char *error, size_t cap, const char *message) {
    if (error && cap) snprintf(error, cap, "%s", message);
    return 0;
}
int cp_id_valid(const char *id) {
    if (!id || !*id || strlen(id) >= CP_ID) return 0;
    for (; *id; ++id)
        if (!(*id >= 'a' && *id <= 'z') && !(*id >= '0' && *id <= '9') && *id != '-') return 0;
    return 1;
}
int cp_hash_parse(const char *hex, uint8_t hash[32]) {
    static const char digits[] = "0123456789abcdef";
    uint8_t value[32] = {0};
    if (strlen(hex) != 64) return 0;
    for (unsigned i = 0; i < 64; ++i) {
        const char *p = strchr(digits, hex[i]);
        if (!p) return 0;
        value[i / 2] = (uint8_t)((value[i / 2] << 4) | (p - digits));
    }
    memcpy(hash, value, 32); return 1;
}
void cp_hash_format(const uint8_t hash[32], char hex[65]) {
    for (unsigned i = 0; i < 32; ++i) {
        hex[i * 2] = "0123456789abcdef"[hash[i] >> 4];
        hex[i * 2 + 1] = "0123456789abcdef"[hash[i] & 15];
    }
    hex[64] = 0;
}
static int copy(char *out, size_t cap, const char *value) {
    if (!*value || strlen(value) >= cap) return 0;
    for (const char *p = value; *p; ++p) if ((unsigned char)*p < 32 || *p == '|') return 0;
    strcpy(out, value); return 1;
}
static int slot(const char *text, unsigned *out) {
    if (!*text) return 0;
    unsigned n = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9' || n > 255) return 0;
        n = n * 10 + (unsigned)(*text - '0');
    }
    if (n > 255) return 0;
    *out = n; return 1;
}
static int fields(char *text, char **out, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        out[i] = text;
        char *next = strchr(text, '|');
        if ((i + 1 < count) != (next != NULL)) return 0;
        if (next) { *next = 0; text = next + 1; }
    }
    return 1;
}
int cp_manifest_read(const char *path, CpPack *out, char *error, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return fail(error, cap, "Cannot open content manifest");
    CpPack *p = calloc(1, sizeof(*p));
    if (!p) { fclose(f); return fail(error, cap, "Out of memory"); }
    char line[1024]; unsigned seen = 0; int ok = 1;
    while (ok && fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        if (!n || (n == sizeof(line) - 1 && line[n-1] != '\n')) { ok = 0; break; }
        while (n && (line[n-1] == '\r' || line[n-1] == '\n')) line[--n] = 0;
        if (!n || line[0] == '#') continue;
        char *value = strchr(line, '=');
        if (!value) { ok = 0; break; }
        *value++ = 0;
        const char *keys[] = {"format", "id", "name", "author", "adapter", "source_sha256", "target_sha256"};
        unsigned key;
        for (key = 0; key < 7 && strcmp(line, keys[key]); ++key) {}
        if (key < 7) {
            if (seen & (1u << key)) { ok = 0; break; }
            seen |= 1u << key;
            switch (key) {
            case 0: ok = !strcmp(value, "1"); break;
            case 1: ok = cp_id_valid(value) && copy(p->id, sizeof(p->id), value); break;
            case 2: ok = copy(p->name, sizeof(p->name), value); break;
            case 3: ok = copy(p->author, sizeof(p->author), value); break;
            case 4: ok = cp_id_valid(value) && copy(p->adapter, sizeof(p->adapter), value); break;
            case 5: ok = cp_hash_parse(value, p->source_hash); break;
            case 6: ok = cp_hash_parse(value, p->target_hash); break;
            }
        } else if (!strcmp(line, "cup") && p->cup_count < CP_CUPS) {
            CpCup *c = &p->cups[p->cup_count++]; char *v[3];
            ok = fields(value, v, 3) && cp_id_valid(v[0]) && copy(c->id, sizeof(c->id), v[0]) &&
                 copy(c->name, sizeof(c->name), v[1]) && slot(v[2], &c->slot);
        } else if (!strcmp(line, "track") && p->track_count < CP_TRACKS) {
            CpTrack *t = &p->tracks[p->track_count++]; char *v[4];
            ok = fields(value, v, 4) && cp_id_valid(v[0]) && copy(t->id, sizeof(t->id), v[0]) &&
                 copy(t->name, sizeof(t->name), v[1]) && cp_id_valid(v[2]) &&
                 copy(t->cup, sizeof(t->cup), v[2]) && slot(v[3], &t->slot);
        } else ok = 0;
    }
    if (ferror(f)) ok = 0;
    fclose(f);
    if (seen != 127 || !p->cup_count || !p->track_count) ok = 0;
    for (unsigned i = 0; ok && i < p->cup_count; ++i) {
        unsigned tracks = 0;
        for (unsigned j = 0; j < i; ++j) if (!strcmp(p->cups[i].id, p->cups[j].id)) ok = 0;
        for (unsigned j = 0; j < p->track_count; ++j) tracks += !strcmp(p->tracks[j].cup, p->cups[i].id);
        if (!tracks) ok = 0;
    }
    for (unsigned i = 0; ok && i < p->track_count; ++i) {
        int found = 0;
        for (unsigned j = 0; j < p->cup_count; ++j) found |= !strcmp(p->tracks[i].cup, p->cups[j].id);
        if (!found) ok = 0;
        for (unsigned j = 0; j < i; ++j) if (!strcmp(p->tracks[i].id, p->tracks[j].id)) ok = 0;
    }
    if (ok) *out = *p;
    free(p);
    return ok ? 1 : fail(error, cap, "Invalid content manifest: fields, IDs, limits or cup/track references");
}
const CpPack *cp_catalog_find(const CpCatalog *c, const char *id) {
    for (unsigned i = 0; i < c->count; ++i) if (!strcmp(c->packs[i]->id, id)) return c->packs[i];
    return NULL;
}
int cp_catalog_add(CpCatalog *c, const CpPack *p, char *error, size_t cap) {
    if (!cp_id_valid(p->id) || c->count >= CP_PACKS || cp_catalog_find(c, p->id))
        return fail(error, cap, "Duplicate/reserved pack ID or catalog limit reached");
    CpPack *owned = malloc(sizeof(*owned));
    if (!owned) return fail(error, cap, "Out of memory");
    *owned = *p;
    unsigned i = c->count++;
    while (i && strcmp(c->packs[i-1]->id, p->id) > 0) { c->packs[i] = c->packs[i-1]; --i; }
    c->packs[i] = owned; return 1;
}
void cp_catalog_free(CpCatalog *c) {
    for (unsigned i = 0; i < c->count; ++i) free(c->packs[i]);
    memset(c, 0, sizeof(*c));
}
const CpCup *cp_catalog_cup(const CpCatalog *c, const char *key, const CpPack **pack) {
    char id[CP_ID]; const char *slash = strchr(key, '/');
    if (!slash || slash == key || (size_t)(slash-key) >= sizeof(id)) return NULL;
    memcpy(id, key, (size_t)(slash-key)); id[slash-key] = 0;
    const CpPack *p = cp_catalog_find(c, id);
    if (p) for (unsigned i = 0; i < p->cup_count; ++i) if (!strcmp(slash+1, p->cups[i].id)) {
        if (pack) *pack = p;
        return &p->cups[i];
    }
    return NULL;
}
int cp_pack_apply(const CpPack *p, const uint8_t *source, size_t size,
                  const char *path, uint8_t **out, size_t *out_size, char *error, size_t cap) {
    if (!source || !size || size > CP_ROM_LIMIT) return fail(error, cap, "Invalid source ROM size");
    uint8_t hash[32]; sha256_compute(source, size, hash);
    if (memcmp(hash, p->source_hash, 32)) return fail(error, cap, "Patch requires a different source ROM");
    FILE *f = fopen(path, "rb");
    if (!f) return fail(error, cap, "Selected patch is missing; locate it in Track Packs");
    uint8_t *patch = NULL, *target = NULL; size_t target_size = 0; int ok = 0;
    if (fseek(f, 0, SEEK_END)) goto done;
    long length = ftell(f);
    if (length < 8 || length > CP_ROM_LIMIT * 2 || fseek(f, 0, SEEK_SET)) goto done;
    patch = malloc((size_t)length);
    if (!patch || fread(patch, 1, (size_t)length, f) != (size_t)length) goto done;
    if (!cp_patch_apply(source, size, patch, (size_t)length, &target, &target_size, error, cap)) {
        free(patch); fclose(f); return 0;
    }
    sha256_compute(target, target_size, hash);
    if (memcmp(hash, p->target_hash, 32)) {
        free(target); free(patch); fclose(f);
        return fail(error, cap, "Patch output does not match this pack/revision; nothing activated");
    }
    *out = target; *out_size = target_size; ok = 1;
done:
    free(patch); fclose(f);
    return ok ? 1 : fail(error, cap, "Cannot read bounded patch input");
}
