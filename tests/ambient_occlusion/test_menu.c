/* Tests for the native menus (pc_menu.c) against a fake game.
 *
 *   pc-menu-test                         run the checks (render checks too when the game's files are there)
 *   pc-menu-test check FILE.ini          parse a definition and print what is wrong with it
 *   pc-menu-test render FILE.ini PAGE OUT.bmp [W H [BACKGROUND.bmp [FOCUS]]]
 *                                        draw one page (over a background frame) with the game's own fonts
 *
 * The fake game answers like BLACK's front end: $KEY strings from the language file when it is present (keys
 * hash with the game's CRC), a few &KEY texts, the option values, and it records every request the page makes.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include "pc_menu.h"
#include "pc_movie_skip.h"
#include "pc_input.h"
#include "pc_video.h"
#include "pc_video_menu.h"

/* The menu regression never runs a movie, so keep the subtitle service inactive. */
int pc_movie_skip_subtitle(uint16_t *out, unsigned capacity)
{ if (out && capacity) out[0] = 0; return 0; }

static int g_fail, g_pass, g_have_files;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the fake game ------------------------------------------------------------------------------------------- */

typedef struct Kv { char name[48]; int value; } Kv;
static Kv g_values[32];
static int g_nvalues;
static char g_texts[16][2][160];
static int g_ntexts;
static char g_log[8192];
static int g_busy;
static uint32_t g_crc[256];
static uint8_t *g_strings;
static size_t g_strings_size;

static void log_add(const char *s) { strncat(g_log, s, sizeof(g_log) - strlen(g_log) - 2); strcat(g_log, "\n"); }
static void log_clear(void) { g_log[0] = 0; }

static void value_set(const char *name, int v)
{
    int i;
    for (i = 0; i < g_nvalues; i++) if (!strcmp(g_values[i].name, name)) { g_values[i].value = v; return; }
    snprintf(g_values[g_nvalues].name, 48, "%s", name);
    g_values[g_nvalues++].value = v;
}

static void text_set(const char *name, const char *v)
{
    int i;
    for (i = 0; i < g_ntexts; i++) if (!strcmp(g_texts[i][0], name)) { snprintf(g_texts[i][1], 160, "%s", v); return; }
    snprintf(g_texts[g_ntexts][0], 160, "%s", name);
    snprintf(g_texts[g_ntexts++][1], 160, "%s", v);
}

static uint32_t key_hash(const char *s)
{
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; s++) c = ((c >> 8) | ((c & 0x80000000u) ? 0xFF000000u : 0u)) ^ g_crc[(c ^ (uint32_t)(int32_t)(signed char)*s) & 0xFFu];
    return c;
}

static const char *lang_string(uint32_t hash)
{
    uint32_t n, table, i;
    if (!g_strings) return NULL;
    memcpy(&n, g_strings + 8, 4); memcpy(&table, g_strings + 12, 4);
    for (i = 0; i < n; i++) {
        uint32_t off, h;
        memcpy(&off, g_strings + table + i * 4, 4);
        if (off + 8 > g_strings_size) continue;
        memcpy(&h, g_strings + off, 4);
        if (h == hash) return (const char *)g_strings + off + 4;
    }
    return NULL;
}

static int f_gamecode(void *ctx, const char *name, const char *const args[3], int force)
{
    char line[200];
    (void)ctx;
    if (g_busy && !force) return 0;
    snprintf(line, sizeof(line), "gamecode %s %s %s %s", name, args[0] ? args[0] : "0", args[1] ? args[1] : "0", args[2] ? args[2] : "0");
    log_add(line);
    /* the game's option functions store what they are given */
    if (!strcmp(name, "ToggleInvertLook")) value_set("FEInvertLookFlag", atoi(args[0]));
    if (!strcmp(name, "ToggleCrouchOnOff")) value_set("FEToggleCrouchFlag", atoi(args[0]));
    if (!strcmp(name, "ToggleVibration")) value_set("FEVibration", atoi(args[0]));
    if (!strcmp(name, "SetSfxValue")) value_set("FESFXVolume", atoi(args[0]));
    if (!strcmp(name, "SetMusicValue")) value_set("FEMusicVolume", atoi(args[0]));
    return 1;
}

static void f_page(void *ctx, const char *name) { char line[100]; (void)ctx; snprintf(line, sizeof(line), "page %s", name); log_add(line); }
static void f_sound(void *ctx, const char *clip) { char line[100]; (void)ctx; snprintf(line, sizeof(line), "sound %s", clip); log_add(line); }
static void f_video(void *ctx) { (void)ctx; log_add("video"); }

static int f_value(void *ctx, const char *name, int *out)
{
    int i;
    (void)ctx;
    for (i = 0; i < g_nvalues; i++) if (!strcmp(g_values[i].name, name)) { *out = g_values[i].value; return 1; }
    return 0;
}

static int utf8_out(const char *s, uint16_t *out, unsigned cap)
{
    unsigned n = 0;
    const unsigned char *u = (const unsigned char *)s;
    while (*u && n + 1 < cap) {
        unsigned c = *u++;
        if (c >= 0xC0 && c < 0xE0 && *u) c = ((c & 0x1F) << 6) | (*u++ & 0x3F);
        else if (c >= 0xE0 && c < 0xF0 && u[0] && u[1]) { c = ((c & 0x0F) << 12) | ((u[0] & 0x3F) << 6) | (u[1] & 0x3F); u += 2; }
        out[n++] = (uint16_t)c;
    }
    out[n] = 0;
    return (int)n;
}

static int f_text(void *ctx, const char *spec, uint16_t *out, unsigned cap)
{
    int i;
    (void)ctx;
    if (spec[0] == '&') {
        for (i = 0; i < g_ntexts; i++) if (!strcmp(g_texts[i][0], spec + 1)) return utf8_out(g_texts[i][1], out, cap);
        return -1;
    }
    if (spec[0] == '$') {
        const char *s;
        if (!strcmp(spec, "$FE_CONFIRM") && pc_input_prompt_device() == PC_DEVICE_KEYBOARD_MOUSE) return utf8_out("[ENTER] confirm", out, cap);
        if (!strcmp(spec, "$FE_BACK") && pc_input_prompt_device() == PC_DEVICE_KEYBOARD_MOUSE) return utf8_out("[ESC] back", out, cap);
        s = lang_string(key_hash(spec + 1));
        if (s) return utf8_out(s, out, cap);
        /* without the language file: the key itself, lower case like the game's strings */
        {
            char low[64];
            size_t k;
            for (k = 0; spec[k + 4] && k + 1 < sizeof(low); k++) low[k] = (char)tolower((unsigned char)spec[k + 4]);
            low[k] = 0;
            return utf8_out(low, out, cap);
        }
    }
    return -1;
}

/* The game's own page variables (ActionScript globals in BLACK). */
static char g_gvars[8][2][48];
static int g_ngvars;

static void f_set_var(void *ctx, const char *name, const char *value)
{
    int i;
    (void)ctx;
    for (i = 0; i < g_ngvars; i++) if (!strcmp(g_gvars[i][0], name)) { snprintf(g_gvars[i][1], 48, "%s", value); return; }
    if (g_ngvars < 8) { snprintf(g_gvars[g_ngvars][0], 48, "%s", name); snprintf(g_gvars[g_ngvars++][1], 48, "%s", value); }
}

static int f_get_var(void *ctx, const char *name, char *out, unsigned cap)
{
    int i;
    (void)ctx;
    for (i = 0; i < g_ngvars; i++) if (!strcmp(g_gvars[i][0], name)) { snprintf(out, cap, "%s", g_gvars[i][1]); return 1; }
    return 0;
}

static void f_quit(void *ctx) { (void)ctx; log_add("quit"); }

static PcMenuHost g_host = { NULL, f_gamecode, f_page, f_value, f_text, f_sound, f_video, f_set_var, f_get_var, f_quit };

static int tick(const char *page) { return pc_menu_tick(page, 1, &g_host); }
static void press(int action, const char *page) { pc_menu_test_action(action); tick(page); }

/* ---- the game's files ---------------------------------------------------------------------------------------- */

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
    if (b) { b[n] = 0; *size = (size_t)n; }
    return b;
}

static int load_game_files(void)
{
    const char *dir = getenv("BLACK_GAME_DIR");
    char path[512];
    size_t size;
    uint8_t *d;
    int ok = 1;
    static const uint32_t ids[2] = { 4u, 3u };
    static const int slots[2] = { PCM_IMAGE_LOGO, PCM_IMAGE_BULLET };
    if (!dir || !*dir) return 0; /* Source-only CI does not require retail assets. */
    snprintf(path, sizeof(path), "%s/language/fonts/Small.bin", dir);
    d = read_all(path, &size); ok &= d && pc_menu_load_font_file(PCM_FONT_SMALL, d, size); free(d);
    snprintf(path, sizeof(path), "%s/language/fonts/Big.bin", dir);
    d = read_all(path, &size); ok &= d && pc_menu_load_font_file(PCM_FONT_BIG, d, size); free(d);
    snprintf(path, sizeof(path), "%s/export/FrontEnd/Core.bin", dir);
    d = read_all(path, &size); ok &= d && pc_menu_load_bundle_images(d, size, "core", ids, slots, 2) == 2; free(d);
    snprintf(path, sizeof(path), "%s/language/strings/MainUS.bin", dir);
    g_strings = read_all(path, &g_strings_size);
    return ok;
}

/* ---- bitmaps ----------------------------------------------------------------------------------------------------- */

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

/* A 24- or 32-bit BMP, scaled (nearest) to w x h, as opaque pixels. */
static uint32_t *read_bmp(const char *path, unsigned w, unsigned h)
{
    size_t size;
    uint8_t *d = read_all(path, &size);
    uint32_t *out;
    int bw, bh, bpp, off, x, y;
    if (!d || size < 54 || d[0] != 'B' || d[1] != 'M') { free(d); return NULL; }
    memcpy(&off, d + 10, 4); memcpy(&bw, d + 18, 4); memcpy(&bh, d + 22, 4); bpp = d[28] | (d[29] << 8);
    out = (uint32_t *)malloc((size_t)w * h * 4);
    if (!out || (bpp != 24 && bpp != 32)) { free(d); free(out); return NULL; }
    for (y = 0; y < (int)h; y++) for (x = 0; x < (int)w; x++) {
        const int sx = x * bw / (int)w, sy0 = y * (bh < 0 ? -bh : bh) / (int)h, sy = bh > 0 ? bh - 1 - sy0 : sy0;
        const int stride = ((bw * (bpp / 8)) + 3) & ~3;
        const uint8_t *p = d + off + (size_t)sy * stride + (size_t)sx * (bpp / 8);
        out[(size_t)y * w + x] = 0xFF000000u | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
    }
    free(d);
    return out;
}

static void composite(uint32_t *dst, const uint32_t *overlay, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        const uint32_t o = overlay[i], b = dst[i];
        const unsigned a = o >> 24, inv = 255 - a;
        const unsigned r = ((o >> 16) & 255) + (((b >> 16) & 255) * inv + 127) / 255;
        const unsigned g = ((o >> 8) & 255) + (((b >> 8) & 255) * inv + 127) / 255;
        const unsigned bl = (o & 255) + ((b & 255) * inv + 127) / 255;
        dst[i] = 0xFF000000u | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (bl > 255 ? 255 : bl);
    }
}

/* ---- the definition used by the behaviour checks (BLACK's pages, as in the game project's default) ------------ */

static const char k_def[] =
"[style]\n"
"focus_scale = 1.1\n"
"[page PauseMenu]\n"
"title = $FE_PAUSEMENU\n"
"label = 284 141 $FE_CURRENTOBJECTIVES\n"
"text = 284 165 &FE_CURRENTOBJECTIVE | wrap 300\n"
"list = 285 241 25\n"
"start = back\n"
"back = gamecode ClosePauseMenu\n"
"loading = FELoadingPauseMenus\n"
"item = $FE_CONTINUE | gamecode ClosePauseMenu\n"
"item = $FE_OBJECTIVESSTATUS | page PauseMenuObjectives\n"
"item = $FE_RESTARTMISSION | set FEConfirmYesGameCode RestartLevel; set FEConfirmNoPage PauseMenu; page ConfirmWarning; gamecode SetupConfirmWarning RestartConfirm\n"
"item = $FE_OPTIONS | page PauseMenuOptionXbox\n"
"item = $FE_QUIT | set FEConfirmYesGameCode QuitLevel; set FEConfirmNoPage PauseMenu; page ConfirmWarning; gamecode SetupConfirmWarning QuitConfirm\n"
"[page PauseMenuOptionXbox]\n"
"title = $FE_OPTIONS\n"
"list = 218 147 27\n"
"columns = 431 519\n"
"arrows = 4\n"
"back = page PauseMenu\n"
"toggle = $FE_TOGGLECROUCH | FEToggleCrouchFlag | ToggleCrouchOnOff\n"
"toggle = $FE_INVERTLOOK | FEInvertLookFlag | ToggleInvertLook\n"
"toggle = $FE_VIBRATION | FEVibration | ToggleVibration\n"
"slider = $FE_SFXVOLUME | FESFXVolume | SetSfxValue | 0 100 4 | gap 15\n"
"slider = $FE_MUSICVOLUME | FEMusicVolume | SetMusicValue | 0 100 4\n"
"item = \"VIDEO SETTINGS\" | video\n"
"[page ConfirmWarning]\n"
"require = FEConfirmYesGameCode FEConfirmNoPage\n"
"title = &FE_CONFIRMTITLE\n"
"text = 284 141 &FE_CONFIRMMESSAGE | wrap 290\n"
"focus = 2\n"
"prompts = select confirm\n"
"back = page @FEConfirmNoPage\n"
"item = $FE_YES | gamecode @FEConfirmYesGameCode\n"
"item = $FE_NO | page @FEConfirmNoPage\n"
"[page MissionFailed]\n"
"title = $FE_MISSIONFAILED\n"
"item = $FE_CONTINUEMISSION | gamecode ContinueMission | disabled unless FE_LevelAllowContinue\n"
"item = $FE_RESTARTMISSION | set FEConfirmYesGameCode RestartMission; set FEConfirmNoPage MissionFailed; page ConfirmWarning; gamecode SetupConfirmWarning RestartConfirm\n"
"item = $FE_QUIT | set FEConfirmYesGameCode QuitFromMissionFailed; set FEConfirmNoPage MissionFailed; page ConfirmWarning; gamecode SetupConfirmWarning QuitConfirm\n";

/* The PC pages use the same layout and action syntax as the shipped native definition. */
static const char k_video_def[] =
"[style]\nintro = 0\nanimation = 0\n"
"[page OptionsMenuXbox]\ntitle = \"OPTIONS\"\nback = gamecode StartAutosaveSettings\n"
"item = \"CONTROLS\" | none\nitem = \"SOUND\" | none\nitem = \"VIDEO SETTINGS\" | video\n"
"[page PauseMenuOptionXbox]\ntitle = \"PAUSE OPTIONS\"\nback = page PauseMenu\n"
"item = \"CONTROLS\" | none\nitem = \"SOUND\" | none\nitem = \"VIDEO SETTINGS\" | video\n"
"[page VideoSettings]\ntitle = \"VIDEO SETTINGS\"\nlist = 218 147 27\nselector = 349\nback = close\n"
"item = \"DISPLAY\" | open VideoDisplay\nitem = \"GRAPHICS\" | open VideoGraphics\n"
"item = \"RESET TO DEFAULTS\" | reset_video\nitem = \"BACK\" | close\n"
"[page VideoDisplay]\ntitle = \"DISPLAY\"\nlist = 168 142 23\nvalues = 607\nitem_size = 13.5\n"
"selector = 451\narrows = 4\ntext = 168 366 &VIDEO_HELP | wrap 439 | size 12\nback = close\n"
"setting = \"WINDOW MODE\" | mode\nsetting = \"WINDOW SIZE\" | window\n"
"setting = \"INTERNAL RESOLUTION\" | internal\nsetting = \"ASPECT RATIO\" | aspect\n"
"setting = \"SCALING\" | scale\nsetting = \"SCALING FILTER\" | filter\n"
"setting = \"VERTICAL SYNC\" | vsync\nsetting = \"FRAME LIMIT\" | fps\nitem = \"BACK\" | close\n"
"[page VideoGraphics]\ntitle = \"GRAPHICS\"\nlist = 168 142 23\nvalues = 607\nitem_size = 13.5\n"
"selector = 451\narrows = 4\ntext = 168 366 &VIDEO_HELP | wrap 439 | size 12\nback = close\n"
"setting = \"ANTI-ALIASING\" | aa\nsetting = \"ANISOTROPIC FILTERING\" | af\n"
"setting = \"DEPTH OF FIELD\" | dof\nsetting = \"SHARPENING\" | sharpen\n"
"setting = \"AMBIENT OCCLUSION\" | ao_method\nsetting = \"AO QUALITY\" | ao_quality\n"
"setting = \"FIELD OF VIEW\" | fov\nsetting = \"MOTION BLUR\" | blur\nitem = \"BACK\" | close\n";

static char g_video_ini[MAX_PATH + 48];

static void video_fresh(void)
{
    static const char *const env[] = { "RECOMP_WINDOW_WIDTH", "RECOMP_WINDOW_HEIGHT", "RECOMP_WINDOW_MODE",
        "RECOMP_RENDER_WIDTH", "RECOMP_RENDER_HEIGHT", "RECOMP_INTERNAL_HEIGHT", "RECOMP_AA", "RECOMP_AF",
        "RECOMP_DOF", "RECOMP_SHARPEN", "RECOMP_SSAO", "RECOMP_AO_METHOD", "RECOMP_AO_QUALITY",
        "RECOMP_BLACK_ASPECT", "RECOMP_BLACK_FOV", "RECOMP_BLACK_MOTION_BLUR",
        "RECOMP_SCALE_MODE", "RECOMP_PRESENT_FILTER", "RECOMP_VSYNC", "RECOMP_FPS_LIMIT", "RECOMP_VIDEO_INI" };
    unsigned i;
    if (!g_video_ini[0]) {
        DWORD n = GetTempPathA(MAX_PATH, g_video_ini);
        CHECK(n > 0 && n < MAX_PATH, "a private temporary video settings path is available");
        snprintf(g_video_ini + n, sizeof(g_video_ini) - n, "pc-menu-video-%lu.ini", (unsigned long)GetCurrentProcessId());
    }
    for (i = 0; i < sizeof(env) / sizeof(env[0]); i++) _putenv_s(env[i], "");
    DeleteFileA(g_video_ini);
    pc_video_test_reset(g_video_ini);
}

static void reset_game(void)
{
    g_nvalues = 0; g_ntexts = 0; g_busy = 0; g_ngvars = 0; log_clear();
    value_set("FEInvertLookFlag", 0); value_set("FEToggleCrouchFlag", 0); value_set("FEVibration", 1);
    value_set("FESFXVolume", 48); value_set("FEMusicVolume", 100); value_set("FELoadingPauseMenus", 0);
    value_set("FE_LevelAllowContinue", 1);
    text_set("FE_CURRENTOBJECTIVE", "rendezvous with black cell");
    text_set("FE_CONFIRMTITLE", "restart");
    text_set("FE_CONFIRMMESSAGE", "All checkpoint progress will be lost!  are you sure you want to restart the mission?");
    pc_menu_test_reset(k_def);
    tick("");
}

static int log_is(const char *want) { return !strcmp(g_log, want); }

/* ---- checks ---------------------------------------------------------------------------------------------------------- */

static void test_parser(void)
{
    char log[4096];
    int n = pc_menu_check_definition(k_def, log, sizeof(log));
    CHECK(n == 4 && !log[0], "the test definition: %d pages, log '%s'", n, log);
    n = pc_menu_check_definition("[page A]\nitem = $X | jump Y\n[page B]\nitem = $Y | page Z\n", log, sizeof(log));
    CHECK(n == 1 && strstr(log, "menus.ini:2:") && strstr(log, "unknown action 'jump'"), "a bad action drops its page: %d '%s'", n, log);
    n = pc_menu_check_definition("[page A]\nlist = 1 2\n", log, sizeof(log));
    CHECK(n == 0 && strstr(log, "list needs"), "list needs three numbers: '%s'", log);
    n = pc_menu_check_definition("[style]\nitem = GG0000\n[page A]\nitem = \"X\" | none\n", log, sizeof(log));
    CHECK(n == 1 && strstr(log, "bad colour"), "a bad colour is reported, the page stays: %d '%s'", n, log);
    n = pc_menu_check_definition("[page A]\nitem = $X | none\n[page a]\nitem = $Y | none\n", log, sizeof(log));
    CHECK(n == 1 && strstr(log, "listed twice"), "page names are unique ignoring case: '%s'", log);
    n = pc_menu_check_definition("[page A]\nslider = $X | V | F | 0 100\n", log, sizeof(log));
    CHECK(n == 0 && strstr(log, "MIN MAX STEP"), "a slider needs its range: '%s'", log);
    n = pc_menu_check_definition("[page A]\nlabel = 1 2 CURRENT OBJECTIVE\n", log, sizeof(log));
    CHECK(n == 0 && strstr(log, "quote a text"), "unquoted words are an error: '%s'", log);
    n = pc_menu_check_definition("; comment\n# also\n[page A]\nlist = 10 20 30 ; trailing comment\nlabel = 1 2 \"two words\" | wrap 100\nitem = $X | gamecode F a b c\n", log, sizeof(log));
    CHECK(n == 1 && !log[0], "comments, quoted labels and three arguments are fine: '%s'", log);
    n = pc_menu_check_definition("[page A]\nitem = $X | gamecode F a b c d\n", log, sizeof(log));
    CHECK(n == 0 && strstr(log, "too many arguments"), "a gamecode takes three arguments: '%s'", log);
    n = pc_menu_check_definition("[nonsense]\nfoo = 1\n", log, sizeof(log));
    CHECK(n == 0 && strstr(log, "unknown section"), "unknown sections are reported: '%s'", log);
    n = pc_menu_check_definition("[page A]\nitem = $X | open B; close; exit\nback = close\n", log, sizeof(log));
    CHECK(n == 1 && !log[0], "open, close and exit are actions: '%s'", log);
    n = pc_menu_check_definition("[page A]\nitem = $X | open\n", log, sizeof(log));
    CHECK(n == 0 && strstr(log, "'open' needs a name"), "open needs a page: '%s'", log);
    n = pc_menu_check_definition("[page A]\nitem = $X | exit now\n", log, sizeof(log));
    CHECK(n == 0 && strstr(log, "'exit' takes nothing"), "exit takes nothing: '%s'", log);
    n = pc_menu_check_definition(k_video_def, log, sizeof(log));
    CHECK(n == 5 && !log[0], "all native video setting names and reset_video parse: %d '%s'", n, log);
    n = pc_menu_check_definition("[page A]\nsetting = \"WINDOW MODE\" | mode | focus gamecode One; gamecode Two\n", log, sizeof(log));
    CHECK(n == 1 && !log[0], "a setting keeps its complete focus script after a semicolon: '%s'", log);
    n = pc_menu_check_definition("[page A]\nsetting = \"BAD\" | unknown\n", log, sizeof(log));
    CHECK(n == 0 && strstr(log, "video row name"), "unknown video settings are rejected: '%s'", log);
    n = pc_menu_check_definition("[page A]\nsetting = \"BAD\"\n", log, sizeof(log));
    CHECK(n == 0 && strstr(log, "video row name"), "a setting needs its model row: '%s'", log);
    n = pc_menu_check_definition("[page A]\nitem = \"RESET\" | reset_video now\n", log, sizeof(log));
    CHECK(n == 0 && strstr(log, "'reset_video' takes nothing"), "reset_video rejects extra arguments: '%s'", log);
}

static int display_has(const char *text)
{
    char dump[16384];
    pc_menu_test_dump(dump, sizeof(dump));
    return strstr(dump, text) != NULL;
}

static void video_menu_fixture(void)
{
    reset_game();
    pc_menu_test_reset(k_video_def);
    tick("");
}

static int same_video_settings(const PcVideoSettings *a, const PcVideoSettings *b)
{
    char a_text[1024], b_text[1024];
    /* The persistent value format excludes structure padding, which assignments need not retain. */
    pc_video_format(a, a_text, sizeof(a_text)); pc_video_format(b, b_text, sizeof(b_text));
    return !strcmp(a_text, b_text) && a->render_override_w == b->render_override_w && a->render_override_h == b->render_override_h;
}

static void test_native_video_pages(void)
{
    static const char *const options[] = { "OptionsMenuXbox", "PauseMenuOptionXbox" };
    static const int display_rows[] = { PCV_ROW_MODE, PCV_ROW_WINDOW, PCV_ROW_INTERNAL, PCV_ROW_ASPECT,
        PCV_ROW_SCALE, PCV_ROW_FILTER, PCV_ROW_VSYNC, PCV_ROW_FPS };
    static const int graphics_rows[] = { PCV_ROW_AA, PCV_ROW_AF, PCV_ROW_DOF, PCV_ROW_SHARPEN,
        PCV_ROW_AO_METHOD, PCV_ROW_AO_QUALITY, PCV_ROW_FOV, PCV_ROW_BLUR };
    PcVideoSettings s, d;
    int variant, i;
    video_fresh();
    for (variant = 0; variant < 2; variant++) {
        const char *page = options[variant];
        video_menu_fixture();
        CHECK(tick(page) == 1, "%s is native", page);
        press(PCM_ACT_DOWN, page); press(PCM_ACT_DOWN, page);
        log_clear();
        press(PCM_ACT_CONFIRM, page);
        CHECK(display_has("VIDEO SETTINGS") && display_has("DISPLAY") && pc_menu_test_focus() == 0,
              "video opens the native settings hub over %s", page);
        CHECK(log_is("sound SNDOpen\nsound SNDSelect\n") && !pc_video_menu_is_open(),
              "the hub uses native page sounds and leaves the popup closed: '%s'", g_log);
        press(PCM_ACT_CONFIRM, page);
        CHECK(display_has("WINDOW MODE") && display_has("WINDOWED") && display_has("F11 switches"),
              "Display draws its settings, live values and the focused row's help");
        for (i = 0; i < 8; i++) {
            unsigned generation = pc_video_generation();
            char value[96];
            log_clear(); press(PCM_ACT_RIGHT, page);
            CHECK(pc_video_generation() == generation + 1 && log_is("sound SNDChange\n"),
                  "Display row %d changes the real model once with Right: '%s'", i, g_log);
            pc_video_menu_row_value(display_rows[i], value, sizeof(value));
            CHECK(display_has(value), "Display row %d publishes its current value '%s'", i, value);
            log_clear(); press(PCM_ACT_LEFT, page);
            CHECK(pc_video_generation() == generation + 2 && log_is("sound SNDChange\n"),
                  "Display row %d changes back once with Left", i);
            if (i < 7) press(PCM_ACT_DOWN, page);
        }
        CHECK(display_has("FRAME LIMIT") && display_has("Caps how often"), "the last display row has frame limit help");
        press(PCM_ACT_BACK, page);
        CHECK(display_has("GRAPHICS") && !display_has("WINDOW MODE") && pc_menu_test_focus() == 0,
              "Back from Display restores the hub's Display focus");
        press(PCM_ACT_DOWN, page); press(PCM_ACT_CONFIRM, page);
        CHECK(display_has("ANTI-ALIASING") && display_has("AMBIENT OCCLUSION") && display_has("AO QUALITY") && display_has("GPU renderer"),
              "Graphics has its own rows, English AO controls and GPU availability help");
        for (i = 0; i < 8; i++) {
            unsigned generation = pc_video_generation();
            char value[96];
            log_clear(); press(PCM_ACT_CONFIRM, page);
            CHECK(pc_video_generation() == generation + 1 && log_is("sound SNDSelect\n"),
                  "Graphics row %d changes with Confirm and keeps the page open", i);
            pc_video_menu_row_value(graphics_rows[i], value, sizeof(value));
            if (i != 6) CHECK(display_has(value), "Graphics row %d publishes '%s'", i, value);
            if (i < 7) press(PCM_ACT_DOWN, page);
        }
        pc_video_get(&s);
        CHECK(s.aa == PCV_AA_SSAA && s.af == 1 && s.dof == PCV_STEP_LOW && s.sharpen == PCV_STEP_LOW &&
              s.ao_method == PCV_AO_SSAO && s.ao_quality == PCV_AOQ_ULTRA && s.fov == 35 && s.motion_blur_off,
              "Graphics changes all eight real settings");
        pc_video_test_reset(g_video_ini); pc_video_get(&d);
        CHECK(same_video_settings(&s, &d), "native row changes persist in the private settings file");
        press(PCM_ACT_BACK, page);
        CHECK(display_has("RESET TO DEFAULTS") && pc_menu_test_focus() == 1,
              "Back from Graphics restores the hub's Graphics focus");
        press(PCM_ACT_DOWN, page); log_clear(); press(PCM_ACT_CONFIRM, page);
        pc_video_get(&s); pc_video_defaults(&d);
        CHECK(same_video_settings(&s, &d) && log_is("sound SNDSelect\n") && display_has("DISPLAY"),
              "Reset applies defaults and keeps the hub visible");
        log_clear(); press(PCM_ACT_BACK, page);
        CHECK(pc_menu_test_focus() == 2 && display_has(variant ? "PAUSE OPTIONS" : "OPTIONS") && !display_has("RESET TO DEFAULTS"),
              "second Back restores %s with Video Settings selected", page);
        CHECK(!strstr(g_log, "page ") && !strstr(g_log, "gamecode "), "nested Back does not navigate the hidden guest page: '%s'", g_log);
        video_fresh();
    }
    /* A pinned setting remains focusable for its explanation, but cannot change or be reset. */
    _putenv_s("RECOMP_VSYNC", "0"); _putenv_s("RECOMP_BLACK_FOV", "90");
    pc_video_test_reset(g_video_ini);
    video_menu_fixture(); tick("OptionsMenuXbox"); pc_menu_request_video(); tick("OptionsMenuXbox");
    press(PCM_ACT_CONFIRM, "OptionsMenuXbox");
    for (i = 0; i < 6; i++) press(PCM_ACT_DOWN, "OptionsMenuXbox");
    CHECK(pc_menu_test_focus() == 6 && display_has("environment variable"), "a pinned VSync row exposes its lock explanation");
    log_clear(); press(PCM_ACT_RIGHT, "OptionsMenuXbox"); pc_video_get(&s);
    CHECK(!s.vsync && log_is("sound SNDError\n"), "Right cannot change pinned VSync");
    log_clear(); press(PCM_ACT_CONFIRM, "OptionsMenuXbox"); pc_video_get(&s);
    CHECK(!s.vsync && log_is("sound SNDError\n"), "Confirm cannot change pinned VSync");
    press(PCM_ACT_BACK, "OptionsMenuXbox");
    press(PCM_ACT_DOWN, "OptionsMenuXbox"); press(PCM_ACT_DOWN, "OptionsMenuXbox");
    press(PCM_ACT_CONFIRM, "OptionsMenuXbox"); pc_video_get(&s);
    CHECK(!s.vsync && s.fov == 90 && s.aa == PCV_AA_FXAA, "hub Reset preserves pinned fields and resets other settings");
    video_fresh();
}

static void test_pause(void)
{
    reset_game();
    CHECK(tick("PauseMenu") == 1, "PauseMenu is native");
    CHECK(pc_menu_test_focus() == 0, "focus starts on Continue (%d)", pc_menu_test_focus());
    press(PCM_ACT_DOWN, "PauseMenu");
    CHECK(pc_menu_test_focus() == 1 && log_is("sound SNDChange\n"), "down moves to Objectives Status with the change sound: %d '%s'", pc_menu_test_focus(), g_log);
    log_clear();
    press(PCM_ACT_UP, "PauseMenu"); press(PCM_ACT_UP, "PauseMenu");
    CHECK(pc_menu_test_focus() == 4, "up from Continue wraps to Quit (%d)", pc_menu_test_focus());
    press(PCM_ACT_DOWN, "PauseMenu");
    CHECK(pc_menu_test_focus() == 0, "down from Quit wraps to Continue (%d)", pc_menu_test_focus());
    log_clear();
    press(PCM_ACT_CONFIRM, "PauseMenu");
    CHECK(log_is("gamecode ClosePauseMenu 0 0 0\nsound SNDSelect\n"), "Continue closes the pause menu: '%s'", g_log);
    log_clear();
    press(PCM_ACT_BACK, "PauseMenu");
    CHECK(log_is("gamecode ClosePauseMenu 0 0 0\nsound SNDBack\n"), "back closes it too: '%s'", g_log);
    log_clear();
    press(PCM_ACT_START, "PauseMenu");
    CHECK(log_is("gamecode ClosePauseMenu 0 0 0\nsound SNDBack\n"), "START is back on the pause page: '%s'", g_log);
    log_clear();
    g_busy = 1;
    press(PCM_ACT_CONFIRM, "PauseMenu");
    CHECK(log_is("sound SNDError\n"), "a waiting request refuses Continue, with the error sound: '%s'", g_log);
    log_clear();
    press(PCM_ACT_BACK, "PauseMenu");
    CHECK(log_is("gamecode ClosePauseMenu 0 0 0\nsound SNDBack\n"), "back does not wait (mActionBackGameCode): '%s'", g_log);
    g_busy = 0;
    log_clear();
    press(PCM_ACT_DOWN, "PauseMenu");
    press(PCM_ACT_CONFIRM, "PauseMenu");
    CHECK(log_is("sound SNDChange\npage PauseMenuObjectives\nsound SNDSelect\n"), "Objectives Status is a page: '%s'", g_log);
    log_clear();
    pc_menu_test_action(PCM_ACT_DOWN);
    pc_menu_tick("PauseMenu", 0, &g_host);
    CHECK(pc_menu_test_focus() == 1 && !g_log[0], "input while the page is not ready is dropped: %d '%s'", pc_menu_test_focus(), g_log);
}

static void test_confirm(void)
{
    reset_game();
    CHECK(tick("ConfirmWarning") == 0, "ConfirmWarning without an earlier native page is the game's");
    tick("PauseMenu");
    press(PCM_ACT_DOWN, "PauseMenu"); press(PCM_ACT_DOWN, "PauseMenu");
    log_clear();
    press(PCM_ACT_CONFIRM, "PauseMenu");
    CHECK(log_is("page ConfirmWarning\ngamecode SetupConfirmWarning RestartConfirm 0 0\nsound SNDSelect\n"),
          "Restart Mission asks first (page, then the warning text): '%s'", g_log);
    CHECK(tick("ConfirmWarning") == 1 && pc_menu_test_focus() == 1, "the confirmation is native, focus on No (%d)", pc_menu_test_focus());
    log_clear();
    press(PCM_ACT_CONFIRM, "ConfirmWarning");
    CHECK(log_is("page PauseMenu\nsound SNDSelect\n"), "No goes back to the pause menu: '%s'", g_log);
    tick("PauseMenu");
    tick("ConfirmWarning");
    log_clear();
    press(PCM_ACT_UP, "ConfirmWarning");
    press(PCM_ACT_CONFIRM, "ConfirmWarning");
    CHECK(log_is("sound SNDChange\ngamecode RestartLevel 0 0 0\nsound SNDSelect\n"), "Yes restarts the level: '%s'", g_log);
    log_clear();
    press(PCM_ACT_BACK, "ConfirmWarning");
    CHECK(log_is("page PauseMenu\nsound SNDBack\n"), "back is No: '%s'", g_log);
    tick("");
    g_ngvars = 0;                                      /* a fresh game: nobody asked */
    CHECK(tick("ConfirmWarning") == 0, "closing the menu forgets the page's own answers; without the game's, the page is the game's");
}

/* A native page and a page of the game's own hand the question over through the game's variables. */
static void test_mixed(void)
{
    char v[48];
    reset_game();
    tick("PauseMenu");
    press(PCM_ACT_DOWN, "PauseMenu"); press(PCM_ACT_DOWN, "PauseMenu"); press(PCM_ACT_DOWN, "PauseMenu"); press(PCM_ACT_DOWN, "PauseMenu");
    press(PCM_ACT_CONFIRM, "PauseMenu");
    CHECK(f_get_var(NULL, "FEConfirmYesGameCode", v, sizeof(v)) && !strcmp(v, "QuitLevel"),
          "Quit writes the game's own FEConfirmYesGameCode, for the game's confirmation page (%s)", v);
    CHECK(f_get_var(NULL, "FEConfirmNoPage", v, sizeof(v)) && !strcmp(v, "PauseMenu"), "and FEConfirmNoPage (%s)", v);
    /* the other way: the game's own pause page asked; the native confirmation reads its answers */
    reset_game();
    f_set_var(NULL, "FEConfirmYesGameCode", "RestartMission");
    f_set_var(NULL, "FEConfirmNoPage", "MissionFailed");
    CHECK(tick("ConfirmWarning") == 1, "the native confirmation takes a question the game's page asked");
    log_clear();
    press(PCM_ACT_UP, "ConfirmWarning");
    press(PCM_ACT_CONFIRM, "ConfirmWarning");
    CHECK(log_is("sound SNDChange\ngamecode RestartMission 0 0 0\nsound SNDSelect\n"), "its Yes is the game's answer: '%s'", g_log);
    log_clear();
    press(PCM_ACT_BACK, "ConfirmWarning");
    CHECK(log_is("page MissionFailed\nsound SNDBack\n"), "and its back the game's No page: '%s'", g_log);
}

static void test_options(void)
{
    reset_game();
    tick("PauseMenuOptionXbox");
    CHECK(pc_menu_test_focus() == 0, "focus starts on Toggle Crouch (%d)", pc_menu_test_focus());
    press(PCM_ACT_LEFT, "PauseMenuOptionXbox");
    CHECK(log_is("gamecode ToggleCrouchOnOff 1 0 0\n"), "left is ON: '%s'", g_log);
    log_clear();
    press(PCM_ACT_LEFT, "PauseMenuOptionXbox");
    CHECK(!g_log[0], "left again changes nothing: '%s'", g_log);
    press(PCM_ACT_RIGHT, "PauseMenuOptionXbox");
    CHECK(log_is("gamecode ToggleCrouchOnOff 0 0 0\n"), "right is OFF: '%s'", g_log);
    press(PCM_ACT_DOWN, "PauseMenuOptionXbox"); press(PCM_ACT_DOWN, "PauseMenuOptionXbox"); press(PCM_ACT_DOWN, "PauseMenuOptionXbox");
    log_clear();
    press(PCM_ACT_RIGHT, "PauseMenuOptionXbox");
    CHECK(log_is("gamecode SetSfxValue 52 0 0\n"), "the SFX slider moves one step of 4 from the game's 48: '%s'", g_log);
    {
        int i, n = 0;
        const char *p;
        log_clear();
        for (i = 0; i < 20; i++) press(PCM_ACT_LEFT, "PauseMenuOptionXbox");
        for (p = g_log; (p = strstr(p, "SetSfxValue")) != NULL; p++) n++;
        CHECK(n == 13 && strstr(g_log, "SetSfxValue 0 0 0"), "it stops at 0 after 13 steps: %d", n);
    }
    log_clear();
    press(PCM_ACT_CONFIRM, "PauseMenuOptionXbox");
    CHECK(log_is("page PauseMenu\nsound SNDSelect\n"), "confirm on a setting goes back, as the original: '%s'", g_log);
    tick("PauseMenu");
    tick("PauseMenuOptionXbox");
    log_clear();
    press(PCM_ACT_BACK, "PauseMenuOptionXbox");
    CHECK(log_is("page PauseMenu\nsound SNDBack\n"), "back is the pause menu: '%s'", g_log);
    log_clear();
    press(PCM_ACT_START, "PauseMenuOptionXbox");
    CHECK(log_is("page PauseMenu\nsound SNDSelect\n"), "START is confirm on the options page: '%s'", g_log);
    log_clear();
    press(PCM_ACT_UP, "PauseMenuOptionXbox");
    press(PCM_ACT_CONFIRM, "PauseMenuOptionXbox");
    CHECK(log_is("sound SNDChange\nvideo\nsound SNDSelect\n"), "the last item opens the Video Settings page: '%s'", g_log);
}

/* The pointer, in a 640 x 480 client area (menu units are pixels): sweep it over the page to find the rows,
 * then hover and click them. Before the pointer has moved, a left click is still the keyboard's confirm. */
static void click(const char *page)
{
    pc_menu_test_button(0, 1);
    pc_menu_test_button(0, 0);
    tick(page);
}

static int sweep(const char *page, int top[8], int left[8], int right[8])
{
    int x, y, f, n = 0;
    for (f = 0; f < 8; f++) { top[f] = -1; left[f] = 9999; right[f] = -1; }
    for (y = 0; y < 480; y += 2)
        for (x = 0; x < 640; x += 4) {
            pc_menu_pointer(x, y);
            tick(page);
            f = pc_menu_test_pointer_item();
            if (f < 0 || f >= 8) continue;
            if (top[f] < 0) { top[f] = y; n++; }
            if (y == top[f] + 10) { if (x < left[f]) left[f] = x; if (x > right[f]) right[f] = x; }
        }
    return n;
}

static void test_pointer(void)
{
    int top[8], left[8], right[8], n, f;
    reset_game();
    tick("PauseMenu");
    pc_menu_test_pointer_space(640, 480);
    log_clear();
    click("PauseMenu");
    CHECK(log_is("gamecode ClosePauseMenu 0 0 0\nsound SNDSelect\n"), "before the pointer moves, a click confirms the focused item: '%s'", g_log);

    reset_game();
    tick("PauseMenu");
    pc_menu_test_pointer_space(640, 480);
    n = sweep("PauseMenu", top, left, right);
    CHECK(n == 5, "the sweep finds the pause menu's five rows (%d)", n);
    for (f = 1; f < n; f++) CHECK(top[f] > top[f - 1], "row %d is below row %d (%d, %d)", f, f - 1, top[f], top[f - 1]);
    pc_menu_pointer(left[0] + 4, top[0] + 10); tick("PauseMenu");
    log_clear();
    pc_menu_pointer(left[1] + 4, top[1] + 10); tick("PauseMenu");
    CHECK(pc_menu_test_focus() == 1 && log_is("sound SNDChange\n"), "hovering a row focuses it with the change sound: %d '%s'", pc_menu_test_focus(), g_log);
    log_clear();
    pc_menu_pointer(left[1] + 20, top[1] + 12); tick("PauseMenu");
    CHECK(pc_menu_test_focus() == 1 && !g_log[0], "moving within the row changes nothing: '%s'", g_log);
    click("PauseMenu");
    CHECK(log_is("page PauseMenuObjectives\nsound SNDSelect\n"), "a click opens the hovered row: '%s'", g_log);

    reset_game();
    tick("PauseMenu");
    pc_menu_test_pointer_space(640, 480);
    pc_menu_pointer(left[0] + 4, top[0] + 10); tick("PauseMenu");
    pc_menu_pointer(4, 4); tick("PauseMenu");
    log_clear();
    click("PauseMenu");
    CHECK(pc_menu_test_focus() == 0 && !g_log[0], "a click away from the rows does nothing: %d '%s'", pc_menu_test_focus(), g_log);
    reset_game();
    tick("PauseMenu");
    press(PCM_ACT_UP, "PauseMenu");
    {
        char keyboard[512];
        log_clear();
        press(PCM_ACT_CONFIRM, "PauseMenu");
        snprintf(keyboard, sizeof(keyboard), "%s", g_log);
        reset_game();
        tick("PauseMenu");
        pc_menu_test_pointer_space(640, 480);
        pc_menu_pointer(left[4] + 4, top[4] + 10); tick("PauseMenu");
        log_clear();
        click("PauseMenu");
        CHECK(!strcmp(g_log, keyboard), "clicking the last row does what confirming it does: '%s' vs '%s'", g_log, keyboard);
    }

    /* ON / OFF: a click on the row's right part is OFF, on its left part ON */
    reset_game();
    tick("PauseMenuOptionXbox");
    pc_menu_test_pointer_space(640, 480);
    n = sweep("PauseMenuOptionXbox", top, left, right);
    CHECK(n >= 4 && top[0] >= 0 && right[0] > left[0], "the options rows are found (%d)", n);
    pc_menu_pointer(left[0] + 4, top[0] + 10); tick("PauseMenuOptionXbox");
    log_clear();
    click("PauseMenuOptionXbox");
    CHECK(log_is("gamecode ToggleCrouchOnOff 1 0 0\n"), "a click on the left of a toggle row is ON: '%s'", g_log);
    pc_menu_pointer(right[0] - 2, top[0] + 10); tick("PauseMenuOptionXbox");
    log_clear();
    click("PauseMenuOptionXbox");
    CHECK(log_is("gamecode ToggleCrouchOnOff 0 0 0\n"), "a click on the right of a toggle row is OFF: '%s'", g_log);
}

static void test_mission_failed(void)
{
    reset_game();
    value_set("FE_LevelAllowContinue", 0);
    tick("MissionFailed");
    CHECK(pc_menu_test_focus() == 1, "without a checkpoint the focus starts on Restart (%d)", pc_menu_test_focus());
    press(PCM_ACT_UP, "MissionFailed");
    CHECK(pc_menu_test_focus() == 2, "up skips the disabled Continue (%d)", pc_menu_test_focus());
    {
        char dump[16384];
        pc_menu_test_dump(dump, sizeof(dump));
        CHECK(strstr(dump, "FF202B26") != NULL, "the disabled Continue is still shown, greyed:\n%s", dump);
    }
    log_clear();
    press(PCM_ACT_BACK, "MissionFailed");
    CHECK(log_is("sound SNDError\n"), "mission failed has no back action: '%s'", g_log);
    reset_game();
    tick("MissionFailed");
    CHECK(pc_menu_test_focus() == 0, "with a checkpoint the focus starts on Continue (%d)", pc_menu_test_focus());
    log_clear();
    press(PCM_ACT_CONFIRM, "MissionFailed");
    CHECK(log_is("gamecode ContinueMission 0 0 0\nsound SNDSelect\n"), "Continue continues: '%s'", g_log);
    CHECK(tick("PressStart") == 0, "a page the file does not list is the game's");
}

static void test_objectives(void)
{
    static const char def[] =
        "[page PauseMenuObjectives]\n"
        "label = 246 108 $FE_OBJECTIVESTARGET\n"
        "text = 589 108 &FE_OBJECTIVESTARGETTOTAL | right\n"
        "line = 245 129 592\n"
        "list = 247 144 26\n"
        "values = 589\n"
        "highlight = off\n"
        "back = page PauseMenu\n"
        "item = $FE_BLACKMAIL | none | value &FE_BLACKMAILRATING | focus gamecode ChangeObjectiveHint 2 | dim unless FEDiffMode\n"
        "item = $FE_SECONDARY | none | value &FE_SECONDARYRATING | focus gamecode ChangeObjectiveHint 1 | dim unless FEDiffMode=2\n";
    char dump[16384];
    reset_game();
    value_set("FEDiffMode", 1);
    text_set("FE_OBJECTIVESTARGETTOTAL", "1/8"); text_set("FE_BLACKMAILRATING", "0/3"); text_set("FE_SECONDARYRATING", "-");
    pc_menu_test_reset(def);
    tick("");
    tick("PauseMenuObjectives");
    pc_menu_test_dump(dump, sizeof(dump));
    CHECK(strstr(dump, "text 247.0 144.0 15.0 1.0 FF36473D blackmail") != NULL, "a plain focused row keeps the item colour and size:\n%s", dump);
    CHECK(strstr(dump, "text 247.0 170.0 15.0 1.0 FF2B362B destruction") != NULL, "FEDiffMode=2 rows are dimmed at mode 1");
    CHECK(strstr(dump, "rect 245.0 129.0 347.0 2.0") != NULL, "a separator line");
    {
        /* right-aligned: the value ends on x = 589 */
        const char *v = strstr(dump, " 0/3");
        float x = 0;
        if (v) { const char *q = v; while (q > dump && q[-1] != '\n') q--; sscanf(q, "text %f", &x); }
        CHECK(v && x > 540 && x < 580, "the value is right-aligned (%.1f)", x);
    }
    log_clear();
    press(PCM_ACT_DOWN, "PauseMenuObjectives");
    CHECK(log_is("sound SNDChange\ngamecode ChangeObjectiveHint 1 0 0\n"), "moving onto a row asks for its hint: '%s'", g_log);
    log_clear();
    press(PCM_ACT_CONFIRM, "PauseMenuObjectives");
    CHECK(log_is("page PauseMenu\nsound SNDSelect\n"), "confirm on a row goes back, as the original: '%s'", g_log);
    value_set("FEDiffMode", 2);
    tick("PauseMenuObjectives");
    pc_menu_test_dump(dump, sizeof(dump));
    CHECK(strstr(dump, "FF2B362B destruction") == NULL, "at mode 2 nothing is dimmed");
}

/* The main menu with EXIT GAME, which asks first on a page of the file that the game does not have. */
static void test_exit_game(void)
{
    static const char def[] =
        "[page blacktitlemenu2]\n"
        "column = 00000000\n"
        "list = 319.5 150.5 25\n"
        "prompts = select confirm\n"
        "item = $FE_STARTMISSION | page Difficulty\n"
        "item = $FE_OPTIONS | page OptionsMenuXbox\n"
        "item = \"EXIT GAME\" | open ExitGame\n"
        "item = \"NOWHERE\" | open Nowhere | if ShowNowhere\n"
        "[page ExitGame]\n"
        "text = 301 188 \"ARE YOU SURE YOU WANT TO QUIT?\" | wrap 289\n"
        "focus = 2\n"
        "back = close\n"
        "item = $FE_YES | exit\n"
        "item = $FE_NO | close\n";
    char dump[16384];
    reset_game();
    pc_menu_test_reset(def);
    tick("");
    CHECK(tick("blacktitlemenu2") == 1 && pc_menu_test_focus() == 0, "the main menu is native, focus on Start Mission (%d)", pc_menu_test_focus());
    {
        uint32_t *pixels = (uint32_t *)calloc(640 * 480, sizeof(uint32_t));
        pc_menu_test_render(pixels, 640, 480);
        CHECK(pixels[100 * 640 + 150] == 0, "the front-end movie between the rails has no added tint");
        CHECK((pixels[100 * 640 + 30] >> 24) > 200, "the front-end retains the original dark shade outside the rails");
        free(pixels);
    }
    log_clear();
    press(PCM_ACT_BACK, "blacktitlemenu2");
    CHECK(log_is("sound SNDError\n"), "back on the main menu does nothing, with the error sound, as the original: '%s'", g_log);
    log_clear();
    press(PCM_ACT_CONFIRM, "blacktitlemenu2");
    CHECK(log_is("page Difficulty\nsound SNDSelect\n"), "Start Mission is the difficulty page: '%s'", g_log);
    log_clear();
    press(PCM_ACT_UP, "blacktitlemenu2");
    CHECK(pc_menu_test_focus() == 2, "up wraps to EXIT GAME, past the hidden item (%d)", pc_menu_test_focus());
    log_clear();
    press(PCM_ACT_CONFIRM, "blacktitlemenu2");
    CHECK(log_is("sound SNDOpen\nsound SNDSelect\n") && pc_menu_test_focus() == 1, "EXIT GAME asks (with the page's open sound), focus on No: %d '%s'", pc_menu_test_focus(), g_log);
    tick("blacktitlemenu2");
    pc_menu_test_dump(dump, sizeof(dump));
    CHECK(strstr(dump, "ARE YOU SURE") != NULL && pc_menu_test_focus() == 1,
          "the question stays up over the game's main menu:\n%s", dump);
    log_clear();
    press(PCM_ACT_BACK, "blacktitlemenu2");
    pc_menu_test_dump(dump, sizeof(dump));
    CHECK(log_is("sound SNDOpen\nsound SNDBack\n") && pc_menu_test_focus() == 2 && !strstr(dump, "ARE YOU SURE"),
          "back leaves the question, focus on EXIT GAME again: %d '%s'", pc_menu_test_focus(), g_log);
    press(PCM_ACT_CONFIRM, "blacktitlemenu2");
    log_clear();
    press(PCM_ACT_CONFIRM, "blacktitlemenu2");
    CHECK(log_is("sound SNDOpen\nsound SNDSelect\n") && pc_menu_test_focus() == 2, "No leaves it too: %d '%s'", pc_menu_test_focus(), g_log);
    log_clear();
    press(PCM_ACT_BACK, "blacktitlemenu2");
    CHECK(log_is("sound SNDError\n"), "back on the main menu is still nothing (no page to close): '%s'", g_log);
    press(PCM_ACT_CONFIRM, "blacktitlemenu2");
    log_clear();
    press(PCM_ACT_UP, "blacktitlemenu2");
    press(PCM_ACT_CONFIRM, "blacktitlemenu2");
    CHECK(log_is("sound SNDChange\nquit\nsound SNDSelect\n"), "Yes closes the game: '%s'", g_log);
    /* the game changes page while the question is up (the attract movie): it goes, the menu starts afresh */
    tick("FMVPlayer");
    CHECK(tick("BlackTitleMenu2") == 1 && pc_menu_test_focus() == 0, "back on the main menu (named as the game names it on the way back), focus on Start Mission (%d)", pc_menu_test_focus());
    pc_menu_test_dump(dump, sizeof(dump));
    CHECK(!strstr(dump, "ARE YOU SURE"), "without the question");
    value_set("ShowNowhere", 1);
    press(PCM_ACT_UP, "BlackTitleMenu2");
    log_clear();
    press(PCM_ACT_CONFIRM, "BlackTitleMenu2");
    CHECK(log_is("sound SNDError\n") && pc_menu_test_focus() == 3, "opening a page the file does not have does nothing: %d '%s'", pc_menu_test_focus(), g_log);
}

static uint64_t fnv1a64(const char *s)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 0x100000001B3ull; }
    return h;
}

static int file_is(const char *path, const char *want)
{
    size_t n = 0;
    uint8_t *d = read_all(path, &n);
    const int same = d && n == strlen(want) && !memcmp(d, want, n);
    free(d);
    return same;
}

static void file_put(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(text, 1, strlen(text), f); fclose(f); }
}

/* An earlier build's default that was never edited is brought up to date; an edited one is the player's. */
static void test_replaced_default(void)
{
    static const char old_def[] = "[page A]\r\nitem = \"OLD\" | none\r\n";
    static const char new_def[] = "[page A]\r\nitem = \"OLD\" | none\r\n[page B]\r\nitem = \"NEW\" | close\r\n";
    static const char edited[] = "[page A]\r\nitem = \"MINE\" | none\r\n";
    char dir[MAX_PATH], path[MAX_PATH];
    static uint64_t replaced[1];
    int n;
    GetTempPathA(sizeof(dir), dir);
    snprintf(path, sizeof(path), "%spc-menu-test-%lu.ini", dir, (unsigned long)GetCurrentProcessId());
    replaced[0] = fnv1a64(old_def);
    pc_menu_set_default_definition(new_def);
    pc_menu_set_replaced_defaults(replaced, 1);
    file_put(path, old_def);
    n = pc_menu_test_load_file(path);
    CHECK(n == 2 && file_is(path, new_def), "an untouched earlier default is replaced (%d pages)", n);
    file_put(path, edited);
    n = pc_menu_test_load_file(path);
    CHECK(n == 1 && file_is(path, edited), "an edited file stays as it is (%d pages)", n);
    DeleteFileA(path);
    n = pc_menu_test_load_file(path);
    CHECK(n == 2 && file_is(path, new_def), "a missing file is written from the default (%d pages)", n);
    DeleteFileA(path);
    pc_menu_set_default_definition(NULL);
    pc_menu_set_replaced_defaults(NULL, 0);
}

static void test_video_file_supplements(void)
{
    static const char edited[] =
        "; this file belongs to the player; preserve every byte\r\n"
        "[style]\r\nitem = 00CC44\r\n"
        "[page OptionsMenuXbox]\r\ntitle = \"MY OPTIONS\"\r\nitem = \"VIDEO\" | video\r\n"
        "[page VideoGraphics]\r\ntitle = \"MY GRAPHICS\"\r\nback = close\r\nsetting = \"MY FOV\" | fov\r\n";
    char dir[MAX_PATH], path[MAX_PATH];
    static uint64_t replaced[1];
    int n;
    GetTempPathA(sizeof(dir), dir);
    snprintf(path, sizeof(path), "%spc-menu-supplement-%lu.ini", dir, (unsigned long)GetCurrentProcessId());
    video_fresh(); video_menu_fixture();
    pc_menu_set_default_definition(k_video_def);
    file_put(path, edited);
    n = pc_menu_test_load_file(path);
    CHECK(n == 4 && file_is(path, edited), "an edited file gains only the two missing video pages in memory (%d pages)", n);
    CHECK(tick("PauseMenuOptionXbox") == 0, "supplementing video pages does not add omitted native Options pages");
    tick("OptionsMenuXbox"); press(PCM_ACT_CONFIRM, "OptionsMenuXbox");
    CHECK(display_has("RESET TO DEFAULTS"), "the missing video hub is supplemented");
    press(PCM_ACT_CONFIRM, "OptionsMenuXbox");
    CHECK(display_has("WINDOW MODE"), "the missing Display page is supplemented");
    press(PCM_ACT_BACK, "OptionsMenuXbox"); press(PCM_ACT_DOWN, "OptionsMenuXbox"); press(PCM_ACT_CONFIRM, "OptionsMenuXbox");
    CHECK(display_has("MY GRAPHICS") && display_has("MY FOV") && !display_has("ANTI-ALIASING"), "an edited Graphics page wins over the built-in page");
    CHECK(file_is(path, edited), "using supplemented settings keeps the edited file's original bytes");
    DeleteFileA(path);
    pc_menu_set_default_definition(NULL);
    pc_menu_set_replaced_defaults(NULL, 0);

    /* Exercise the actual BLACK v2 -> native-video default migration when the game's source is here. */
    {
        const char *new_path = getenv("PCM_DEFAULT_DEFINITION"), *old_path = getenv("PCM_PREVIOUS_DEFINITION");
        uint8_t *new_def, *old_def;
        size_t new_size = 0, old_size = 0;
        if (!new_path) new_path = "menus-default.ini";
        if (!old_path) old_path = "menus-previous.ini";
        new_def = read_all(new_path, &new_size); old_def = read_all(old_path, &old_size);
        if (new_def && old_def) {
            replaced[0] = 0xBBE0D874943DB641ull;
            CHECK(fnv1a64((const char *)old_def) == replaced[0], "the actual v2 default matches its registered migration hash");
            pc_menu_set_default_definition((const char *)new_def);
            pc_menu_set_replaced_defaults(replaced, 1);
            video_menu_fixture();
            file_put(path, (const char *)old_def);
            n = pc_menu_test_load_file(path);
            CHECK(n == 12 && file_is(path, (const char *)new_def), "BLACK's untouched v2 default migrates to the 12 native pages (%d)", n);
            tick("OptionsMenuXbox");
            press(PCM_ACT_UP, "OptionsMenuXbox"); press(PCM_ACT_CONFIRM, "OptionsMenuXbox");
            CHECK(display_has("RESET TO DEFAULTS") && !pc_video_menu_is_open(), "the migrated front-end Options item opens the native hub");
            DeleteFileA(path);
            pc_menu_set_default_definition(NULL); pc_menu_set_replaced_defaults(NULL, 0);
        } else printf("(native-video default artifacts absent: actual BLACK v2 migration check skipped)\n");
        free(new_def); free(old_def);
    }
    video_fresh();
}

static void test_native_page_stack(void)
{
    char definition[4096] = "[style]\nintro = 0\nanimation = 0\n[page OptionsMenuXbox]\nfocus = 3\n"
        "item = \"ONE\" | none\nitem = \"TWO\" | none\nitem = \"ENTER\" | open Chain0\n";
    int i;
    for (i = 0; i < 10; i++) {
        char page[240];
        snprintf(page, sizeof(page), "[page Chain%d]\ntitle = \"CHAIN %d\"\nfocus = 2\nback = close\n"
            "item = \"FIRST\" | none\nitem = \"NEXT\" | open Chain%d\n", i, i, i + 1);
        strncat(definition, page, sizeof(definition) - strlen(definition) - 1);
    }
    reset_game(); pc_menu_test_reset(definition); tick(""); tick("OptionsMenuXbox");
    CHECK(pc_menu_test_focus() == 2, "the stack's underlying native Options page starts on Enter");
    for (i = 0; i < 8; i++) {
        char title[32];
        press(PCM_ACT_CONFIRM, "OptionsMenuXbox");
        snprintf(title, sizeof(title), "CHAIN %d", i);
        CHECK(display_has(title) && pc_menu_test_focus() == 1, "nested open %d selects the child page's second item", i + 1);
    }
    log_clear(); press(PCM_ACT_CONFIRM, "OptionsMenuXbox");
    CHECK(display_has("CHAIN 7") && log_is("sound SNDError\n"), "opening past stack capacity fails without replacing its top page");
    for (i = 7; i >= 0; i--) {
        char title[32];
        press(PCM_ACT_BACK, "OptionsMenuXbox");
        if (i) {
            snprintf(title, sizeof(title), "CHAIN %d", i - 1);
            CHECK(display_has(title) && pc_menu_test_focus() == 1, "closing depth %d restores the parent's focus", i + 1);
        } else CHECK(!display_has("CHAIN") && pc_menu_test_focus() == 2, "the final close restores the native Options focus");
    }
    press(PCM_ACT_CONFIRM, "OptionsMenuXbox");
    CHECK(tick("FMVPlayer") == 0 && tick("OptionsMenuXbox") == 1 && pc_menu_test_focus() == 2 && !display_has("CHAIN"),
          "a real guest page change discards the complete native stack");
}

static void test_video_setting_focus_and_layout(void)
{
    static const char definition[] =
        "[style]\nintro = 0\nanimation = 0\n[page OptionsMenuXbox]\n"
        "list = 218 147 27\nselector = 349\n"
        "item = \"FIRST\" | none\n"
        "setting = \"WINDOW MODE\" | mode | focus gamecode FirstFocus; gamecode SecondFocus\n";
    char dump[16384], *line;
    float x = -1;
    video_fresh(); reset_game(); pc_menu_test_reset(definition); tick(""); tick("OptionsMenuXbox");
    log_clear(); press(PCM_ACT_DOWN, "OptionsMenuXbox");
    CHECK(log_is("sound SNDChange\ngamecode FirstFocus 0 0 0\ngamecode SecondFocus 0 0 0\n"),
          "a setting executes both focus actions: '%s'", g_log);
    pc_menu_test_dump(dump, sizeof(dump));
    for (line = strtok(dump, "\n"); line; line = strtok(NULL, "\n"))
        if (strstr(line, "WINDOWED")) sscanf(line, "text %f", &x);
    CHECK(x > 218 && x < 567, "a setting with no explicit values position stays inside the selector (x=%g)", x);
    video_fresh();
}

static void test_render(void)
{
    const unsigned w = 640, h = 480;
    uint32_t *px = (uint32_t *)calloc((size_t)w * h, 4);
    char dump[16384];
    reset_game();
    tick("PauseMenu");
    pc_menu_test_dump(dump, sizeof(dump));
    CHECK(strstr(dump, "text") && strstr(dump, "image"), "the pause page has texts and the logo:\n%s", dump);
    pc_menu_test_render(px, w, h);
    {
        /* the shade outside the rails, the rail itself, the selector bar behind Continue, and some text */
        const uint32_t shade = px[100 * w + 30], rail = px[100 * w + 75], bar = px[245 * w + 450];
        int ink = 0, x, y;
        for (y = 241; y < 252; y++) for (x = 285; x < 380; x++) if (((px[y * w + x] >> 8) & 255) > 90) ink++;
        CHECK((shade >> 24) > 200, "the game is shaded outside the rails (%08X)", shade);
        CHECK((rail >> 24) == 255 && ((rail >> 8) & 255) > 80, "the left rail (%08X)", rail);
        CHECK((bar >> 24) > 220 && ((bar >> 8) & 255) < 20, "the selector bar (%08X)", bar);
        CHECK(ink > 100, "CONTINUE is drawn (%d bright pixels)", ink);
    }
    free(px);
}

/* Real input through pc_input: the pad report and keys reach the page, and the game sees none of it. */
static void pad(uint8_t out[20], unsigned buttons, uint8_t a, uint8_t b)
{
    memset(out, 0, 20);
    out[1] = 20;
    out[2] = (uint8_t)buttons;                /* 1 up, 2 down, 4 left, 8 right, 0x10 start */
    out[4] = a; out[5] = b;
    pc_input_apply_report(out, 1);
}

static int neutral(const uint8_t out[20])
{
    int i;
    for (i = 2; i < 20; i++) if (out[i]) return 0;
    return 1;
}

static void test_input(void)
{
    uint8_t out[20];
    _putenv("RECOMP_BLACK_INPUT_MODE=auto");
    _putenv("RECOMP_BLACK_PROMPT_MODE=auto");
    pc_input_test_reset();
    pc_menu_install();
    pc_input_focus_event(1);
    reset_game();
    pc_menu_take_input();
    pc_input_key_event(0x50, 1, 1, 0);                 /* no page yet: the game's own input */
    pc_input_key_event(0x50, 1, 0, 0);
    CHECK(!pc_menu_take_input(), "input with no native page up is not the page's");
    tick("PauseMenu");
    pad(out, 2, 0, 0);
    CHECK(neutral(out), "the pad's down does not reach the game while the page is up");
    CHECK(pc_menu_take_input() && !pc_menu_take_input(), "the page's input is reported once to the game's idle check");
    tick("PauseMenu");
    CHECK(pc_menu_test_focus() == 1, "the pad's down moves the focus (%d)", pc_menu_test_focus());
    pad(out, 0, 0, 0);
    log_clear();
    pad(out, 0, 255, 0);
    CHECK(neutral(out), "A does not reach the game");
    tick("PauseMenu");
    CHECK(log_is("page PauseMenuObjectives\nsound SNDSelect\n"), "A confirms: '%s'", g_log);
    tick("PauseMenuObjectives");                       /* not a page in this definition: the game's own */
    pad(out, 0, 255, 0);
    CHECK(neutral(out), "A held from the page that left stays away from the game");
    pad(out, 0, 0, 0);
    pad(out, 0, 255, 0);
    CHECK(!neutral(out), "a new press of A on the game's page is the game's");
    pad(out, 0, 0, 0);
    tick("PauseMenu");
    log_clear();
    pad(out, 0x10, 0, 0);
    CHECK(neutral(out), "START does not reach the game");
    tick("PauseMenu");
    CHECK(log_is("gamecode ClosePauseMenu 0 0 0\nsound SNDBack\n"), "START closes the pause menu, as the original: '%s'", g_log);
    pad(out, 0, 0, 0);
    /* keys */
    tick("PauseMenu");
    log_clear();
    pc_menu_take_input();
    pc_input_key_event(0x50, 1, 1, 0);                 /* down arrow */
    pc_input_key_event(0x50, 1, 0, 0);
    CHECK(pc_menu_take_input(), "a key on the page is input for the game's idle check");
    tick("PauseMenu");
    CHECK(pc_menu_test_focus() == 1 && log_is("sound SNDChange\n"), "the down arrow moves the focus: %d '%s'", pc_menu_test_focus(), g_log);
    pc_input_key_event(0x4D, 1, 1, 0);                 /* right arrow, held: one step */
    pc_input_key_event(0x4D, 1, 1, 1);
    pc_input_key_event(0x4D, 1, 1, 1);
    pc_input_key_event(0x4D, 1, 0, 0);
    log_clear();
    pc_input_key_event(0x01, 0, 1, 0);                 /* Escape */
    tick("PauseMenu");
    CHECK(log_is("gamecode ClosePauseMenu 0 0 0\nsound SNDBack\n"), "Escape is back: '%s'", g_log);
    tick("");
    CHECK(!pc_menu_active(), "no native page once the menu is closed");
    pc_input_key_event(0x01, 0, 0, 0);                 /* its release is swallowed with it; the next press is the game's */
    /* The idle timer must see held controls even when they have no menu action. */
    reset_game();
    tick("PauseMenu");
    {
        int i, sign;
        for (i = 0; i < 8; i++) {
            pad(out, 1u << i, 0, 0);
            CHECK(neutral(out) && pc_menu_take_input(), "digital pad bit %d is swallowed and prevents attract", i);
        }
        for (i = 4; i < 12; i++) {
            const uint8_t threshold = i < 10 ? 36 : 46;
            memset(out, 0, sizeof(out)); out[1] = 20; out[i] = threshold - 1;
            pc_input_apply_report(out, 1);
            CHECK(!pc_menu_take_input(), "analog button %d below retail idle threshold allows attract", i - 4);
            memset(out, 0, sizeof(out)); out[1] = 20; out[i] = threshold;
            pc_input_apply_report(out, 1);
            CHECK(neutral(out) && pc_menu_take_input(), "analog button %d is input even without a menu action", i - 4);
        }
        for (i = 12; i < 20; i += 2) for (sign = -1; sign <= 1; sign += 2) {
            uint16_t axis = (uint16_t)(int16_t)(sign * 8847);
            memset(out, 0, sizeof(out)); out[1] = 20;
            out[i] = (uint8_t)axis; out[i + 1] = (uint8_t)(axis >> 8);
            pc_input_apply_report(out, 1);
            CHECK(!pc_menu_take_input(), "stick axis %d below retail threshold allows attract", (i - 12) / 2);
            axis = (uint16_t)(int16_t)(sign * 8848);
            memset(out, 0, sizeof(out)); out[1] = 20;
            out[i] = (uint8_t)axis; out[i + 1] = (uint8_t)(axis >> 8);
            pc_input_apply_report(out, 1);
            CHECK(neutral(out) && pc_menu_take_input(), "stick axis %d direction %d prevents attract", (i - 12) / 2, sign);
        }
        memset(out, 0, sizeof(out)); out[1] = 20; out[16] = 1;
        pc_input_apply_report(out, 1);
        CHECK(!pc_menu_take_input(), "small stick noise does not prevent attract");
        pc_input_mouse_button_event(2, 1);      /* middle mouse has no navigation action */
        CHECK(pc_menu_take_input(), "mouse press counts as input");
        for (i = 0; i < 2; i++) {
            pad(out, 0, 0, 0);
            CHECK(pc_menu_take_input(), "held mouse stays active on poll %d", i);
        }
        pc_input_mouse_button_event(2, 0);
        pad(out, 0, 0, 0);
        CHECK(!pc_menu_take_input(), "released mouse and neutral pad allow attract again");
    }
    pc_input_focus_event(0);
    pc_input_set_filter_slot(1, NULL);
    _putenv("RECOMP_BLACK_INPUT_MODE=keyboard_mouse");
    _putenv("RECOMP_BLACK_PROMPT_MODE=keyboard_mouse");
    pc_input_test_reset();
}

static void key_press(unsigned code, int extended)
{
    pc_input_key_event(code, extended, 1, 0);
    pc_input_key_event(code, extended, 0, 0);
}

static void video_shoulder(uint8_t out[20], uint8_t white)
{
    memset(out, 0, 20); out[1] = 20; out[9] = white;
    pc_input_apply_report(out, 1);
}

static void test_native_video_shortcuts(void)
{
    const char *page = "OptionsMenuXbox";
    PcVideoSettings s;
    uint8_t out[20];
    unsigned generation;
    video_fresh();
    _putenv_s("RECOMP_BLACK_INPUT_MODE", "auto");
    _putenv_s("RECOMP_BLACK_PROMPT_MODE", "auto");
    pc_input_test_reset();
    pc_video_menu_test_force_available(0);
    pc_video_menu_install(); pc_menu_install();
    pc_input_focus_event(1);
    video_menu_fixture(); tick(page);
    press(PCM_ACT_DOWN, page); press(PCM_ACT_DOWN, page);
    pc_video_menu_page_seen(); pc_menu_take_input();
    pc_input_key_event(0x2F, 0, 1, 0); pc_input_key_event(0x2F, 0, 1, 1);
    pc_input_key_event(0x2F, 0, 0, 0);
    CHECK(!pc_video_menu_is_open() && pc_menu_take_input(), "V queues native work and counts as menu activity without opening the popup");
    CHECK(tick(page) == 1 && display_has("RESET TO DEFAULTS") && pc_menu_test_focus() == 0,
          "the V shortcut opens exactly one hub after the game-thread tick");
    key_press(0x1C, 0); tick(page);
    CHECK(display_has("WINDOW MODE"), "Enter passes the video filter and opens Display through the native filter");
    generation = pc_video_generation();
    pc_input_key_event(0x4D, 1, 1, 0); pc_input_key_event(0x4D, 1, 1, 1);
    pc_input_key_event(0x4D, 1, 1, 1); pc_input_key_event(0x4D, 1, 0, 0); tick(page);
    pc_video_get(&s);
    CHECK(s.borderless && pc_video_generation() == generation + 1, "Right and its repeats change the native setting once per press");
    pad(out, 0, 0, 0);
    CHECK(neutral(out), "the video navigation keys do not leak into the guest pad report");
    key_press(0x01, 0); tick(page);
    CHECK(display_has("RESET TO DEFAULTS") && pc_menu_test_focus() == 0, "Escape returns Display to its native hub");
    key_press(0x01, 0); tick(page);
    CHECK(!display_has("RESET TO DEFAULTS") && pc_menu_test_focus() == 2, "the next Escape restores native Options with its former focus");

    /* RB is delivered to slot zero, while the resulting page's A/B/D-pad go through slot one. */
    pc_video_menu_page_seen(); video_shoulder(out, 255);
    CHECK(neutral(out) && !pc_video_menu_is_open(), "RB is consumed and does not create a legacy popup");
    tick(page); video_shoulder(out, 255); tick(page);
    CHECK(display_has("RESET TO DEFAULTS") && pc_menu_test_focus() == 0, "held RB opens one native hub");
    video_shoulder(out, 0);
    pad(out, 2, 0, 0); tick(page); CHECK(neutral(out) && pc_menu_test_focus() == 1, "D-pad moves the hub to Graphics");
    pad(out, 0, 0, 0); pad(out, 0, 255, 0); tick(page);
    CHECK(neutral(out) && display_has("ANTI-ALIASING"), "A opens Graphics through the native filter");
    pad(out, 0, 0, 0); generation = pc_video_generation();
    pad(out, 8, 0, 0); tick(page); pad(out, 8, 0, 0); tick(page);
    CHECK(pc_video_generation() == generation + 1 && neutral(out), "held D-pad Right changes the video value only on its edge");
    pad(out, 0, 0, 0); pad(out, 0, 0, 255); tick(page);
    CHECK(neutral(out) && display_has("RESET TO DEFAULTS") && pc_menu_test_focus() == 1, "B returns Graphics to the hub with focus preserved");
    pad(out, 0, 0, 255); tick(page);
    CHECK(display_has("RESET TO DEFAULTS"), "held B does not close the hub too");
    pad(out, 0, 0, 0); pad(out, 0, 0, 255); tick(page);
    CHECK(neutral(out) && !display_has("RESET TO DEFAULTS") && pc_menu_test_focus() == 2, "a second B press returns to Options");
    pad(out, 0, 0, 0);

    /* The game's original Options may be intentionally omitted from an edited native definition. */
    {
        const char *only_video = strstr(k_video_def, "[page VideoSettings]");
        pc_menu_test_reset(only_video); tick("");
        CHECK(tick("PauseMenuOptionXbox") == 0, "an omitted Options page is the game's own page");
        pc_video_menu_page_seen(); key_press(0x2F, 0);
        CHECK(tick("PauseMenuOptionXbox") == 1 && display_has("RESET TO DEFAULTS"), "V can open the native hub over original Options");
        key_press(0x01, 0);
        CHECK(tick("PauseMenuOptionXbox") == 0 && !pc_menu_active(), "closing that hub returns zero so the host re-shows original Options");
        video_shoulder(out, 0); pc_video_menu_page_seen(); video_shoulder(out, 255);
        CHECK(tick("PauseMenuOptionXbox") == 1 && display_has("RESET TO DEFAULTS"), "RB can open the native hub over original Options");
        video_shoulder(out, 0); pad(out, 0, 0, 255);
        CHECK(tick("PauseMenuOptionXbox") == 0 && neutral(out), "B closes the hub over original Options without its press reaching the guest");
        pad(out, 0, 0, 255); CHECK(neutral(out), "B stays swallowed while held after original Options returns");
        pad(out, 0, 0, 0); pad(out, 0, 0, 255); CHECK(!neutral(out), "a fresh B press after closing belongs to original Options");
        pad(out, 0, 0, 0);
    }
    video_menu_fixture(); tick(page); pc_video_menu_page_seen(); key_press(0x2F, 0);
    CHECK(tick("FMVPlayer") == 0 && !pc_menu_active(), "a queued V request is ignored after Options leaves for an attract movie");
    tick(page); CHECK(!display_has("RESET TO DEFAULTS"), "a stale V request cannot reopen when Options returns");
    pc_menu_request_video();
    CHECK(pc_menu_tick(page, 0, &g_host) == 1 && !display_has("RESET TO DEFAULTS"), "a native request is dropped while Options is not ready");
    tick(page); CHECK(!display_has("RESET TO DEFAULTS"), "the dropped request cannot open on the next ready tick");
    pc_menu_request_video();
    CHECK(tick("FMVPlayer") == 0 && !pc_video_menu_is_open(), "a direct native request also revalidates the guest page");
    pc_input_focus_event(0);
    pc_input_set_filter_slot(0, NULL); pc_input_set_filter_slot(1, NULL);
    pc_video_menu_set_native_opener(NULL);
    _putenv_s("RECOMP_BLACK_INPUT_MODE", "keyboard_mouse");
    _putenv_s("RECOMP_BLACK_PROMPT_MODE", "keyboard_mouse");
    pc_input_test_reset();
    video_fresh();
}

/* The presenter redraws only what changed; that must give exactly the pixels of a whole redraw. */
static void test_incremental(void)
{
    const unsigned w = 3440, h = 1440;
    const int px = 40, py = 0;
    const unsigned pw = 3360, ph = 1440;
    uint32_t *full = (uint32_t *)malloc((size_t)w * h * 4), *inc = (uint32_t *)malloc((size_t)w * h * 4);
    const uint32_t *pixels = NULL;
    int changed = 0, step, same, i;
    unsigned box[4];
    LARGE_INTEGER f, t0, t1;
    double whole_ms = 0, part_ms = 0;
    QueryPerformanceFrequency(&f);
    reset_game();
    tick("PauseMenu");
    QueryPerformanceCounter(&t0);
    CHECK(pc_menu_overlay(w, h, px, py, pw, ph, &pixels, &changed) == 1 && changed && pixels, "the overlay is drawn");
    QueryPerformanceCounter(&t1);
    whole_ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    {
        /* the slide-in: a few frames of it, each redrawn in part, end on the same pixels as a whole redraw */
        int frames = 0;
        double slide_ms = 0;
        for (i = 0; i < 40; i++) {
            Sleep(10);
            tick("PauseMenu");
            QueryPerformanceCounter(&t0);
            pc_menu_overlay(w, h, px, py, pw, ph, &pixels, &changed);
            QueryPerformanceCounter(&t1);
            if (changed) { frames++; slide_ms += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart; }
        }
        printf("slide-in: %d frames redrawn, %.2f ms each on average\n", frames, frames ? slide_ms / frames : 0.0);
    }
    for (step = 0; step < 6; step++) {
        press(step < 3 ? PCM_ACT_DOWN : PCM_ACT_UP, "PauseMenu");
        Sleep(350);                               /* past the focus animation */
        tick("PauseMenu");
        QueryPerformanceCounter(&t0);
        pc_menu_overlay(w, h, px, py, pw, ph, &pixels, &changed);
        QueryPerformanceCounter(&t1);
        part_ms += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
        CHECK(changed && pc_menu_overlay_dirty(box) && box[3] < h / 2, "a focus move changes a band, not the screen (%u x %u)", box[2], box[3]);
        memcpy(inc, pixels, (size_t)w * h * 4);
        memset(full, 0, (size_t)w * h * 4);
        {
            /* a whole redraw of the same display list, into the same picture */
            uint32_t *pic = (uint32_t *)calloc((size_t)pw * ph, 4);
            unsigned y;
            pc_menu_test_render(pic, pw, ph);
            for (y = 0; y < ph; y++) memcpy(full + (size_t)(py + y) * w + px, pic + (size_t)y * pw, (size_t)pw * 4);
            free(pic);
        }
        same = 1;
        for (i = 0; i < (int)(w * h) && same; i++) if (full[i] != inc[i]) same = 0;
        CHECK(same, "step %d: the partly redrawn overlay equals a whole redraw (first difference at pixel %d)", step, same ? -1 : i - 1);
    }
    printf("overlay at %ux%u: whole %.1f ms, a focus move %.2f ms on average\n", w, h, whole_ms, part_ms / 6);
    free(full); free(inc);
}

static int cmd_render(int argc, char **argv)
{
    unsigned w = argc > 5 ? (unsigned)atoi(argv[5]) : 640, h = argc > 6 ? (unsigned)atoi(argv[6]) : 480;
    size_t size;
    uint8_t *def = read_all(argv[2], &size);
    uint32_t *px, *bg;
    int i, focus = argc > 8 ? atoi(argv[8]) : 1;
    if (!def) { printf("cannot read %s\n", argv[2]); return 2; }
    load_game_files();
    g_nvalues = 0; g_ntexts = 0;
    value_set("FEInvertLookFlag", 0); value_set("FEToggleCrouchFlag", 0); value_set("FEVibration", 1);
    value_set("FESFXVolume", 100); value_set("FEMusicVolume", 100); value_set("FE_LevelAllowContinue", 1);
    text_set("FE_CURRENTOBJECTIVE", "rendezvous with black cell");
    text_set("FE_CONFIRMTITLE", "restart");
    text_set("FE_CONFIRMMESSAGE", "All checkpoint progress will be lost!  are you sure you want to restart the mission?");
    text_set("FE_OBJECTIVESTARGETTOTAL", "1/8"); text_set("FE_OBJECTIVETOTAL", "0/8");
    text_set("FE_BLACKMAILRATING", "0/3"); text_set("FE_INTELRATING", "0/3"); text_set("FE_RECONRATING", "0/1");
    text_set("FE_ARMAMENTRATING", "0/1"); text_set("FE_SECONDARYRATING", "-");
    text_set("FE_OBJECTIVEHINT", "destroy laptops, safes and briefcases containing classified information.");
    value_set("FEDiffMode", 1);
    {   /* PCM_SET=NAME=V,NAME=V: other values for this render */
        const char *e = getenv("PCM_SET");
        char buf[256], *tok, *ctx = NULL;
        if (e) {
            snprintf(buf, sizeof(buf), "%s", e);
            for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
                char *eq = strchr(tok, '=');
                if (eq) { *eq = 0; value_set(tok, atoi(eq + 1)); }
            }
        }
    }
    pc_menu_test_reset((const char *)def);
    tick("");
    if (!_stricmp(argv[3], "ConfirmWarning")) { tick("PauseMenu"); press(PCM_ACT_DOWN, "PauseMenu"); press(PCM_ACT_DOWN, "PauseMenu"); press(PCM_ACT_CONFIRM, "PauseMenu"); }
    tick(argv[3]);
    for (i = 1; i < focus; i++) press(PCM_ACT_DOWN, argv[3]);
    Sleep(400);                               /* past the focus animation and the slide-in */
    tick(argv[3]);
    px = (uint32_t *)calloc((size_t)w * h, 4);
    pc_menu_test_render(px, w, h);
    if (argc > 7 && (bg = read_bmp(argv[7], w, h)) != NULL) { composite(bg, px, (size_t)w * h); write_bmp(argv[4], bg, w, h); free(bg); }
    else write_bmp(argv[4], px, w, h);
    free(px); free(def);
    return 0;
}

int main(int argc, char **argv)
{
    unsigned i, k;
    for (i = 0; i < 256; i++) { uint32_t c = i; for (k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1; g_crc[i] = c; }
    _putenv("RECOMP_BLACK_PROMPT_MODE=keyboard_mouse");
    _putenv("RECOMP_BLACK_INPUT_MODE=keyboard_mouse");
    if (getenv("PCM_PAD")) _putenv("RECOMP_BLACK_PROMPT_MODE=controller");   /* render: the pad's prompts */
    if (argc >= 3 && !strcmp(argv[1], "check")) {
        size_t size;
        char log[8192];
        uint8_t *d = read_all(argv[2], &size);
        int n;
        if (!d) { printf("cannot read %s\n", argv[2]); return 2; }
        n = pc_menu_check_definition((const char *)d, log, sizeof(log));
        printf("%d page(s)\n%s", n, log);
        free(d);
        return log[0] ? 1 : 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "render")) return cmd_render(argc, argv);
    g_have_files = load_game_files();
    if (!g_have_files) printf("(the game's files are not here: render and text checks skipped)\n");
    test_parser();
    test_native_video_pages();
    test_pause();
    test_confirm();
    test_mixed();
    test_options();
    test_mission_failed();
    test_exit_game();
    test_replaced_default();
    test_video_file_supplements();
    test_native_page_stack();
    test_video_setting_focus_and_layout();
    test_input();
    test_pointer();
    test_native_video_shortcuts();
    if (g_have_files) test_objectives();
    if (g_have_files) test_render();
    if (g_have_files) test_incremental();
    if (g_video_ini[0]) DeleteFileA(g_video_ini);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
