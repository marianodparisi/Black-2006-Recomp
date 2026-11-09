/* The Controls pages: keyboard and controller rebinding, controls.ini, and the native page flow.
 *
 *   controls-test MENUS.INI OUTDIR       MENUS.INI is the built-in definition (extract_definition.py)
 *
 * With BLACK_GAME_DIR set to the game's files, the pages are also rendered to OUTDIR\controls-*.bmp with the game's
 * own fonts. The controller presets here are synthetic; the game reads its own from the XBE. */
#include "pc_menu.h"
#include "pc_input.h"
#include "pc_controls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* Captions are unrelated to controls/menu tests; the game keeps its production caption object. */
int pc_subtitles_enabled(void) { return 0; }
int pc_speech_subtitle(uint16_t *out, unsigned capacity, int paused)
{ (void)paused; if (out && capacity) out[0] = 0; return 0; }

static int g_fail, g_pass;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- a synthetic game: four presets, a live map ---- */

static int g_preset_calls;
static uint32_t g_map[PCC_SLOTS];

static int fake_preset(int preset, int slot)
{
    /* every slot a distinct button-ish control per preset; preset 3 differs at FIRE (9) and AIM (16) */
    static const int base[PCC_SLOTS] = { 28, 4, 5, 6, 7, 10, 11, 8, 9, 23, 19, 18, 14, 16, 3, 2, 15, 25, 25, 24, 24, 26, 26,
                                         27, 27, 12, 20, 22, 0, 1, 0, 19, 17, 1, 21, 0, 13 };
    g_preset_calls++;
    if (preset == 3 && slot == 9) return 22;
    if (preset == 3 && slot == 27) return 23;
    return base[slot] == 28 ? 0 : base[slot];
}

static void fake_apply(void) { pc_controls_pad_map(-1, g_map); }

static const PcControlsGame g_game = { fake_preset, fake_apply };

/* ---- the menu host ---- */

static char g_page[64];
static int f_gamecode(void *c, const char *n, const char *const a[3], int f) { (void)c; (void)n; (void)a; (void)f; return 1; }
static void f_page(void *c, const char *n) { (void)c; snprintf(g_page, sizeof(g_page), "%s", n); }
static int f_value(void *c, const char *n, int *o) { (void)c; (void)n; *o = 0; return 0; }
static int f_text(void *c, const char *s, uint16_t *o, unsigned cap) { (void)c; (void)s; (void)o; (void)cap; return -1; }
static void f_sound(void *c, const char *clip) { (void)c; (void)clip; }
static PcMenuHost g_host = { NULL, f_gamecode, f_page, f_value, f_text, f_sound, NULL, NULL, NULL, NULL };

static void tick(const char *page) { pc_menu_tick(page, 1, &g_host); }
static void press(int action, const char *page) { pc_menu_test_action(action); tick(page); }
static void key(unsigned scancode, int extended, const char *page)
{
    pc_input_key_event(scancode, extended, 1, 0);
    tick(page);
    pc_input_key_event(scancode, extended, 0, 0);
    tick(page);
}

static uint8_t *read_all(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    b = (uint8_t *)malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[n] = 0; if (size) *size = (size_t)n; }
    return b;
}

static char *last_occurrence(char *text, const char *needle)
{
    char *found = NULL, *cursor = text;
    while ((cursor = strstr(cursor, needle)) != NULL) {
        found = cursor;
        cursor += strlen(needle);
    }
    return found;
}

static int seed_explicit_default_override(const char *path)
{
    static const char contents[] = "[controller]\r\nFIRE=RT\r\n";
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f) return 0;
    ok = fwrite(contents, 1, sizeof(contents) - 1, f) == sizeof(contents) - 1;
    if (fclose(f) != 0) ok = 0;
    return ok;
}

/* ---- the actual manual wrapper with synthetic guest memory ----
 * The extracted production block is compiled directly below. The fake generated body models only the native map
 * fill; all preset reads, layout writes and live-map writes go through recomp_manual.c's real wrapper. */
enum { SYNTH_GUEST_BYTES = 0x00400000, SYNTH_MAP_VA = 0x00310000 };
static uint32_t g_synth_guest_memory[SYNTH_GUEST_BYTES / sizeof(uint32_t)];
uint32_t g_ecx;
#define BLACK_VA(va) (va) /* fixtures use the canonical (NTSC) layout */
#define GUEST32(va) (g_synth_guest_memory[(uint32_t)(va) / sizeof(uint32_t)])

static int black_pc_ram_span(uint32_t va, size_t bytes)
{
    return va <= SYNTH_GUEST_BYTES && bytes <= (size_t)SYNTH_GUEST_BYTES - va;
}

#include "black_pc_controls_wrapper.inc"

static uint32_t synth_control(int preset, int slot)
{
    static const uint32_t base[PCC_SLOTS] = {
        28, 4, 5, 6, 7, 10, 11, 8, 9, 23, 19, 18, 14, 16, 3, 2, 15, 25, 25, 24, 24, 26, 26,
        27, 27, 12, 20, 22, 0, 1, 0, 19, 17, 1, 21, 0, 13
    };
    if (preset == 3 && slot == 9) return 22;
    if (preset == 3 && slot == 27) return 23;
    return base[slot];
}

static void seed_synthetic_guest(void)
{
    int preset, slot;
    memset(g_synth_guest_memory, 0, sizeof(g_synth_guest_memory));
    GUEST32(0x002F3BD4u) = SYNTH_MAP_VA;
    for (preset = 0; preset < PCC_PRESETS; preset++)
        for (slot = 0; slot < PCC_SLOTS; slot++)
            GUEST32(0x002A7AF0u + (uint32_t)preset * 0x94u + (uint32_t)slot * 4u) = synth_control(preset, slot);
}

void sub_001F1520_gen(void)
{
    uint32_t slot, map = GUEST32(0x002F3BD4u);
    for (slot = 0; slot < PCC_SLOTS; slot++)
        GUEST32(map + slot * 4u) = GUEST32(0x002A7AF0u + g_ecx * 0x94u + slot * 4u);
}

static void check_synthetic_map(int preset)
{
    int slot;
    for (slot = 0; slot < PCC_SLOTS; slot++)
        CHECK(GUEST32(SYNTH_MAP_VA + (uint32_t)slot * 4u) == synth_control(preset, slot));
}

static void test_manual_wrapper(const char *path)
{
    char *text, *controller;
    int slot;

    seed_synthetic_guest();
    pc_controls_pad_reset();
    black_controls_install();
    pc_controls_pad_reset();
    g_ecx = 0;
    sub_001F1520();
    CHECK(pc_controls_pad_layout() == 0);
    CHECK(pc_controls_pad_control(PC_ACTION_FIRE) == 23);
    CHECK(pc_controls_pad_control(PC_ACTION_RELOAD) == 18);
    check_synthetic_map(0);

    CHECK(pc_controls_pad_bind(PC_ACTION_FIRE, 18));
    CHECK(pc_controls_pad_control(PC_ACTION_FIRE) == 18 && pc_controls_pad_control(PC_ACTION_RELOAD) == 23);
    CHECK(GUEST32(SYNTH_MAP_VA + 9u * 4u) == 18 && GUEST32(SYNTH_MAP_VA + 11u * 4u) == 23);
    for (slot = 0; slot < PCC_SLOTS; slot++) {
        const uint32_t want = slot == 9 ? 18 : slot == 11 ? 23 : synth_control(0, slot);
        CHECK(GUEST32(SYNTH_MAP_VA + (uint32_t)slot * 4u) == want);
    }
    text = (char *)read_all(path, NULL);
    controller = text ? last_occurrence(text, "[controller]") : NULL;
    CHECK(controller && strstr(controller, "FIRE=X") && strstr(controller, "RELOAD=RT"));
    free(text);

    CHECK(pc_controls_pad_bind(PC_ACTION_FIRE, -1));
    CHECK(pc_controls_pad_control(PC_ACTION_FIRE) == 23 && pc_controls_pad_control(PC_ACTION_RELOAD) == 18);
    CHECK(GUEST32(SYNTH_MAP_VA + 9u * 4u) == 23 && GUEST32(SYNTH_MAP_VA + 11u * 4u) == 18);
    check_synthetic_map(0);
    text = (char *)read_all(path, NULL);
    controller = text ? last_occurrence(text, "[controller]") : NULL;
    CHECK(controller && !strstr(controller, "FIRE=") && !strstr(controller, "RELOAD="));
    free(text);

    CHECK(pc_controls_pad_layout_step(-1) && pc_controls_pad_layout() == 3);
    CHECK(pc_controls_pad_control(PC_ACTION_FIRE) == 22 && pc_controls_pad_control(PC_ACTION_GRENADE) == 23);
    check_synthetic_map(3); /* includes sentinel slot 0 == PCC_CONTROLS, copied with all 37 slots */

    pc_controls_set_game(&g_game);
    pc_controls_pad_reset();
}

static void test_restore_loaded_default_override(const char *path)
{
    char *text, *controller;

    CHECK(pc_controls_pad_control(PC_ACTION_FIRE) == 23);
    CHECK(GUEST32(SYNTH_MAP_VA + 9u * 4u) == 23);
    /* Profile 3's default is 22. Staying on 23 proves FIRE=RT was loaded, not merely seeded on disk. */
    g_ecx = 3;
    sub_001F1520();
    CHECK(pc_controls_pad_control(PC_ACTION_FIRE) == 23);
    CHECK(GUEST32(SYNTH_MAP_VA + 9u * 4u) == 23);
    g_ecx = 0;
    sub_001F1520();
    CHECK(pc_controls_pad_control(PC_ACTION_FIRE) == 23 && GUEST32(SYNTH_MAP_VA + 9u * 4u) == 23);

    CHECK(pc_controls_pad_bind(PC_ACTION_FIRE, -1));
    text = (char *)read_all(path, NULL);
    controller = text ? last_occurrence(text, "[controller]") : NULL;
    CHECK(controller && !strstr(controller, "FIRE="));
    free(text);

    /* With the override canonicalized away, a native profile change takes effect in the live map. */
    g_ecx = 3;
    sub_001F1520();
    CHECK(pc_controls_pad_control(PC_ACTION_FIRE) == 22);
    CHECK(GUEST32(SYNTH_MAP_VA + 9u * 4u) == 22);
}

/* ---- keyboard ---- */

static void test_keyboard(void)
{
    char names[64];
    pc_input_bindings_reset();
    CHECK(pc_input_binding(PC_ACTION_RELOAD, 0) == 0x13);                    /* R */
    CHECK(pc_controls_key_bind(PC_ACTION_FIRE, 1, 0x13));                   /* R as Fire's secondary */
    CHECK(pc_input_binding(PC_ACTION_FIRE, 1) == 0x13);
    CHECK(pc_input_binding(PC_ACTION_RELOAD, 0) == 0);                      /* R left Reload: one key does one thing */
    CHECK(pc_controls_key_bind(PC_ACTION_RELOAD, 0, PC_SOURCE_MOUSE(4)));   /* Mouse 5 */
    pc_input_binding_names(PC_ACTION_RELOAD, names, sizeof(names));
    CHECK(!strcmp(names, "Mouse5"));
    CHECK(!pc_controls_key_bind(PC_ACTION_USE, 0, 0x01));                    /* Escape belongs to the host */
    CHECK(pc_controls_key_bind(PC_ACTION_CROUCH, 0, 0));                     /* clear LCtrl: C moves up */
    pc_input_binding_names(PC_ACTION_CROUCH, names, sizeof(names));
    CHECK(!strcmp(names, "C"));
    CHECK(pc_controls_key_bind(PC_ACTION_LOOK_UP, 1, 0x48));                 /* a second column of an empty list */
    CHECK(pc_input_binding(PC_ACTION_LOOK_UP, 0) == 0x48);
    {   /* the prompt shows the new primary */
        char prompt[32];
        CHECK(pc_input_prompt_text(pc_input_actions[PC_ACTION_RELOAD].slot, prompt, sizeof(prompt)) && !strcmp(prompt, "[MOUSE 5]"));
    }
}

/* ---- controller ---- */

static void test_controller(void)
{
    pc_controls_pad_reset();
    CHECK(pc_controls_pad_layout() == 0);
    CHECK(pc_controls_pad_control(PC_ACTION_FIRE) == 23);                    /* RT */
    CHECK(pc_controls_pad_control(PC_ACTION_RELOAD) == 18);                  /* X */
    CHECK(pc_controls_pad_bind(PC_ACTION_FIRE, 18));                         /* Fire on X: Reload swaps to RT */
    CHECK(pc_controls_pad_control(PC_ACTION_FIRE) == 18 && pc_controls_pad_control(PC_ACTION_RELOAD) == 23);
    CHECK(g_map[9] == 18 && g_map[11] == 23);                                /* applied to the live map */
    CHECK(g_map[1] == 4);                                                    /* movement untouched */
    CHECK(!pc_controls_pad_bind(PC_ACTION_FIRE, 12));                        /* START stays the pause button */
    CHECK(!pc_controls_pad_bind(PC_ACTION_MOVE_FORWARD, 16));                /* movement is the layout's */
    CHECK(pc_controls_pad_bind(PC_ACTION_FIRE, -1));                         /* back to the layout's button */
    CHECK(pc_controls_pad_control(PC_ACTION_FIRE) == 23);
    CHECK(pc_controls_pad_layout_step(-1) && pc_controls_pad_layout() == 3); /* 1 -> 4 wraps */
    CHECK(pc_controls_pad_control(PC_ACTION_FIRE) == 22 && g_map[9] == 22);
    CHECK(pc_controls_pad_control(PC_ACTION_RELOAD) == 18 && pc_controls_pad_control(PC_ACTION_GRENADE) == 23);
    {
        const char *mark;
        CHECK(pc_controls_pad_glyph(16, &mark) == 0x00B2 && mark == NULL);  /* A */
        CHECK(pc_controls_pad_glyph(23, &mark) == 0x2514 && !strcmp(mark, "RT"));
    }
    {   /* a report: A, LT and d-pad down held */
        uint8_t r[20] = { 0, 20, 0x02, 0, 200, 0, 0, 0, 0, 0, 255, 0 };
        CHECK(pc_controls_pad_held(r) == ((1u << 16) | (1u << 22) | (1u << 1)));
    }
}

/* ---- the file ---- */

static void test_file(const char *path)
{
    char *text;
    pc_controls_pad_reset();
    pc_input_bindings_reset();
    CHECK(pc_controls_key_bind(PC_ACTION_GRENADE, 0, 0x22 + 0x100 * 0));     /* G stays G */
    CHECK(pc_controls_key_bind(PC_ACTION_GRENADE, 1, PC_SOURCE_WHEEL_UP));
    CHECK(pc_controls_pad_bind(PC_ACTION_GRENADE, 21));                      /* White */
    text = (char *)read_all(path, NULL);
    CHECK(text && strstr(text, "[keyboard]") && strstr(text, "GRENADE=G,WheelUp") && strstr(text, "NEXT_WEAPON=Q") &&
          !strstr(text, "NEXT_WEAPON=Q,"));
    CHECK(text && strstr(text, "[controller]") && strstr(text, "GRENADE=White") && !strstr(text, "\nlayout="));
    free(text);
}

/* ---- the pages ---- */

static void test_pages(const char *definition_path)
{
    char *def = (char *)read_all(definition_path, NULL), log[4096];
    int pages;
    CHECK(def != NULL);
    if (!def) return;
    pages = pc_menu_check_definition(def, log, sizeof(log));
    if (log[0]) printf("%s", log);
    CHECK(pages >= 20 && !log[0]);
    pc_menu_test_reset(def);
    pc_menu_install();
    pc_input_bindings_reset();
    tick("");
    tick("OptionsMenuXbox");
    CHECK(pc_menu_test_focus() == 0);                                        /* CONTROLLER SETUP's place: CONTROLS */
    press(PCM_ACT_CONFIRM, "OptionsMenuXbox");                               /* -> Controls */
    CHECK(pc_menu_test_focus() == 0);
    press(PCM_ACT_CONFIRM, "OptionsMenuXbox");                               /* -> ControlsKeyboard */
    press(PCM_ACT_CONFIRM, "OptionsMenuXbox");                               /* -> ControlsKeysMove, MOVE FORWARD */
    press(PCM_ACT_RIGHT, "OptionsMenuXbox");                                 /* the secondary column */
    press(PCM_ACT_CONFIRM, "OptionsMenuXbox");                               /* waiting for a key */
    key(0x48, 1, "OptionsMenuXbox");                                         /* the Up arrow */
    CHECK(pc_input_binding(PC_ACTION_MOVE_FORWARD, 0) == 0x11 && pc_input_binding(PC_ACTION_MOVE_FORWARD, 1) == 0x148);
    press(PCM_ACT_CONFIRM, "OptionsMenuXbox");                               /* again, then Escape cancels */
    key(0x01, 0, "OptionsMenuXbox");
    CHECK(pc_input_binding(PC_ACTION_MOVE_FORWARD, 1) == 0x148);
    key(0x53, 1, "OptionsMenuXbox");                                         /* Delete clears the secondary */
    CHECK(pc_input_binding(PC_ACTION_MOVE_FORWARD, 1) == 0);
    free(def);
}

/* Input used to bind the focused Controls row even when the pointer hovered a different row during
 * capture. Use the real window-input filter and synthetic menu geometry so each physical source
 * exercises the same ACT_KEY / ACT_PAD path as the native pages. */
static const char k_capture_pointer_def[] =
"[style]\nintro = 0\nanimation = 0\n"
"[page ControlsCapture]\nlist = 100 150 30\nfocus = 1\nback = close\n"
"keybind = \"FIRE\" | FIRE\n"
"padbind = \"FIRE\" | FIRE\n"
"keybind = \"RELOAD\" | RELOAD\n"
"padbind = \"RELOAD\" | RELOAD\n";

static void capture_pointer_page(void)
{
    pc_menu_test_reset(k_capture_pointer_def);
    tick("");
    tick("ControlsCapture");
    pc_menu_test_pointer_space(640, 480);
    press(PCM_ACT_LEFT, "ControlsCapture"); /* Keep keyboard captures in the primary column. */
    CHECK(pc_menu_test_focus() == 0);
}

static void test_capture_input_routing(int device)
{
    uint8_t report[20] = { 0, 20 };

    pc_input_bindings_reset();
    pc_controls_set_game(&g_game);
    pc_controls_pad_reset();
    capture_pointer_page();
    if (device == 2) {
        press(PCM_ACT_DOWN, "ControlsCapture"); /* Fire's controller row. */
        press(PCM_ACT_CONFIRM, "ControlsCapture");
        pc_menu_pointer(120, 245); /* Reload's controller row, while Fire is waiting for a button. */
        CHECK(pc_menu_test_pointer_item() == 3);
        report[6] = 200; /* synthetic X press, control 18 */
        pc_input_apply_report(report, 1);
        tick("ControlsCapture");
        memset(report + 2, 0, 18);
        pc_input_apply_report(report, 1);
        tick("ControlsCapture");
        CHECK(pc_menu_test_focus() == 1);
        CHECK(pc_controls_pad_control(PC_ACTION_FIRE) == 18 && pc_controls_pad_control(PC_ACTION_RELOAD) == 23);
    } else {
        press(PCM_ACT_CONFIRM, "ControlsCapture");
        pc_menu_pointer(120, 215); /* Reload's keyboard row, while Fire is waiting for a source. */
        CHECK(pc_menu_test_pointer_item() == 2);
        if (device == 0) {
            pc_input_key_event(0x30, 0, 1, 0); /* B */
            tick("ControlsCapture");
            pc_input_key_event(0x30, 0, 0, 0);
            tick("ControlsCapture");
            CHECK(pc_menu_test_focus() == 0);
            CHECK(pc_input_binding(PC_ACTION_FIRE, 0) == 0x30 && pc_input_binding(PC_ACTION_RELOAD, 0) == 0x13);
        } else {
            pc_input_mouse_button_event(2, 1); /* Mouse 3, distinct from the default Fire binding. */
            tick("ControlsCapture");
            pc_input_mouse_button_event(2, 0);
            tick("ControlsCapture");
            CHECK(pc_menu_test_focus() == 0);
            CHECK(pc_input_binding(PC_ACTION_FIRE, 0) == PC_SOURCE_MOUSE(2) && pc_input_binding(PC_ACTION_RELOAD, 0) == 0x13);
        }
    }

    pc_input_bindings_reset();
    pc_controls_pad_reset();
}

/* ---- pictures with the game's fonts ---- */

static int write_bmp(const char *path, const uint32_t *px, unsigned w, unsigned h)
{
    FILE *f = fopen(path, "wb");
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    unsigned y;
    if (!f) return 0;
    memset(&fh, 0, sizeof(fh)); memset(&ih, 0, sizeof(ih));
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + w * h * 4;
    ih.biSize = sizeof(ih); ih.biWidth = (LONG)w; ih.biHeight = -(LONG)h; ih.biPlanes = 1; ih.biBitCount = 32;
    fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
    for (y = 0; y < h; y++) fwrite(px + (size_t)y * w, 4, w, f);
    fclose(f);
    return 1;
}

static int load_fonts(void)
{
    const char *dir = getenv("BLACK_GAME_DIR");
    static const uint32_t ids[2] = { 4u, 3u };
    static const int slots[2] = { PCM_IMAGE_LOGO, PCM_IMAGE_BULLET };
    char path[512];
    size_t size;
    uint8_t *d;
    int ok = 1;
    if (!dir || !*dir) return 0;
    snprintf(path, sizeof(path), "%s/language/fonts/Small.bin", dir);
    d = read_all(path, &size); ok &= d && pc_menu_load_font_file(PCM_FONT_SMALL, d, size); free(d);
    snprintf(path, sizeof(path), "%s/language/fonts/Big.bin", dir);
    d = read_all(path, &size); ok &= d && pc_menu_load_font_file(PCM_FONT_BIG, d, size); free(d);
    snprintf(path, sizeof(path), "%s/export/FrontEnd/Core.bin", dir);
    d = read_all(path, &size); ok &= d && pc_menu_load_bundle_images(d, size, "core", ids, slots, 2) == 2; free(d);
    return ok;
}

static void render(const char *definition_path, const char *outdir, const char *page, int focus, int capture, const char *name)
{
    char *def = (char *)read_all(definition_path, NULL), path[600];
    uint32_t *px = (uint32_t *)calloc(640 * 480, 4);
    int i;
    if (!def || !px) { free(def); free(px); return; }
    pc_menu_test_reset(def);
    tick("");
    tick(page);
    for (i = 0; i < focus; i++) press(PCM_ACT_DOWN, page);
    if (capture) press(PCM_ACT_CONFIRM, page);
    Sleep(400);
    tick(page);
    for (i = 0; i < 640 * 480; i++) px[i] = 0xFF1A1D1Au;          /* a dark game picture behind the page */
    pc_menu_test_render(px, 640, 480);
    snprintf(path, sizeof(path), "%s\\controls-%s.bmp", outdir, name);
    write_bmp(path, px, 640, 480);
    printf("rendered %s\n", path);
    if (capture) key(0x01, 0, page);
    free(px); free(def);
}

int main(int argc, char **argv)
{
    char ini[MAX_PATH], env[MAX_PATH + 32];
    const char *outdir = argc > 2 ? argv[2] : ".";
    const int restore_default_case = argc > 3 && !strcmp(argv[3], "--restore-default");
    const int capture_keyboard_case = argc > 3 && !strcmp(argv[3], "--capture-keyboard");
    const int capture_mouse_case = argc > 3 && !strcmp(argv[3], "--capture-mouse");
    const int capture_pad_case = argc > 3 && !strcmp(argv[3], "--capture-pad");
    const int capture_routing_case = capture_keyboard_case || capture_mouse_case || capture_pad_case;
    const char *ini_name = restore_default_case ? "controls-restore-default" :
                           capture_keyboard_case ? "controls-capture-keyboard" :
                           capture_mouse_case ? "controls-capture-mouse" :
                           capture_pad_case ? "controls-capture-pad" : "controls-test";
    if (argc < 2) { printf("usage: controls-test MENUS.INI [OUTDIR]\n"); return 2; }
    snprintf(ini, sizeof(ini), "%s\\%s.ini", outdir, ini_name);
    DeleteFileA(ini);
    snprintf(env, sizeof(env), "RECOMP_CONTROLS_INI=%s", ini);
    _putenv(env);
    snprintf(env, sizeof(env), "RECOMP_MENUS_INI=%s", argv[1]);   /* never a local\menus.ini of a real install */
    _putenv(env);
    _putenv("RECOMP_LANGUAGE=en-US");
    _putenv("RECOMP_BLACK_INPUT_MODE=auto");
    _putenv("RECOMP_BLACK_PROMPT_MODE=keyboard_mouse");

    if (restore_default_case) {
        seed_synthetic_guest();
        if (!seed_explicit_default_override(ini)) {
            CHECK(0);
        } else {
            pc_input_init();
            black_controls_install(); /* loads FIRE=RT before the first guest profile fill */
            g_ecx = 0;
            sub_001F1520();
            test_restore_loaded_default_override(ini);
        }
        DeleteFileA(ini);
        printf("%d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }

    if (capture_routing_case) {
        pc_input_init();
        pc_menu_install();
        test_capture_input_routing(capture_keyboard_case ? 0 : capture_mouse_case ? 1 : 2);
        DeleteFileA(ini);
        printf("%d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }

    pc_input_init();
    pc_controls_set_game(&g_game);
    pc_controls_load();
    test_keyboard();
    test_controller();
    test_manual_wrapper(ini);
    test_file(ini);
    test_pages(argv[1]);
    if (load_fonts()) {
        pc_controls_pad_reset();
        pc_input_bindings_reset();
        render(argv[1], outdir, "Controls", 0, 0, "root");
        render(argv[1], outdir, "ControlsKeysMove", 2, 0, "keys");
        render(argv[1], outdir, "ControlsKeysCombat", 1, 1, "keys-capture");
        render(argv[1], outdir, "ControlsController", 0, 0, "pad");
        render(argv[1], outdir, "ControlsPadCombat", 0, 0, "pad-combat");
        render(argv[1], outdir, "ControlsPadItems", 2, 1, "pad-capture");
        render(argv[1], outdir, "OptionsMenuXbox", 0, 0, "options");
    } else {
        printf("(BLACK_GAME_DIR not set: pages not rendered)\n");
    }
    DeleteFileA(ini);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
