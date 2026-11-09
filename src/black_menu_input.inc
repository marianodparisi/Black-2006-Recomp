/* Native APT frontend prompts. Included only by recomp_manual.c.
 * Core's CONFIRM/BACK text fields use $FE_CONFIRM/$FE_BACK; their Button
 * children are exported Control_S/Control_E movieclips, not font glyphs.
 * sub_00102D90 rebuilds native text through the game's destructor/constructor
 * callbacks and measures it again. Native text changes use flags (~1)|2:
 * bit 2 also prevents sub_00118A20 disposing of the old object a second time.
 */
static int black_menu_guest_name(uint32_t address, const char *name)
{
    const size_t bytes = strlen(name) + 1u;
    return bytes <= 64u && black_pc_ram_span(address, (uint32_t)bytes) &&
        memcmp((const void *)((uintptr_t)g_xbox_mem_offset + address), name, bytes) == 0;
}

static int black_menu_prompt_source(uint32_t source)
{
    uint32_t hash = 0xFFFFFFFFu, i;
    size_t prompt;
    const uint8_t *text;
    if (!black_pc_ram_span(source, 9u) || !black_pc_ram_span(BLACK_VA(0x002AC908u), 1024u)) return 0;
    text = (const uint8_t *)((uintptr_t)g_xbox_mem_offset + source + 8u);
    if (text[0] != '$' && text[0] != '#') return 0;
    /* sub_001F3A80's key CRC uses arithmetic SAR, not a standard CRC32. */
    for (i = 1u; i < 64u; ++i) {
        uint8_t character;
        uint32_t shifted;
        if (!black_pc_ram_span(source + 8u + i, 1u)) return 0;
        character = text[i];
        if (!character) {
            for (prompt = 0; prompt < sizeof(black_menu_prompts) / sizeof(black_menu_prompts[0]); ++prompt)
                if (black_menu_prompts[prompt].hash == hash) return 1;
            return 0;
        }
        shifted = (hash >> 8u) | ((hash & 0x80000000u) ? 0xFF000000u : 0u);
        hash = shifted ^ GUEST32(BLACK_VA(0x002AC908u) + ((hash ^ character) & 255u) * 4u);
    }
    return 0;
}


static int black_strlog_enabled(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("RECOMP_BLACK_STRLOG"); v = e && *e && *e != '0'; }
    return v;
}
static void black_strlog_name(const char *what, uint32_t display, uint32_t state, uint32_t source, uint32_t runtime)
{
    char name[48];
    size_t i;
    name[0] = 0;
    if (black_pc_ram_span(source, 9u)) {
        const char *text = (const char *)((uintptr_t)g_xbox_mem_offset + source + 8u);
        for (i = 0; i < 47u && black_pc_ram_span(source + 8u + (uint32_t)i, 1u) && text[i] >= 32 && text[i] < 127; ++i) name[i] = text[i];
        name[i] = 0;
    }
    fprintf(stderr, "[STRLOG] %s display=%08X state=%08X source=%08X runtime=%08X name=%s\n", what, display, state, source, runtime, name);
}

/* The text keys only an Options page draws: the pause menu's Options (crouch, look, vibration, volumes) and the
 * front end's option lists. The game updates every text object it shows once per frame, so a marker seen in the
 * last few frames means the page is on screen; the presenter then offers the native Video Settings page. */
static int black_video_page_marker(uint32_t source)
{
    static const char *const markers[] = {
        "$FE_TOGGLECROUCH", "$FE_INVERTLOOK", "$FE_VIBRATION", "$FE_SFXVOLUME", "$FE_MUSICVOLUME",
        "$FE_PICTURESETUP", "$FE_GAMMABOOST", "$FE_SOUNDSETUP", "$FE_CONTROLLERSETUP"
    };
    char name[24];
    size_t i;
    if (!black_pc_ram_span(source, 9u)) return 0;
    for (i = 0; i < sizeof(name) - 1u && black_pc_ram_span(source + 8u + (uint32_t)i, 1u); ++i) {
        const char c = *(const char *)((uintptr_t)g_xbox_mem_offset + source + 8u + (uint32_t)i);
        if (!c) break;
        name[i] = c;
    }
    name[i] = 0;
    for (i = 0; i < sizeof(markers) / sizeof(markers[0]); ++i)
        if (strcmp(name, markers[i]) == 0) return 1;
    return 0;
}

typedef struct BlackMenuTextState {
    uint32_t display, state, source, definition, runtime;
    uint32_t bounds[4], position_x;
    int device;
} BlackMenuTextState;

/* Only the exact known localized prompt sources enter this bounded registry. Entries
 * are retired by the native destructor, and every guest write is reached
 * from a freshly validated render object/source identity, never a saved VA.
 */
#define BLACK_MENU_TEXT_SLOTS 64u
static BlackMenuTextState black_menu_text_states[BLACK_MENU_TEXT_SLOTS];
static BlackMenuTextState *black_menu_rebuilding;

static void black_menu_text_destroy(void)
{
    const uint32_t runtime = black_pc_ram_span(g_esp, 8u) ? GUEST32(g_esp + 4u) : 0u;
    size_t i;
    if (black_strlog_enabled()) fprintf(stderr, "[STRLOG] destroy runtime=%08X\n", runtime);
    for (i = 0; i < BLACK_MENU_TEXT_SLOTS; ++i) {
        BlackMenuTextState *entry = &black_menu_text_states[i];
        if (entry != black_menu_rebuilding && runtime && entry->runtime == runtime)
            memset(entry, 0, sizeof(*entry));
    }
    sub_00114030_gen();
}

static void black_menu_text_prepare(void)
{
    const unsigned int caller_csr = _mm_getcsr();
    const uint32_t stack = g_esp;
    uint32_t display = 0, state = 0, source = 0, definition = 0;
    BlackMenuTextState *entry = NULL, *empty = NULL;
    const int device = (int)pc_input_prompt_device();
    size_t i;
    _mm_setcsr(caller_csr);
    if (black_pc_ram_span(stack, 8u)) display = GUEST32(stack + 4u);
    if (black_pc_ram_span(display, 0x5Cu) &&
        (GUEST32(display + 4u) & 0xFE000000u) == 0x1E000000u) {
        state = GUEST32(display + 0x48u);
        if (black_pc_ram_span(state, 0x78u)) {
            source = GUEST32(state + 0x1Cu);
            definition = GUEST32(state + 0x0Cu);
            if (black_strlog_enabled()) black_strlog_name("prepare", display, state, source, GUEST32(state + 0x20u));
            if (black_video_page_marker(source)) pc_video_menu_page_seen();
            if (black_menu_prompt_source(source)) {
                for (i = 0; i < BLACK_MENU_TEXT_SLOTS; ++i) {
                    BlackMenuTextState *candidate = &black_menu_text_states[i];
                    if (!candidate->display && !empty) empty = candidate;
                    if (candidate->display == display) {
                        if (candidate->state == state && candidate->source == source &&
                            candidate->definition == definition) entry = candidate;
                        else {
                            memset(candidate, 0, sizeof(*candidate));
                            empty = candidate;
                        }
                        break;
                    }
                }
                if (!entry && empty) {
                    entry = empty;
                    entry->display = display;
                    entry->state = state;
                    entry->source = source;
                    entry->definition = definition;
                    entry->runtime = GUEST32(state + 0x20u);
                    entry->device = device;
                    entry->position_x = GUEST32(display + 0x1Cu);
                    for (i = 0; i < 4u; ++i)
                        entry->bounds[i] = GUEST32(state + 0x50u + (uint32_t)i * 4u);
                }
                if (entry && entry->device != device) {
                    for (i = 0; i < 4u; ++i)
                        GUEST32(state + 0x50u + (uint32_t)i * 4u) = entry->bounds[i];
                    GUEST32(display + 0x1Cu) = entry->position_x;
                    GUEST32(state + 0x6Cu) = (GUEST32(state + 0x6Cu) & ~1u) | 2u;
                }
            }
        }
    }
    black_menu_rebuilding = entry;
    sub_00102D90_gen();
    black_menu_rebuilding = NULL;
    if (entry && black_pc_ram_span(display, 0x5Cu) &&
        GUEST32(display + 0x48u) == state && black_pc_ram_span(state, 0x78u) &&
        GUEST32(state + 0x1Cu) == source && GUEST32(state + 0x0Cu) == definition) {
        entry->runtime = GUEST32(state + 0x20u);
        entry->device = device;
    }
}

/* Asset export map semantics are also used by sub_00019F80: count+0x30,
 * entries+0x34 ({name, character index}), character count+0x14/table+0x18.
 * Compare the resolved definition, so unrelated uses of a texture survive.
 */
static int black_menu_controller_art(uint32_t display)
{
    uint32_t state, definition, asset, count, entries, chars, char_count, i, type;
    if (!black_pc_ram_span(display, 0x4Cu)) return 0;
    type = GUEST32(display + 4u) & 0xFE000000u;
    /* sub_00102840's original jump table maps types 13/18 to movieclip
     * traversal; type 12 is a leaf shape. Both clip aliases share +0x48.
     */
    if (type != 0x1A000000u && type != 0x24000000u) return 0;
    state = GUEST32(display + 0x48u);
    if (!black_pc_ram_span(state, 0x10u)) return 0;
    definition = GUEST32(state + 0x0Cu);
    if (!black_pc_ram_span(definition, 8u)) return 0;
    asset = GUEST32(definition + 4u);
    if (!black_pc_ram_span(asset, 0x38u)) return 0;
    count = GUEST32(asset + 0x30u);
    entries = GUEST32(asset + 0x34u);
    char_count = GUEST32(asset + 0x14u);
    chars = GUEST32(asset + 0x18u);
    if (!count || count > 4096u || !char_count || char_count > 65536u ||
        !black_pc_ram_span(entries, count * 8u) ||
        !black_pc_ram_span(chars, char_count * 4u)) return 0;
    for (i = 0; i < count; ++i) {
        const uint32_t name = GUEST32(entries + i * 8u);
        const uint32_t index = GUEST32(entries + i * 8u + 4u);
        if (index < char_count && GUEST32(chars + index * 4u) == definition &&
            (black_menu_guest_name(name, "Control_S") || black_menu_guest_name(name, "Control_E")))
            return 1;
    }
    return 0;
}

static void black_menu_render(void)
{
    const unsigned int caller_csr = _mm_getcsr();
    const uint32_t stack = g_esp;
    const int keyboard = pc_input_prompt_device() == PC_DEVICE_KEYBOARD_MOUSE;
    _mm_setcsr(caller_csr);
    if (keyboard && black_pc_ram_span(stack, 12u) &&
        black_menu_controller_art(GUEST32(stack + 4u))) {
        /* Original ret 8: return address and two stack arguments. */
        g_esp += 12u;
        return;
    }
    sub_00102840_gen();
}
