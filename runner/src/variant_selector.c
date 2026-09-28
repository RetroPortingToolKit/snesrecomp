#include "variant_selector.h"

#include <stdio.h>
#include <string.h>

#include "snes_overlay_draw.h"

#define SEL_NAV (SNES_SEL_BTN_UP | SNES_SEL_BTN_DOWN | SNES_SEL_BTN_LEFT | SNES_SEL_BTN_RIGHT)
#define SEL_CONFIRM (SNES_SEL_BTN_A | SNES_SEL_BTN_START)
#define SEL_CANCEL (SNES_SEL_BTN_B | SNES_SEL_BTN_X | SNES_SEL_BTN_Y)

/* Highlight = index into the active variant's group (ordered by
 * selector_order, then registration order). */
static int s_group_count;
static const SnesContentVariant *s_group[SNES_VARIANT_MAX];
static int s_highlight;          /* index into s_group */
static int s_profile = 1;        /* highlighted profile 1..N */
static int s_visible;
static uint32_t s_prev;          /* previous seat-0 word for edge detection */
static uint32_t s_guard;         /* buttons consumed; masked until released */
static int s_switch_pending;
static const SnesContentVariant *s_switch_variant;
static int s_switch_profile;
static int s_frame;

static int group_index_of(const SnesContentVariant *v) {
    for (int i = 0; i < s_group_count; i++)
        if (s_group[i] == v) return i;
    return -1;
}

/* Rebuild the ordered group around the active variant. */
static void build_group(void) {
    const SnesContentVariant *active = snes_variant_active();
    s_group_count = 0;
    if (!active) return;
    unsigned n = snes_variant_count();
    for (unsigned i = 0; i < n && s_group_count < SNES_VARIANT_MAX; i++) {
        const SnesContentVariant *v = snes_variant_at(i);
        if (strcmp(v->decl.selector_group, active->decl.selector_group) != 0) continue;
        /* insertion sort by selector_order, stable on registration order */
        int at = s_group_count;
        while (at > 0 && s_group[at - 1]->decl.selector_order > v->decl.selector_order) {
            s_group[at] = s_group[at - 1];
            at--;
        }
        s_group[at] = v;
        s_group_count++;
    }
}

void snes_variant_selector_sync(void) {
    build_group();
    int idx = group_index_of(snes_variant_active());
    s_highlight = idx < 0 ? 0 : idx;
    s_profile = snes_variant_active_profile();
    s_switch_pending = 0;
    s_switch_variant = NULL;
    s_guard = 0;
}

static int predicate_holds(const uint8_t *wram) {
    const SnesContentVariant *active = snes_variant_active();
    if (!active || !active->decl.has_selector_screen || !wram) return 0;
    if (active->decl.selector_wram_addr >= 0x20000u) return 0;
    return wram[active->decl.selector_wram_addr] == active->decl.selector_wram_value;
}

int snes_variant_selector_visible(void) { return s_visible; }
int snes_variant_selector_highlight_index(void) { return s_highlight; }
int snes_variant_selector_highlight_profile(void) { return s_profile; }

uint32_t snes_variant_selector_filter_input(uint32_t inputs, const uint8_t *wram) {
    s_frame++;
    build_group();
    const uint32_t seat0 = inputs & 0xfffu;
    const uint32_t pressed = seat0 & ~s_prev;
    s_prev = seat0;
    /* A consumed button stays masked until released, so the press that
     * confirmed a switch or moved the highlight never reaches the guest. */
    s_guard &= seat0;

    s_visible = s_group_count > 1 && predicate_holds(wram);
    if (!s_visible) {
        /* Off-screen: forget a stray highlight so the game's own cursor is
         * never blocked when the screen comes back. */
        int idx = group_index_of(snes_variant_active());
        if (idx >= 0) s_highlight = idx;
        return inputs & ~s_guard;
    }

    if (s_highlight >= s_group_count) s_highlight = 0;
    const int home = group_index_of(snes_variant_active());
    const int away = s_highlight != home;

    if (pressed & SNES_SEL_BTN_LEFT) {
        s_highlight = (s_highlight + s_group_count - 1) % s_group_count;
        s_profile = s_highlight == home ? snes_variant_active_profile() : 1;
        s_guard |= SNES_SEL_BTN_LEFT;
    } else if (pressed & SNES_SEL_BTN_RIGHT) {
        s_highlight = (s_highlight + 1) % s_group_count;
        s_profile = s_highlight == home ? snes_variant_active_profile() : 1;
        s_guard |= SNES_SEL_BTN_RIGHT;
    } else if (away && (pressed & SNES_SEL_BTN_UP)) {
        s_profile = s_profile <= 1 ? SNES_VARIANT_PROFILES : s_profile - 1;
        s_guard |= SNES_SEL_BTN_UP;
    } else if (away && (pressed & SNES_SEL_BTN_DOWN)) {
        s_profile = s_profile >= SNES_VARIANT_PROFILES ? 1 : s_profile + 1;
        s_guard |= SNES_SEL_BTN_DOWN;
    } else if (away && (pressed & SEL_CONFIRM)) {
        const SnesContentVariant *target = s_group[s_highlight];
        if (target->available) {
            s_switch_pending = 1;
            s_switch_variant = target;
            s_switch_profile = s_profile;
        }
        s_guard |= pressed & SEL_CONFIRM;
    } else if (away && (pressed & SEL_CANCEL)) {
        if (home >= 0) s_highlight = home;
        s_profile = snes_variant_active_profile();
        s_guard |= pressed & SEL_CANCEL;
    }

    uint32_t mask = s_guard;
    if (s_highlight != home) {
        /* The guest's own cursor must not move while the player is browsing
         * another variant's profiles; face buttons are consumed above. */
        mask |= SEL_NAV | SEL_CONFIRM | SEL_CANCEL;
    }
    return inputs & ~mask;
}

int snes_variant_selector_take_switch(const SnesContentVariant **variant, int *profile) {
    if (!s_switch_pending) return 0;
    s_switch_pending = 0;
    if (variant) *variant = s_switch_variant;
    if (profile) *profile = s_switch_profile;
    s_switch_variant = NULL;
    return 1;
}

/* ── Overlay ──────────────────────────────────────────────────────────────── */

#define COL_PANEL   0xC0101018u
#define COL_TEXT    0xFFF0F0F0u
#define COL_DIM     0xFF8A8A96u
#define COL_ARROW   0xFFFFD24Du
#define COL_WARN    0xFFFF8A6Bu
#define COL_ACTIVE  0xFF6BE06Bu

static int text_w(const char *s, int scale) { return (int)strlen(s) * 8 * scale; }

void snes_variant_selector_draw(uint8_t *pixels, int pitch_bytes, int width, int height) {
    if (!s_visible || !pixels || width <= 0 || height <= 0) return;
    if (s_highlight >= s_group_count) return;
    const SnesContentVariant *v = s_group[s_highlight];
    const int home = group_index_of(snes_variant_active());
    const int scale = width >= 512 ? 2 : 1;
    const int stride = pitch_bytes / 4;
    uint32_t *fb = (uint32_t *)pixels;

    char name[96];
    snprintf(name, sizeof(name), "%s", v->decl.display_name);
    /* Upper-case: the overlay font has capitals and digits only. */
    for (char *p = name; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    char line2[192];
    if (s_highlight == home)
        snprintf(line2, sizeof(line2), "PLAYING  PROFILE %d", snes_variant_active_profile());
    else if (!v->available)
        snprintf(line2, sizeof(line2), "%s", v->reason);
    else
        snprintf(line2, sizeof(line2), "PROFILE %d/%d  A: PLAY  B: BACK",
                 s_profile, SNES_VARIANT_PROFILES);
    for (char *p = line2; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);

    const int line_h = 8 * scale + 2 * scale;
    const int panel_h = line_h * 2 + 6 * scale;
    const int panel_w = width;
    const int y0 = 4 * scale;
    snes_ovl_fill_rect(fb, stride, height, 0, y0, panel_w, panel_h, COL_PANEL);

    /* "<  NAME  >" centred, arrows blink when there is somewhere to go. */
    const int blink = (s_frame / 16) & 1;
    const int name_x = (width - text_w(name, scale)) / 2;
    const int ty = y0 + 3 * scale;
    snes_ovl_draw_text(fb, stride, height, name_x, ty, name,
                       s_highlight == home ? COL_ACTIVE : COL_TEXT, scale);
    if (s_group_count > 1 && (blink || s_highlight != home)) {
        snes_ovl_draw_text(fb, stride, height, name_x - 16 * scale, ty, "<", COL_ARROW, scale);
        snes_ovl_draw_text(fb, stride, height, name_x + text_w(name, scale) + 8 * scale, ty,
                           ">", COL_ARROW, scale);
    }
    const int l2x = (width - text_w(line2, scale)) / 2;
    snes_ovl_draw_text(fb, stride, height, l2x < 0 ? 0 : l2x, ty + line_h, line2,
                       s_highlight == home ? COL_DIM : (v->available ? COL_TEXT : COL_WARN),
                       scale);
    /* Position dots: one per variant in the group. */
    const int dots_w = s_group_count * 6 * scale;
    int dx = (width - dots_w) / 2;
    for (int i = 0; i < s_group_count; i++, dx += 6 * scale)
        snes_ovl_fill_rect(fb, stride, height, dx, y0 + panel_h - 3 * scale,
                           4 * scale, 2 * scale, i == s_highlight ? COL_ARROW : COL_DIM);
}
