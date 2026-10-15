/* BEGIN BLACK_NATIVE_MENUS
 * BLACK's in-game menus -- the pause menu, its options, the restart / quit confirmation and mission
 * failed -- as native pages (xboxrecomp/src/video/pc_menu.c) described by local\menus.ini, which is
 * written from the definition below the first time and can then be edited (the game re-reads it
 * when a menu opens). Included only by recomp_manual.c.
 *
 * The game keeps running its own page underneath, hidden: the front-end manager [0x2D199C] still
 * loads it, prepares it (its Prepare/PagePrepare run, so the game fills in the objective text and
 * the option values exactly as before) and pauses the game for it. The native page replaces what
 * that page shows and how it reads input, and does what the page's buttons do through the same
 * two channels the original ActionScript uses:
 *   - menu functions ("gamecode"): the registry at ui+4 polled by sub_00112F30 once per update --
 *     name at +0x374 (32 bytes), three parameter strings at +0x394 + i*0x3C, trigger +0x370 = 1;
 *   - page changes: FETargetPage at ui+0x3820 (32 bytes), FEChangePage ui+0x3884 = 1.
 * Values the page shows come from the front end's own bound variables (table ui+0x2530, entries
 * {type, pointer, hash, refs, 0}, 0x70 of them; text handles in ui+0x2DF4) and the game's string
 * tables ([0x2D45D8]+0xCE8, eleven sorted tables). Keys hash with the game's CRC (table 0x2AC908,
 * arithmetic shift). Sounds are the page's own SNDxxx clips, started the way the original starts
 * them (gotoAndPlay(2) through the game's host-to-ActionScript call, sub_00101420), and the page is
 * hidden with the game's own property setter (sub_00101790, "<movie>._visible" = "0").
 *
 * Everything runs on the game thread, inside the front-end update (sub_00124330), before the
 * original body -- where the original ActionScript would have run for the same input.
 * RECOMP_NATIVE_MENUS=0 leaves every page to the game; a page missing from (or broken in) the file
 * is left to the game too. --exclude-manual keeps the generated update as sub_00124330_gen.
 *
 * The main menu is native too -- blacktitlemenu2 before any mission is done, blacktitlemenu after (sub_001F8B50
 * picks one by the profile's progress bytes 0x2D0DBE..0x2D10BE) -- with an EXIT GAME item the console game does not
 * have. It asks first (ExitGame: a page of the file opened over the menu, in the look of the front end's own
 * questions, memcardmessage) and then closes the game the way closing its window does. Both menus allow the attract
 * movie (FEAllowAttractMode, ui+0x3894): the front end starts it after FEAttractTimeout seconds (ui+0x3898, 40) in
 * which its idle check, sub_001F4ED0, saw no input. A native page takes all input away from the game, so that check
 * also counts the native page's input (wrapped at the end; generated body renamed sub_001F4ED0_gen in recomp_0011.c).
 */
#include "pc_menu.h"

extern RECOMP_TLS uint32_t g_ebp, g_seh_ebp;
extern void sub_00124330_gen(void);
extern void sub_00101790(void);
extern void sub_00101420(void);
extern void sub_001F4ED0_gen(void);
extern int xbox_FramebufferRequestClose(void);

static const char black_menu_definition[] =
"; BLACK -- native in-game menus.\r\n"
";\r\n"
"; This file is read when a menu opens: edit it while the game runs, then open the menu again.\r\n"
"; Delete it to get the original layout back (the game writes it again). A page that is not listed\r\n"
"; here, or has an error (see the log), is shown by the game itself. RECOMP_NATIVE_MENUS=0 turns\r\n"
"; the native pages off.\r\n"
";\r\n"
"; Pages have the game's own names. Positions are in the original 640 x 480 menu space (the menu\r\n"
"; is centred on the picture at any aspect ratio); a text's Y is the top of its capitals.\r\n"
";\r\n"
"; Text:     $KEY     the game's own words (language file), e.g. $FE_CONTINUE\r\n"
";           &KEY     text the game fills in (objective, warning), e.g. &FE_CURRENTOBJECTIVE\r\n"
";           \"...\"    your own words\r\n"
";           @name    a menu variable (see set)\r\n"
"; Items:    item   = TEXT | ACTIONS [| if VALUE] [| unless VALUE] [| focus ACTIONS] [| gap N]\r\n"
";           toggle = TEXT | VALUE | FUNCTION              ON / OFF, sends FUNCTION(1 or 0)\r\n"
";           slider = TEXT | VALUE | FUNCTION | MIN MAX STEP\r\n"
";           VALUE is a number the game publishes to its menus (FEInvertLookFlag ...).\r\n"
"; Actions:  gamecode NAME [ARG ...]   one of the game's menu functions, as the original page calls it\r\n"
";           page NAME                  go to another page\r\n"
";           set NAME VALUE             remember a value for another page (read it as @NAME)\r\n"
";           video                      open the Video Settings page\r\n"
";           back                       this page's back action\r\n"
";           open NAME                  show a page of this file that the game does not have, over this one\r\n"
";           close                      leave such a page (back to the one it was opened over)\r\n"
";           exit                       close the game, as closing its window does\r\n"
";           Several actions are separated by ';' (keep comments on their own lines).\r\n"
"; Page settings: title, label/text X Y TEXT [| wrap W], list X Y STEP, columns X1 X2, focus N,\r\n"
"; back ACTIONS, start back|confirm (the pad's START), prompts select confirm back, arrows 2|4,\r\n"
"; logo X Y, title_at X Y, prompts_at X Y STEP, rails X1 X2, selector WIDTH, loading VALUE,\r\n"
"; require NAME (only when an earlier page set it).\r\n"
"\r\n"
"[style]\r\n"
"; Colours are RRGGBB or AARRGGBB; they match the original pages as this port shows them.\r\n"
"shade = D9091010\r\n"
"column = 1E1F201E\r\n"
"rail = 5A6057\r\n"
"item = 36473D\r\n"
"focus = 7E7E7E\r\n"
"label = 36473D\r\n"
"text = 7F7F7F\r\n"
"title = 7F7F7F\r\n"
"prompt = 7F7F7F\r\n"
"selector = F0090B0A\r\n"
"line = 7F7F7F\r\n"
"choice = 36473D\r\n"
"choice_on = 7F7F7F\r\n"
"bullet = 36473D\r\n"
"bullet_on = 7F7F7F\r\n"
"focus_scale = 1.1\r\n"
"sounds = on\r\n"
"\r\n"
"[page PauseMenu]\r\n"
"title = $FE_PAUSEMENU\r\n"
"label = 284 141 $FE_CURRENTOBJECTIVES\r\n"
"text = 284 165 &FE_CURRENTOBJECTIVE | wrap 300\r\n"
"list = 285 241 25\r\n"
"focus = 1\r\n"
"start = back\r\n"
"back = gamecode ClosePauseMenu\r\n"
"loading = FELoadingPauseMenus\r\n"
"item = $FE_CONTINUE | gamecode ClosePauseMenu\r\n"
"item = $FE_OBJECTIVESSTATUS | page PauseMenuObjectives\r\n"
"item = $FE_RESTARTMISSION | set FEConfirmYesGameCode RestartLevel; set FEConfirmNoPage PauseMenu; page ConfirmWarning; gamecode SetupConfirmWarning RestartConfirm\r\n"
"item = $FE_OPTIONS | page PauseMenuOptionXbox\r\n"
"item = $FE_QUIT | set FEConfirmYesGameCode QuitLevel; set FEConfirmNoPage PauseMenu; page ConfirmWarning; gamecode SetupConfirmWarning QuitConfirm\r\n"
"\r\n"
"[page PauseMenuOptionXbox]\r\n"
"title = $FE_OPTIONS\r\n"
"logo = 185.5 58\r\n"
"title_at = 199 86\r\n"
"rails = 47 170\r\n"
"list = 218 147 27\r\n"
"columns = 431 519\r\n"
"selector = 349\r\n"
"arrows = 4\r\n"
"; one row more than the original page (VIDEO SETTINGS): the prompts sit a row lower\r\n"
"prompts_at = 218 399 28\r\n"
"focus = 1\r\n"
"back = page PauseMenu\r\n"
"toggle = $FE_TOGGLECROUCH | FEToggleCrouchFlag | ToggleCrouchOnOff\r\n"
"toggle = $FE_INVERTLOOK | FEInvertLookFlag | ToggleInvertLook\r\n"
"toggle = $FE_VIBRATION | FEVibration | ToggleVibration\r\n"
"slider = $FE_SFXVOLUME | FESFXVolume | SetSfxValue | 0 100 4 | gap 15\r\n"
"slider = $FE_MUSICVOLUME | FEMusicVolume | SetMusicValue | 0 100 4\r\n"
"item = \"VIDEO SETTINGS\" | video\r\n"
"\r\n"
"[page OptionsMenuXbox]\r\n"
"column = 00000000\r\n"
"title = $FE_OPTIONS\r\n"
"logo = 269.5 58.3\r\n"
"title_at = 283.2 84.1\r\n"
"rails = 94.5 255\r\n"
"list = 303.2 147.9 26\r\n"
"prompts_at = 306.2 368 28\r\n"
"focus = 1\r\n"
"back = gamecode StartAutosaveSettings\r\n"
"item = $FE_CONTROLLERSETUP | page ControllerSetup\r\n"
"item = $FE_SOUNDSETUP | page SoundSetupXbox\r\n"
"item = $FE_PICTURESETUP | page PictureSetupXbox\r\n"
"item = $FE_PLAYERPROFILE | page ProfileOptions\r\n"
"item = \"VIDEO SETTINGS\" | video\r\n"
"\r\n"
"; PC video pages use the same fonts, rails, focus and input as the game's menus.\r\n"
"; setting = TEXT | mode/window/internal/aa/af/dof/sharpen/ao_method/ao_quality/aspect/fov/scale/filter/vsync/fps/blur\r\n"
"; &VIDEO_HELP is the focused setting's explanation. reset_video preserves locked values.\r\n"
"[page VideoSettings]\r\n"
"column = 00000000\r\n"
"title = \"VIDEO SETTINGS\"\r\n"
"logo = 185.5 58\r\n"
"title_at = 199 86\r\n"
"rails = 47 170\r\n"
"list = 218 147 27\r\n"
"selector = 349\r\n"
"prompts_at = 218 399 28\r\n"
"back = close\r\n"
"item = \"DISPLAY\" | open VideoDisplay\r\n"
"item = \"GRAPHICS\" | open VideoGraphics\r\n"
"item = \"RESET TO DEFAULTS\" | reset_video\r\n"
"item = \"BACK\" | close\r\n"
"\r\n"
"[page VideoDisplay]\r\n"
"column = 00000000\r\n"
"title = \"DISPLAY\"\r\n"
"logo = 151.5 58\r\n"
"title_at = 165 86\r\n"
"rails = 47 130\r\n"
"list = 168 142 23\r\n"
"values = 607\r\n"
"item_size = 13.5\r\n"
"selector = 451\r\n"
"arrows = 4\r\n"
"text = 168 366 &VIDEO_HELP | wrap 439 | size 12\r\n"
"prompts_at = 168 405 22\r\n"
"back = close\r\n"
"setting = \"WINDOW MODE\" | mode\r\n"
"setting = \"WINDOW SIZE\" | window\r\n"
"setting = \"INTERNAL RESOLUTION\" | internal\r\n"
"setting = \"ASPECT RATIO\" | aspect\r\n"
"setting = \"SCALING\" | scale\r\n"
"setting = \"SCALING FILTER\" | filter\r\n"
"setting = \"VERTICAL SYNC\" | vsync\r\n"
"setting = \"FRAME LIMIT\" | fps\r\n"
"item = \"BACK\" | close\r\n"
"\r\n"
"[page VideoGraphics]\r\n"
"column = 00000000\r\n"
"title = \"GRAPHICS\"\r\n"
"logo = 151.5 58\r\n"
"title_at = 165 86\r\n"
"rails = 47 130\r\n"
"list = 168 142 23\r\n"
"values = 607\r\n"
"item_size = 13.5\r\n"
"selector = 451\r\n"
"arrows = 4\r\n"
"text = 168 366 &VIDEO_HELP | wrap 439 | size 12\r\n"
"prompts_at = 168 405 22\r\n"
"back = close\r\n"
"setting = \"ANTI-ALIASING\" | aa\r\n"
"setting = \"ANISOTROPIC FILTERING\" | af\r\n"
"setting = \"DEPTH OF FIELD\" | dof\r\n"
"setting = \"SHARPENING\" | sharpen\r\n"
"setting = \"AMBIENT OCCLUSION\" | ao_method\r\n"
"setting = \"AO QUALITY\" | ao_quality\r\n"
"setting = \"FIELD OF VIEW\" | fov\r\n"
"setting = \"MOTION BLUR\" | blur\r\n"
"item = \"BACK\" | close\r\n"
"\r\n"
"[page PauseMenuObjectives]\r\n"
"title = $FE_OBJECTIVESSTATUS\r\n"
"logo = 213.5 40\r\n"
"title_at = 227 67\r\n"
"rails = 71 195\r\n"
"text = 246 108 $FE_OBJECTIVESTARGET\r\n"
"text = 589 108 &FE_OBJECTIVESTARGETTOTAL | right\r\n"
"line = 245 129 592\r\n"
"line = 245 270 592\r\n"
"text = 246 286 $FE_CURRENTTOTAL\r\n"
"text = 589 286 &FE_OBJECTIVETOTAL | right\r\n"
"label = 246 310 $FE_COMPLETEALL\r\n"
"text = 227 353 &FE_OBJECTIVEHINT | wrap 340 | size 14\r\n"
"list = 247 144 26\r\n"
"values = 589\r\n"
"selector = 357\r\n"
"highlight = off\r\n"
"focus = 1\r\n"
"prompts = back\r\n"
"prompts_at = 250 431 28\r\n"
"back = page PauseMenu\r\n"
"; Moving onto a row shows its hint, as the original. The rows the difficulty leaves out are dimmed.\r\n"
"item = $FE_BLACKMAIL | none | value &FE_BLACKMAILRATING | focus gamecode ChangeObjectiveHint 2 | dim unless FEDiffMode\r\n"
"item = $FE_INTEL | none | value &FE_INTELRATING | focus gamecode ChangeObjectiveHint 3 | dim unless FEDiffMode\r\n"
"item = $FE_RECON | none | value &FE_RECONRATING | focus gamecode ChangeObjectiveHint 4 | dim unless FEDiffMode\r\n"
"item = $FE_ARMAMENT | none | value &FE_ARMAMENTRATING | focus gamecode ChangeObjectiveHint 5 | dim unless FEDiffMode\r\n"
"item = $FE_SECONDARY | none | value &FE_SECONDARYRATING | focus gamecode ChangeObjectiveHint 1 | dim unless FEDiffMode=2\r\n"
"\r\n"
"[page ConfirmWarning]\r\n"
"; Shown after a question was asked (FEConfirmYesGameCode and FEConfirmNoPage set, as the original pages do).\r\n"
"require = FEConfirmYesGameCode FEConfirmNoPage\r\n"
"title = &FE_CONFIRMTITLE\r\n"
"logo = 265 54\r\n"
"title_at = 283 83\r\n"
"rails = 93 251\r\n"
"text = 302 144 &FE_CONFIRMMESSAGE | wrap 297\r\n"
"list = 302 233.5 30\r\n"
"focus = 2\r\n"
"prompts = select confirm\r\n"
"prompts_at = 304 401 27\r\n"
"back = page @FEConfirmNoPage\r\n"
"item = $FE_YES | gamecode @FEConfirmYesGameCode\r\n"
"item = $FE_NO | page @FEConfirmNoPage\r\n"
"\r\n"
"[page MissionFailed]\r\n"
"title = $FE_MISSIONFAILED\r\n"
"logo = none\r\n"
"title_at = 313 86\r\n"
"rails = 93 281\r\n"
"list = 333 147 25\r\n"
"prompts = select confirm\r\n"
"prompts_at = 345 397 28\r\n"
"focus = 1\r\n"
"; Without a checkpoint to go back to, Continue is greyed and skipped, as the original.\r\n"
"item = $FE_CONTINUEMISSION | gamecode ContinueMission | disabled unless FE_LevelAllowContinue\r\n"
"item = $FE_RESTARTMISSION | set FEConfirmYesGameCode RestartMission; set FEConfirmNoPage MissionFailed; page ConfirmWarning; gamecode SetupConfirmWarning RestartConfirm\r\n"
"item = $FE_QUIT | set FEConfirmYesGameCode QuitFromMissionFailed; set FEConfirmNoPage MissionFailed; page ConfirmWarning; gamecode SetupConfirmWarning QuitConfirm\r\n"
"\r\n"
"; The main menu, before any mission is done. EXIT GAME is not on the console page: it asks first (ExitGame below).\r\n"
"[page blacktitlemenu2]\r\n"
"column = 00000000\r\n"
"logo = 299.5 54\r\n"
"rails = 105 268\r\n"
"list = 319.5 150.5 25\r\n"
"prompts = select confirm\r\n"
"prompts_at = 317 398 28\r\n"
"focus = 1\r\n"
"item = $FE_STARTMISSION | page Difficulty\r\n"
"item = $FE_OPTIONS | page OptionsMenuXbox\r\n"
"item = \"EXIT GAME\" | open ExitGame\r\n"
"\r\n"
"; The main menu once a mission is done.\r\n"
"[page blacktitlemenu]\r\n"
"column = 00000000\r\n"
"logo = 269 54.5\r\n"
"rails = 63.5 231\r\n"
"list = 285.5 150.5 25\r\n"
"prompts = select confirm\r\n"
"prompts_at = 286 399 28\r\n"
"focus = 1\r\n"
"item = $FE_CONTINUEMISSION | page MissionModeSelect\r\n"
"item = $FE_OPTIONS | page OptionsMenuXbox\r\n"
"item = $FE_OPERATORPROFILE | page OperatorProfile\r\n"
"item = \"EXIT GAME\" | open ExitGame\r\n"
"\r\n"
"; Not a page of the game: EXIT GAME opens it over the main menu, in the look of the front end's own questions.\r\n"
"; The question is the game's own sentence.\r\n"
"[page ExitGame]\r\n"
"column = 00000000\r\n"
"logo = 282.5 54\r\n"
"rails = 94 247\r\n"
"text = 301 188 \"ARE YOU SURE YOU WANT TO QUIT?\"\r\n"
"list = 300.5 295 25\r\n"
"focus = 2\r\n"
"prompts = select confirm\r\n"
"prompts_at = 298 398 28\r\n"
"back = close\r\n"
"item = $FE_YES | exit\r\n"
"item = $FE_NO | close\r\n"
"\r\n"
"; Choosing the difficulty (the game's difficulty page, opened by START MISSION and before a mission is replayed).\r\n"
"; Levels 0 easy, 1 normal, 2 hard, 3 black. FE_MissionDifficulty counts the levels open to the player (easy always);\r\n"
"; the page starts on FE_DifficultySelection, or the highest open level below it. Moving onto a level shows its\r\n"
"; description (UpdateDificulty, the game's own spelling); choosing it starts the mission (SelectedDifficulty).\r\n"
"[page Difficulty]\r\n"
"column = 00000000\r\n"
"logo = 188 39\r\n"
"rails = 60 163\r\n"
"title = $FE_DIFFICULTYSELECT\r\n"
"title_at = 203 67\r\n"
"list = 253 128 28\r\n"
"text = 203 252 &FE_DIFFICULTYDESC | wrap 380 | size 12\r\n"
"prompts = select confirm back\r\n"
"prompts_at = 224 372 28\r\n"
"focus_from = FE_DifficultySelection\r\n"
"back = gamecode DifficultyBack\r\n"
"item = $FE_EASY | gamecode SelectedDifficulty 0 | focus gamecode UpdateDificulty 0\r\n"
"item = $FE_NORMAL | gamecode SelectedDifficulty 1 | focus gamecode UpdateDificulty 1 | disabled unless FE_MissionDifficulty>1\r\n"
"item = $FE_HARD | gamecode SelectedDifficulty 2 | focus gamecode UpdateDificulty 2 | disabled unless FE_MissionDifficulty>2\r\n"
"item = $FE_BLACKOPS | gamecode SelectedDifficulty 3 | focus gamecode UpdateDificulty 3 | disabled unless FE_MissionDifficulty>3\r\n";

/* The built-in definitions of earlier builds (FNV-1a 64 of the whole file): a local\menus.ini that is still exactly
 * one of them was never edited and is brought up to date. 1: the in-game pages only (2026-10-05); 4: before
 * the Difficulty page (2026-10-15); 5: its two-level first version (2026-10-15). */
static const uint64_t black_menu_replaced[] = { 0xD8419D900AB2FEA6ull, 0xBBE0D874943DB641ull, 0x9F7652C358180DC6ull,
    0xFF74A7E07CF4C7F0ull, 0xC9F0402F44A39FEBull };

/* ---- guest helpers --------------------------------------------------------------------------------------- */

static uint32_t bnm_scratch;          /* 1 KiB of guest heap for strings handed to the game's own functions */

static uint32_t bnm_hash(const char *s)
{
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; s++) {
        const uint32_t i = ((c & 0xFFu) ^ (uint32_t)(int32_t)(signed char)*s) & 0xFFFFFFFFu;
        if (i > 255u) return 0;                         /* the game's keys are ASCII */
        c = ((c >> 8) | ((c & 0x80000000u) ? 0xFF000000u : 0u)) ^ GUEST32(BLACK_VA(0x002AC908u) + i * 4u);
    }
    return c;
}

/* Calls a generated guest function the way guest code would: cdecl arguments on the guest stack below
 * the caller's, eax set, every register and the floating-point control restored afterwards. */
static int bnm_guest_call(void (*fn)(void), uint32_t eax, const uint32_t *args, unsigned n)
{
    const uint32_t s_eax = g_eax, s_ecx = g_ecx, s_edx = g_edx, s_ebx = g_ebx, s_esi = g_esi, s_edi = g_edi;
    const uint32_t s_esp = g_esp, s_ebp = g_ebp, s_seh = g_seh_ebp;
    const unsigned csr = _mm_getcsr();
    uint32_t sp = (s_esp - 0x200u) & ~15u, result;
    unsigned i;
    if (s_esp < 0x20000u || !black_pc_ram_span(sp - 4u * (n + 1u), 4u * (n + 1u) + 0x200u)) return 0;
    for (i = n; i-- > 0;) { sp -= 4u; GUEST32(sp) = args[i]; }
    sp -= 4u; GUEST32(sp) = BLACK_VA(0x00124330u);              /* returns into the front-end update, like its own calls */
    g_esp = sp; g_eax = eax;
    fn();
    result = g_eax;
    g_eax = s_eax; g_ecx = s_ecx; g_edx = s_edx; g_ebx = s_ebx; g_esi = s_esi; g_edi = s_edi;
    g_esp = s_esp; g_ebp = s_ebp; g_seh_ebp = s_seh;
    _mm_setcsr(csr);
    return (int)(result & 0xFFu) | 0x100;
}

static int bnm_put_string(uint32_t at, uint32_t cap, const char *s)
{
    size_t n = strlen(s);
    if (n + 1 > cap || !black_pc_ram_span(at, cap)) return 0;
    memcpy((void *)((uintptr_t)g_xbox_mem_offset + at), s, n + 1);
    return 1;
}

static void bnm_get_string(uint32_t at, char *out, size_t cap)
{
    size_t i;
    out[0] = 0;
    if (!black_pc_ram_span(at, 1u)) return;
    for (i = 0; i + 1 < cap && black_pc_ram_span(at + (uint32_t)i, 1u); i++) {
        const char c = *(const char *)((uintptr_t)g_xbox_mem_offset + at + (uint32_t)i);
        if (!c) break;
        out[i] = c;
    }
    out[i] = 0;
}

/* A bound front-end variable: returns its pointer and type, or 0. */
static uint32_t bnm_bound(uint32_t ui, uint32_t table_offset, uint32_t hash, uint32_t *type)
{
    const uint32_t table = ui + table_offset;
    uint32_t i;
    if (!hash || !black_pc_ram_span(table, 4u + 0x70u * 0x14u)) return 0;
    for (i = 0; i < 0x70u; i++) {
        const uint32_t e = table + 4u + i * 0x14u;
        if (GUEST32(e + 8u) == hash) { *type = GUEST32(e); return GUEST32(e + 4u); }
    }
    return 0;
}

/* Search the same manager instance passed to sub_001F3A80. */
static uint32_t bnm_string_from_manager(uint32_t tm, uint32_t hash)
{
    unsigned t;
    if (!tm || !black_pc_ram_span(BLACK_VA(0x002F3B0Eu), 1u) || !*(const uint8_t *)((uintptr_t)g_xbox_mem_offset + BLACK_VA(0x002F3B0Eu)) ||
        !black_pc_ram_span(tm + 0xCE8u, 44u)) return 0;
    for (t = 0; t < 11u; t++) {
        const uint32_t table = GUEST32(tm + 0xCE8u + t * 4u);
        int32_t lo = 0, hi;
        uint32_t entries;
        if (!table || !black_pc_ram_span(table, 0x10u)) continue;
        hi = (int32_t)GUEST32(table + 8u) - 1;
        entries = GUEST32(table + 0xCu);
        if (hi < 0 || hi > 100000 || !black_pc_ram_span(entries, (uint32_t)(hi + 1) * 4u)) continue;
        while (lo <= hi) {
            const int32_t mid = lo + (hi - lo) / 2;
            const uint32_t e = GUEST32(entries + (uint32_t)mid * 4u);
            int32_t h;
            if (!black_pc_ram_span(e, 8u)) break;
            h = (int32_t)GUEST32(e);
            if ((int32_t)hash < h) hi = mid - 1;
            else if ((int32_t)hash > h) lo = mid + 1;
            else return e + 4u;
        }
    }
    return 0;
}

/* Native menu helpers without an explicit manager use BLACK's global instance. */
static uint32_t bnm_string(uint32_t hash)
{
    const uint32_t tm = black_pc_ram_span(BLACK_VA(0x002D45D8u), 4u) ? GUEST32(BLACK_VA(0x002D45D8u)) : 0u;
    return bnm_string_from_manager(tm, hash);
}

static int bnm_utf8_to_utf16(uint32_t at, uint16_t *out, unsigned cap)
{
    unsigned n = 0;
    uint32_t p = at;
    while (n + 1 < cap && black_pc_ram_span(p, 1u)) {
        const uint8_t *u = (const uint8_t *)((uintptr_t)g_xbox_mem_offset + p);
        unsigned c = u[0];
        if (!c) break;
        if (c >= 0xC0 && c < 0xE0 && black_pc_ram_span(p + 1u, 1u) && (u[1] & 0xC0) == 0x80) { c = ((c & 0x1Fu) << 6) | (u[1] & 0x3Fu); p += 2; }
        else if (c >= 0xE0 && c < 0xF0 && black_pc_ram_span(p + 2u, 1u) && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
            c = ((c & 0x0Fu) << 12) | ((u[1] & 0x3Fu) << 6) | (u[2] & 0x3Fu); p += 3;
        } else p += 1;
        out[n++] = (uint16_t)c;
    }
    out[n] = 0;
    return (int)n;
}

/* The same for a string of this program (the localized keyboard prompts). */
static int bnm_host_utf8_to_utf16(const char *text, uint16_t *out, unsigned cap)
{
    const uint8_t *u = (const uint8_t *)text;
    unsigned n = 0;
    while (n + 1 < cap && *u) {
        unsigned c = u[0];
        if (c >= 0xC0 && c < 0xE0 && (u[1] & 0xC0) == 0x80) { c = ((c & 0x1Fu) << 6) | (u[1] & 0x3Fu); u += 2; }
        else if (c >= 0xE0 && c < 0xF0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
            c = ((c & 0x0Fu) << 12) | ((u[1] & 0x3Fu) << 6) | (u[2] & 0x3Fu); u += 3;
        } else u += 1;
        out[n++] = (uint16_t)c;
    }
    out[n] = 0;
    return (int)n;
}

/* ---- the game's services ---------------------------------------------------------------------------------- */

typedef struct BlackNmCtx { uint32_t ui; } BlackNmCtx;

static int bnm_gamecode(void *ctx, const char *name, const char *const args[3], int force)
{
    const uint32_t reg = ((BlackNmCtx *)ctx)->ui + 4u;
    unsigned i;
    if (!black_pc_ram_span(reg, 0x394u + 3u * 0x3Cu)) return 0;
    /* The original refuses a second request while one is waiting (TriggerGamecode, actionSelectGameCode);
     * a back action and a value change do not check, and neither do we then. */
    if (!force && GUEST32(reg + 0x370u) == 1u) return 0;
    if (!bnm_put_string(reg + 0x374u, 0x20u, name)) return 0;
    for (i = 0; i < 3u; i++) bnm_put_string(reg + 0x394u + i * 0x3Cu, 0x3Cu, args[i] && args[i][0] ? args[i] : "0");
    GUEST32(reg + 0x370u) = 1u;
    fprintf(stderr, "[MENU] gamecode %s(%s, %s, %s)\n", name, args[0] ? args[0] : "0", args[1] ? args[1] : "0", args[2] ? args[2] : "0");
    return 1;
}

static void bnm_page(void *ctx, const char *name)
{
    const uint32_t ui = ((BlackNmCtx *)ctx)->ui;
    if (!bnm_put_string(ui + 0x3820u, 0x20u, name)) return;
    GUEST32(ui + 0x3884u) = 1u;
    fprintf(stderr, "[MENU] page -> %s\n", name);
}

static int bnm_value(void *ctx, const char *name, int *out)
{
    const uint32_t ui = ((BlackNmCtx *)ctx)->ui;
    uint32_t type = 0, p = bnm_bound(ui, 0x2530u, bnm_hash(name), &type);
    if (!p || !black_pc_ram_span(p, 4u)) return 0;
    if (type == 5u) {
        char s[32];
        bnm_get_string(p, s, sizeof(s));
        *out = atoi(s);
    } else *out = (int)GUEST32(p);
    return 1;
}

static int bnm_text(void *ctx, const char *spec, uint16_t *out, unsigned cap)
{
    const uint32_t ui = ((BlackNmCtx *)ctx)->ui;
    const uint32_t hash = bnm_hash(spec + 1);
    out[0] = 0;
    if (spec[0] == '$') {
        size_t i;
        uint32_t s;
        /* With keyboard prompts the pad's menu legends name the keys, as everywhere else in the game. */
        if (pc_input_prompt_device() == PC_DEVICE_KEYBOARD_MOUSE)
            for (i = 0; i < sizeof(black_menu_prompts) / sizeof(black_menu_prompts[0]); i++) {
                char label[16], text[96];
                const char *language = getenv("RECOMP_LANGUAGE");
                if (black_menu_prompts[i].hash != hash) continue;
                if (!pc_input_menu_text((PcMenuKey)black_menu_prompts[i].key, label, sizeof(label))) break;
                snprintf(text, sizeof(text), "%s%s%s", black_menu_prompt_localize(black_menu_prompts[i].before, language),
                         label, black_menu_prompt_localize(black_menu_prompts[i].after, language));
                return bnm_host_utf8_to_utf16(text, out, cap);
            }
        s = bnm_string(hash);
        return s ? bnm_utf8_to_utf16(black_pc_brand_guest(s), out, cap) : -1;
    }
    if (spec[0] == '&') {
        uint32_t type = 0, p = bnm_bound(ui, 0x2DF4u, hash, &type);
        unsigned n = 0;
        if (!p) p = bnm_bound(ui, 0x2530u, hash, &type);
        if (!p || type != 4u) return -1;
        while (n + 1 < cap && black_pc_ram_span(p + n * 2u, 2u)) {
            const uint16_t c = *(const uint16_t *)((uintptr_t)g_xbox_mem_offset + p + n * 2u);
            if (!c) break;
            out[n++] = c;
        }
        out[n] = 0;
        {
            const size_t branded = black_pc_brand_text16(out, cap);
            if (branded != BLACK_PC_BRAND_ERROR) n = (unsigned)branded;
        }
        return (int)n;
    }
    return -1;
}

/* Lab trace (RECOMP_MENU_TRACE=1): reads an ActionScript property of the game's page back, through the
 * game's own GetVariable (sub_00101720: eax = "movie.path", one stack argument = the result buffer). */
extern void sub_00101720(void);
static int bnm_trace = -1;

static void bnm_trace_property(uint32_t ui, const char *what, const char *property)
{
    char movie[48], path[128], value[64];
    uint32_t args[1];
    if (bnm_trace < 0) { const char *e = getenv("RECOMP_MENU_TRACE"); bnm_trace = e && *e && *e != '0'; }
    if (!bnm_trace || !bnm_scratch) return;
    bnm_get_string(ui + 0x38E7u, movie, sizeof(movie));
    snprintf(path, sizeof(path), "%s.%s", movie, property);
    if (!bnm_put_string(bnm_scratch + 512u, 128u, path)) return;
    memset((void *)((uintptr_t)g_xbox_mem_offset + bnm_scratch + 640u), 0, 64u);
    args[0] = bnm_scratch + 640u;
    bnm_guest_call(sub_00101720, bnm_scratch + 512u, args, 1u);
    bnm_get_string(bnm_scratch + 640u, value, sizeof(value));
    fprintf(stderr, "[MENU] trace %s: %s = '%s'\n", what, path, value);
}

static char bnm_sound_pending[32];   /* traced on the next update, when the clip has moved on */

static void bnm_sound(void *ctx, const char *clip)
{
    const uint32_t ui = ((BlackNmCtx *)ctx)->ui;
    char movie[48], path[96];
    uint32_t args[5];
    if (!bnm_scratch) return;
    bnm_get_string(ui + 0x38E7u, movie, sizeof(movie));
    if (!movie[0]) return;
    /* The original says gotoAndPlay(2); the host call passes its arguments as strings, and "2" would be a frame
     * label. Each clip stops itself on frame 1 and starts its sound on frame 2, so play() is the same thing. */
    snprintf(path, sizeof(path), "%s.%s", movie, clip);
    if (!bnm_put_string(bnm_scratch, 128u, path) || !bnm_put_string(bnm_scratch + 128u, 32u, "play")) return;
    GUEST32(bnm_scratch + 192u) = 0u;
    args[0] = bnm_scratch + 128u;      /* function */
    args[1] = bnm_scratch + 192u;      /* result buffer (64 bytes) */
    args[2] = 0u;                      /* argument count */
    bnm_guest_call(sub_00101420, bnm_scratch, args, 3u);
    snprintf(bnm_sound_pending, sizeof(bnm_sound_pending), "%s", clip);
}

/* The original pages keep the question's answers as ActionScript globals (_global.FEConfirmYesGameCode,
 * _global.FEConfirmNoPage): written and read with the game's own SetVariable / GetVariable. */
static void bnm_set_var(void *ctx, const char *name, const char *value)
{
    char path[96];
    uint32_t args[2];
    (void)ctx;
    if (!bnm_scratch) return;
    snprintf(path, sizeof(path), "_global.%s", name);
    if (!bnm_put_string(bnm_scratch + 704u, 128u, path) || !bnm_put_string(bnm_scratch + 832u, 64u, value)) return;
    args[0] = bnm_scratch + 704u;
    args[1] = bnm_scratch + 832u;
    bnm_guest_call(sub_00101790, 0u, args, 2u);
}

static int bnm_get_var(void *ctx, const char *name, char *out, unsigned cap)
{
    char path[96];
    uint32_t args[1];
    (void)ctx;
    out[0] = 0;
    if (!bnm_scratch) return 0;
    snprintf(path, sizeof(path), "_global.%s", name);
    if (!bnm_put_string(bnm_scratch + 704u, 128u, path)) return 0;
    memset((void *)((uintptr_t)g_xbox_mem_offset + bnm_scratch + 896u), 0, 64u);
    args[0] = bnm_scratch + 896u;
    bnm_guest_call(sub_00101720, bnm_scratch + 704u, args, 1u);
    bnm_get_string(bnm_scratch + 896u, out, cap < 64u ? cap : 64u);
    /* an undefined ActionScript value reads as "" (or "undefined" in some paths): no value */
    if (!strcmp(out, "undefined")) out[0] = 0;
    return out[0] != 0;
}

static void bnm_video(void *ctx)
{
    (void)ctx;
    pc_menu_request_video();
}

/* EXIT GAME: the presenter closes its window as its close button does, and the process ends with 0. */
static void bnm_quit(void *ctx)
{
    (void)ctx;
    fprintf(stderr, "[MENU] exit: closing the game\n");
    fflush(stderr);
    if (!xbox_FramebufferRequestClose()) ExitProcess(0);
}

/* Hides (or shows again) the game's own page: the property setter the game uses for the same purpose. */
static void bnm_page_visible(uint32_t ui, int visible)
{
    char movie[48], path[96];
    uint32_t args[2];
    if (!bnm_scratch) return;
    bnm_get_string(ui + 0x38E7u, movie, sizeof(movie));
    if (!movie[0]) return;
    snprintf(path, sizeof(path), "%s._visible", movie);
    if (!bnm_put_string(bnm_scratch + 256u, 128u, path) || !bnm_put_string(bnm_scratch + 384u, 8u, visible ? "1" : "0")) return;
    args[0] = bnm_scratch + 256u;
    args[1] = bnm_scratch + 384u;
    bnm_guest_call(sub_00101790, 0u, args, 2u);
    bnm_trace_property(ui, visible ? "show" : "hide", "_visible");
}

/* ---- the user's own assets --------------------------------------------------------------------------------- */

static uint8_t *bnm_read_game_file(const char *xbox_path, size_t *size)
{
    WCHAR host[MAX_PATH];
    HANDLE f;
    LARGE_INTEGER sz;
    uint8_t *buf;
    DWORD got = 0;
    if (!xbox_translate_path(xbox_path, host, MAX_PATH)) return NULL;
    f = CreateFileW(host, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 16 * 1024 * 1024) { CloseHandle(f); return NULL; }
    buf = (uint8_t *)malloc((size_t)sz.QuadPart);
    if (buf && (!ReadFile(f, buf, (DWORD)sz.QuadPart, &got, NULL) || got != (DWORD)sz.QuadPart)) { free(buf); buf = NULL; }
    CloseHandle(f);
    if (buf) *size = (size_t)sz.QuadPart;
    return buf;
}

static int bnm_load_font(int slot, const char *xbox_path)
{
    size_t size = 0;
    uint8_t *d;
    int ok;
    if (pc_menu_has_font(slot)) return 1;
    d = bnm_read_game_file(xbox_path, &size);
    ok = d && pc_menu_load_font_file(slot, d, size);
    fprintf(stderr, "[MENU] font %s: %s\n", xbox_path, ok ? "loaded" : "NOT loaded");
    free(d);
    return ok;
}

/* Core.bin's "core" movie: image 4 is BLACK_Logo.tif, image 3 Bullet_White.tif (the volume bars). */
static void bnm_load_core_images(void)
{
    static const uint32_t ids[2] = { 4u, 3u };
    static const int slots[2] = { PCM_IMAGE_LOGO, PCM_IMAGE_BULLET };
    size_t size = 0;
    uint8_t *d = bnm_read_game_file("D:\\export\\FrontEnd\\Core.bin", &size);
    const int n = d ? pc_menu_load_bundle_images(d, size, "core", ids, slots, 2) : 0;
    fprintf(stderr, "[MENU] %d image(s) from Core.bin\n", n);
    free(d);
}

/* ---- the update hook --------------------------------------------------------------------------------------- */

static int bnm_state;                 /* 0 not started, 1 ready, -1 off */
static char bnm_hidden_page[48];
static int bnm_hidden_frames;

static void black_native_menu_tick(uint32_t ui)
{
    BlackNmCtx ctx;
    PcMenuHost host;
    char page[40];
    int ready, active;
    const unsigned caller_csr = _mm_getcsr();
    if (!ui || ui != GUEST32(BLACK_VA(0x002D199Cu)) || !black_pc_ram_span(ui, 0x3960u)) return;
    {
        /* Lab only: RECOMP_MENU_GOTO=Page[@FromPage] asks the game for that page once the pause menu (or FromPage) has
         * been up for two seconds, with the native pages on or off -- to see pages a short scripted run cannot reach. */
        static int goto_left = -1, goto_ticks;
        static char goto_page[40], goto_from[40] = "PauseMenu";
        if (goto_left < 0) {
            const char *e = getenv("RECOMP_MENU_GOTO"), *at;
            goto_left = e && *e ? 1 : 0;
            if (goto_left) {
                snprintf(goto_page, sizeof(goto_page), "%s", e);
                at = strchr(e, '@');
                if (at) { goto_page[at - e] = 0; snprintf(goto_from, sizeof(goto_from), "%s", at + 1); }
            }
        }
        if (goto_left) {
            char cur[40];
            bnm_get_string(ui + 0x3840u, cur, sizeof(cur));
            if (!_stricmp(cur, goto_from) && GUEST32(ui + 0x381Cu) == 2u && ++goto_ticks > 120 &&
                bnm_put_string(ui + 0x3820u, 0x20u, goto_page)) {
                GUEST32(ui + 0x3884u) = 1u;
                goto_left = 0;
                fprintf(stderr, "[MENU] RECOMP_MENU_GOTO: page -> %s\n", goto_page);
            }
        }
    }
    {
        /* Lab only: RECOMP_MENU_PROBE=path,path logs those ActionScript values whenever they change (native pages on
         * or off), e.g. _global.FEConfirmYesGameCode while the original pages ask a question. */
        static int probe = -1;
        static char paths[4][64], last[4][64];
        static uint32_t probe_buf;
        if (probe < 0) {
            const char *e = getenv("RECOMP_MENU_PROBE");
            probe = 0;
            if (e && *e && (probe_buf = xbox_HeapAlloc(256u, 16u)) != 0) {
                char tmp[300], *tok, *ctx = NULL;
                snprintf(tmp, sizeof(tmp), "%s", e);
                for (tok = strtok_s(tmp, ",", &ctx); tok && probe < 4; tok = strtok_s(NULL, ",", &ctx))
                    snprintf(paths[probe++], 64, "%s", tok);
            }
        }
        if (probe > 0 && GUEST32(ui + 0x381Cu) == 2u) {
            int k;
            for (k = 0; k < probe; k++) {
                uint32_t args[1];
                char value[64];
                if (!bnm_put_string(probe_buf, 64u, paths[k])) continue;
                memset((void *)((uintptr_t)g_xbox_mem_offset + probe_buf + 64u), 0, 64u);
                args[0] = probe_buf + 64u;
                bnm_guest_call(sub_00101720, probe_buf, args, 1u);
                bnm_get_string(probe_buf + 64u, value, sizeof(value));
                if (strcmp(value, last[k])) {
                    char cur[40];
                    bnm_get_string(ui + 0x3840u, cur, sizeof(cur));
                    fprintf(stderr, "[MENU] probe %s = '%s' (page %s)\n", paths[k], value, cur);
                    snprintf(last[k], 64, "%s", value);
                }
            }
        }
    }
    if (bnm_state < 0) return;
    if (!bnm_state) {
        const char *e = getenv("RECOMP_NATIVE_MENUS");
        if (e && *e == '0') { bnm_state = -1; pc_video_menu_set_native_opener(NULL); fprintf(stderr, "[MENU] RECOMP_NATIVE_MENUS=0: the game's own menus\n"); return; }
        bnm_state = 1;
        bnm_scratch = xbox_HeapAlloc(1024u, 16u);
        if (!bnm_load_font(PCM_FONT_SMALL, "D:\\language\\fonts\\Small.bin") | !bnm_load_font(PCM_FONT_BIG, "D:\\language\\fonts\\Big.bin") ||
            !bnm_scratch) {
            bnm_state = -1;
            pc_video_menu_set_native_opener(NULL);
            fprintf(stderr, "[MENU] the game's fonts could not be read: the game's own menus\n");
            _mm_setcsr(caller_csr);
            return;
        }
        bnm_load_core_images();
        pc_menu_set_default_definition(black_menu_definition);
        pc_menu_set_replaced_defaults(black_menu_replaced, (int)(sizeof(black_menu_replaced) / sizeof(black_menu_replaced[0])));
    }
    bnm_get_string(ui + 0x3840u, page, sizeof(page));
    /* prepared (state 2), not leaving (FERelease 0) and no page change waiting */
    ready = GUEST32(ui + 0x381Cu) == 2u && GUEST32(ui + 0x388Cu) == 0u && GUEST32(ui + 0x3884u) == 0u;
    ctx.ui = ui;
    host.ctx = &ctx; host.gamecode = bnm_gamecode; host.page = bnm_page; host.value = bnm_value; host.text = bnm_text;
    host.sound = bnm_sound; host.video = bnm_video; host.set_var = bnm_set_var; host.get_var = bnm_get_var;
    host.quit = bnm_quit;
    if (bnm_sound_pending[0]) {
        char prop[64];
        snprintf(prop, sizeof(prop), "%s._currentframe", bnm_sound_pending);
        bnm_sound_pending[0] = 0;
        bnm_trace_property(ui, "sound", prop);
    }
    active = pc_menu_tick(page, ready, &host);
    if (active) {
        /* Hidden once the page is loaded, and again now and then: a page the game reloads comes back visible. */
        if (_stricmp(bnm_hidden_page, page) || ++bnm_hidden_frames >= 30) {
            bnm_page_visible(ui, 0);
            snprintf(bnm_hidden_page, sizeof(bnm_hidden_page), "%s", page);
            bnm_hidden_frames = 0;
        }
        if (!_stricmp(page, "PauseMenuOptionXbox") || !_stricmp(page, "OptionsMenuXbox")) pc_video_menu_page_seen();
    } else if (bnm_hidden_page[0]) {
        if (!_stricmp(bnm_hidden_page, page)) bnm_page_visible(ui, 1);   /* the same page, now the game's again */
        bnm_hidden_page[0] = 0;
    }
    _mm_setcsr(caller_csr);
}

void sub_00124330(void)
{
    const uint32_t ui = black_pc_ram_span(g_esp, 8u) ? GUEST32(g_esp + 4u) : 0u;
    black_native_menu_tick(ui);
    sub_00124330_gen();
}

/* The front end's idle check (al = 1: there was input this frame), called once a frame by sub_0020ECF0 while the page
 * allows the attract movie. The native page's input is the player's input too. */
void sub_001F4ED0(void)
{
    sub_001F4ED0_gen();
    if (pc_menu_take_input()) g_eax = (g_eax & 0xFFFFFF00u) | 1u;
}
/* END BLACK_NATIVE_MENUS */
