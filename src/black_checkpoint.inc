/* Disk persistence for the supported retail build's native checkpoint.
 * No guest pointers are serialized. Loading runs after Guns/LevelDat are ready,
 * before either initial streaming unit starts (1EDC90:1EE310).
 * Native death/ContinueMission continues to own the actual restore.
 */
#define BLACK_CP_MAX_OBJECTIVES 4096u
#define BLACK_CP_MAX_GUNS 127u
typedef struct BlackCheckpoint {
    uint32_t magic, version, checksum, profile_stamp;
    uint32_t level, stage, difficulty, unit, objectives, guns;
    uint8_t name[80];
    uint8_t player[80];             /* +0x50 is an allocation, deliberately excluded */
    uint8_t stats[24];              /* 2F3BF4 time, six counters and two shorts */
    uint32_t campaign_hash;
    uint8_t inventory[BLACK_CP_MAX_GUNS];
    uint8_t objective_data[16u * BLACK_CP_MAX_OBJECTIVES]; /* descriptor ID64, counter, state */
} BlackCheckpoint;

typedef struct BlackCheckpointContext {
    uint32_t core, main, profile, header, gun_header;
    uint32_t objective_data, objective_backup, inventory, objective_count, gun_count;
} BlackCheckpointContext;

static BlackCheckpoint *black_cp_cached;
static uint32_t black_cp_origin_stamp;
static char black_cp_discarded[160];
/* A profile may be saved before any checkpoint exists, or between captures.
 * Remember only a confirmed successful save, not an in-flight slot buffer. */
static struct {
    uint32_t origin_stamp, saved_stamp, campaign_hash;
    uint8_t name[80];
} black_cp_binding;

static uint8_t *black_cp_memory(uint32_t address)
{
    return (uint8_t *)((uintptr_t)g_xbox_mem_offset + address);
}

static uint32_t black_cp_hash(const void *data, size_t length)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t hash = 2166136261u;
    while (length--) hash = (hash ^ *p++) * 16777619u;
    return hash;
}

static int black_cp_context(BlackCheckpointContext *c, int buffers)
{
    uint32_t manager, guns;
    memset(c, 0, sizeof(*c));
    c->core = GUEST32(BLACK_VA(0x2D45D4u));
    c->main = GUEST32(BLACK_VA(0x2D45F0u));
    manager = GUEST32(BLACK_VA(0x2D19B4u));
    if (!black_pc_ram_span(c->core, 0x5CB4u) ||
        !black_pc_ram_span(c->main, 0x21090u) || !black_pc_ram_span(manager, 4u)) return 0;
    c->profile = GUEST32(manager);
    /* Empty/new unsaved profiles have no stable identity across launches. */
    if (!black_pc_ram_span(c->profile, 0x6A8u) ||
        black_cp_memory(c->profile)[1701] || !GUEST32(c->profile) ||
        !black_cp_memory(c->profile)[8] || !memchr(black_cp_memory(c->profile + 8u), 0, 80u)) return 0;
    if (black_cp_memory(c->core)[0x5AECu] > 8u || black_cp_memory(c->core)[0x5AECu] == 2u ||
        black_cp_memory(c->core)[0x5AEDu] != 1u || GUEST32(c->main + 0x2014Cu) > 3u) return 0;
    c->header = GUEST32(c->core + 0x5AF4u);
    if (!black_pc_ram_span(c->header, 0x13u) ||
        !black_cp_memory(c->header)[0x11] || black_cp_memory(c->header)[0x11] > 127u) return 0;
    if (!buffers) return 1;
    guns = GUEST32(BLACK_VA(0x2D12D0u));
    if (!black_pc_ram_span(guns, 4u)) return 0;
    c->gun_header = GUEST32(guns);
    if (!black_pc_ram_span(c->gun_header, 8u)) return 0;
    c->gun_count = black_cp_memory(c->gun_header)[1];
    c->objective_count = GUEST32(c->core + 0x934u);
    c->objective_data = GUEST32(c->core + 0x930u);
    c->objective_backup = GUEST32(BLACK_VA(0x2F3BFCu));
    c->inventory = GUEST32(c->core + 0x440u);
    return c->gun_count && c->gun_count <= BLACK_CP_MAX_GUNS &&
        c->objective_count <= BLACK_CP_MAX_OBJECTIVES &&
        black_pc_ram_span(c->inventory, c->gun_count) &&
        (!c->objective_count ||
         (black_pc_ram_span(c->objective_data, 12u * c->objective_count) &&
          black_pc_ram_span(c->objective_backup, 12u * c->objective_count)));
}

static int black_cp_path(const BlackCheckpointContext *c, char path[160])
{
    return snprintf(path, 160, "save/checkpoints/%08X-%08X-%u-%u-%u.bin",
        GUEST32(c->profile), black_cp_hash(black_cp_memory(c->profile + 8u), 80u),
        GUEST32(c->main + 0x2014Cu), black_cp_memory(c->core)[0x5AECu],
        black_cp_memory(c->core)[0x5AEDu]) > 0;
}

static void black_cp_saved_path(const BlackCheckpoint *cp, char path[160])
{
    snprintf(path, 160, "save/checkpoints/%08X-%08X-%u-%u-%u.bin", cp->profile_stamp,
        black_cp_hash(cp->name, 80u), cp->difficulty, cp->level, cp->stage);
}

static int black_cp_write(const char *path, BlackCheckpoint *cp)
{
    char temporary[192];
    HANDLE file;
    DWORD written;
    int ok;
    CreateDirectoryA("save", NULL);
    CreateDirectoryA("save/checkpoints", NULL);
    snprintf(temporary, sizeof(temporary), "%s.%lu.tmp", path, (unsigned long)GetCurrentProcessId());
    cp->checksum = 0;
    cp->checksum = black_cp_hash(cp, sizeof(*cp));
    file = CreateFileA(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(file, cp, sizeof(*cp), &written, NULL) && written == sizeof(*cp) && FlushFileBuffers(file);
    if (!CloseHandle(file)) ok = 0;
    if (ok) ok = MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    /* A failed replacement retains the last complete checkpoint. */
    if (!ok) DeleteFileA(temporary);
    return ok;
}

static void black_cp_identity(BlackCheckpoint *cp, const BlackCheckpointContext *c)
{
    memset(cp, 0, sizeof(*cp));
    cp->magic = 0x50434C42u; /* BLCP */
    cp->version = 2;
    cp->profile_stamp = GUEST32(c->profile);
    cp->level = black_cp_memory(c->core)[0x5AECu];
    cp->stage = black_cp_memory(c->core)[0x5AEDu];
    cp->difficulty = GUEST32(c->main + 0x2014Cu);
    cp->campaign_hash = black_cp_hash(black_cp_memory(c->profile + 92u), 1024u);
    memcpy(cp->name, black_cp_memory(c->profile + 8u), 80);
}

static void black_cp_bind_cached(void)
{
    BlackCheckpoint *cp = black_cp_cached;
    uint32_t previous;
    char path[160];
    if (!cp || !black_cp_binding.saved_stamp ||
        black_cp_binding.origin_stamp != black_cp_origin_stamp ||
        black_cp_binding.campaign_hash != cp->campaign_hash ||
        memcmp(black_cp_binding.name, cp->name, 80u) ||
        cp->profile_stamp == black_cp_binding.saved_stamp) return;
    previous = cp->profile_stamp;
    cp->profile_stamp = black_cp_binding.saved_stamp;
    black_cp_saved_path(cp, path);
    if (!black_cp_write(path, cp)) {
        cp->profile_stamp = previous;
        fprintf(stderr, "[CHECKPOINT] profile-save rebinding failed\n");
    }
}

static void black_cp_save(void)
{
    BlackCheckpointContext c;
    BlackCheckpoint *cp;
    char path[160];
    if (!black_cp_memory(BLACK_VA(0x2F3BF8u))[0] || !black_cp_context(&c, 1)) return;
    cp = (BlackCheckpoint *)calloc(1, sizeof(*cp));
    if (!cp) return;
    black_cp_identity(cp, &c);
    cp->unit = GUEST32(BLACK_VA(0x2A7EDCu));
    if (cp->unit < 2u || cp->unit > black_cp_memory(c.header)[0x11]) { free(cp); return; }
    cp->objectives = c.objective_count;
    cp->guns = c.gun_count;
    memcpy(cp->player, black_cp_memory(c.core + 0x3F0u), 80);
    memcpy(cp->stats, black_cp_memory(BLACK_VA(0x2F3BF4u)), 4);
    memcpy(cp->stats + 4, black_cp_memory(BLACK_VA(0x2F3C00u)), 16);
    memcpy(cp->inventory, black_cp_memory(c.inventory), c.gun_count);
    {
        uint32_t i;
        for (i = 0; i < c.objective_count; ++i) {
            const uint32_t entry = c.objective_backup + i * 12u;
            const uint32_t descriptor = GUEST32(entry);
            if (!black_pc_ram_span(descriptor, 22u)) { free(cp); return; }
            memcpy(cp->objective_data + i * 16u, black_cp_memory(descriptor), 8u);
            memcpy(cp->objective_data + i * 16u + 8u, black_cp_memory(entry + 4u), 8u);
        }
    }
    black_cp_path(&c, path);
    {
        const int saved = black_cp_write(path, cp);
        if (saved && !strcmp(path, black_cp_discarded)) black_cp_discarded[0] = 0;
        fprintf(stderr, "[CHECKPOINT] %s level=%u stage=%u difficulty=%u unit=%u\n",
            saved ? "saved" : "SAVE FAILED", cp->level, cp->stage, cp->difficulty, cp->unit);
    }
    free(black_cp_cached);
    black_cp_cached = cp;
    black_cp_origin_stamp = GUEST32(c.profile);
    black_cp_bind_cached();
}

static int black_cp_token(const BlackCheckpointContext *c, const uint8_t *token, int optional)
{
    uint32_t table = GUEST32(c->gun_header + 4u), i, matches = 0;
    static const uint8_t empty[8] = {0};
    if (optional && !memcmp(token, empty, 8)) return 1;
    if (!black_pc_ram_span(table, c->gun_count * 32u)) return 0;
    for (i = 0; i < c->gun_count; ++i)
        if (!memcmp(token, black_cp_memory(table + i * 32u), 8)) ++matches;
    return matches == 1u;
}

static int black_cp_valid(const BlackCheckpoint *cp, const BlackCheckpointContext *c)
{
    uint32_t i, value;
    if (cp->magic != 0x50434C42u || cp->version != 2u ||
        cp->profile_stamp != GUEST32(c->profile) ||
        cp->campaign_hash != black_cp_hash(black_cp_memory(c->profile + 92u), 1024u) ||
        memcmp(cp->name, black_cp_memory(c->profile + 8u), 80) ||
        cp->level != black_cp_memory(c->core)[0x5AECu] ||
        cp->stage != black_cp_memory(c->core)[0x5AEDu] ||
        cp->difficulty != GUEST32(c->main + 0x2014Cu) ||
        cp->unit < 2u || cp->unit > black_cp_memory(c->header)[0x11] ||
        cp->guns != c->gun_count || cp->objectives != c->objective_count ||
        !black_cp_token(c, cp->player, 0) || !black_cp_token(c, cp->player + 8, 1)) return 0;
    memcpy(&value, cp->player + 72, 4);
    if (value != 0u) return 0; /* supported single-player record */
    memcpy(&value, cp->player + 16, 4);
    if (!value || value >= 0x7F800000u) return 0; /* positive finite native health */
    for (i = 20; i <= 64; i += 4) {
        memcpy(&value, cp->player + i, 4);
        if (value > 65535u) return 0;
    }
    for (i = 0; i < c->objective_count; ++i) {
        const uint32_t descriptor = GUEST32(c->objective_data + i * 12u);
        if (!black_pc_ram_span(descriptor, 22u) ||
            memcmp(cp->objective_data + i * 16u, black_cp_memory(descriptor), 8u)) return 0;
        memcpy(&value, cp->objective_data + i * 16u + 8u, 4u);
        if (value > 65535u) return 0;
        memcpy(&value, cp->objective_data + i * 16u + 12u, 4u);
        if (value > 3u) return 0;
    }
    return 1;
}

static void black_cp_load(uint32_t core)
{
    BlackCheckpointContext c;
    BlackCheckpoint *cp;
    char path[160];
    FILE *file;
    uint32_t checksum, parity;
    int complete;
    /* A live checkpoint is already authoritative, including after death. */
    if (black_cp_memory(BLACK_VA(0x2F3BF8u))[0] || !black_cp_context(&c, 1) || core != c.core) return;
    black_cp_path(&c, path);
    if (!strcmp(path, black_cp_discarded)) return;
    file = fopen(path, "rb");
    if (!file) return;
    cp = (BlackCheckpoint *)malloc(sizeof(*cp));
    if (!cp) { fclose(file); return; }
    complete = fread(cp, 1, sizeof(*cp), file) == sizeof(*cp) && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    checksum = complete ? cp->checksum : 0u;
    if (complete) cp->checksum = 0;
    if (!complete || black_cp_hash(cp, sizeof(*cp)) != checksum || !black_cp_valid(cp, &c)) {
        fprintf(stderr, "[CHECKPOINT] ignored incompatible or incomplete checkpoint\n");
        free(cp);
        return;
    }
    /* Validate everything first. Keep current allocations, never old addresses. */
    memcpy(black_cp_memory(c.core + 0x3F0u), cp->player, 80);
    memcpy(black_cp_memory(c.inventory), cp->inventory, c.gun_count);
    {
        uint32_t i;
        for (i = 0; i < c.objective_count; ++i) {
            const uint32_t entry = c.objective_backup + i * 12u;
            GUEST32(entry) = GUEST32(c.objective_data + i * 12u);
            memcpy(black_cp_memory(entry + 4u), cp->objective_data + i * 16u + 8u, 8u);
        }
    }
    memcpy(black_cp_memory(BLACK_VA(0x2F3BF4u)), cp->stats, 4);
    memcpy(black_cp_memory(BLACK_VA(0x2F3C00u)), cp->stats + 4, 16);
    GUEST32(BLACK_VA(0x2A7EDCu)) = cp->unit;
    GUEST32(BLACK_VA(0x2EC464u)) = 1;
    GUEST32(c.core + 0x5AF0u) = cp->unit;
    black_cp_memory(c.main)[0x2020Du] = (uint8_t)cp->unit;
    parity = (cp->unit + !(black_cp_memory(c.header)[0x12] & 1u)) % 2u;
    GUEST32(c.core + 0x49D4u + parity * 0x880u) = cp->unit;
    GUEST32(c.core + 0x49D4u + (parity ^ 1u) * 0x880u) =
        cp->unit < black_cp_memory(c.header)[0x11] ? cp->unit + 1u : UINT32_MAX;
    black_cp_memory(c.core)[0x5AEEu] = (uint8_t)(cp->unit < black_cp_memory(c.header)[0x11] ? parity : parity ^ 1u);
    black_cp_memory(BLACK_VA(0x2F3BF8u))[0] = 1;
    fprintf(stderr, "[CHECKPOINT] restored level=%u stage=%u difficulty=%u unit=%u\n",
        cp->level, cp->stage, cp->difficulty, cp->unit);
    free(black_cp_cached);
    black_cp_cached = cp;
    black_cp_origin_stamp = GUEST32(c.profile);
}

static void black_cp_discard_cached(void)
{
    char path[160];
    BlackCheckpoint *cp = black_cp_cached;
    if (!cp) return;
    black_cp_saved_path(cp, path);
    DeleteFileA(path);
    cp->profile_stamp = black_cp_origin_stamp;
    black_cp_saved_path(cp, path);
    strcpy(black_cp_discarded, path);
    DeleteFileA(path);
    free(cp);
    black_cp_cached = NULL;
}

static void black_cp_clear(void)
{
    BlackCheckpointContext c;
    char path[160];
    if (black_cp_context(&c, 0)) {
        black_cp_path(&c, path);
        strcpy(black_cp_discarded, path);
        if (!DeleteFileA(path) && GetLastError() != ERROR_FILE_NOT_FOUND)
            fprintf(stderr, "[CHECKPOINT] could not discard saved checkpoint (%lu)\n", (unsigned long)GetLastError());
        if (black_cp_cached && black_cp_origin_stamp == GUEST32(c.profile) &&
            !memcmp(black_cp_cached->name, black_cp_memory(c.profile + 8u), 80u)) {
            black_cp_discard_cached();
        }
    }
}

/* A successful native profile autosave gives the on-disk profile a new date.
 * Rebind only the checkpoint belonging to its old identity; failed profile
 * writes never reach 1FE713. Changed campaign results retire the checkpoint.
 * Keep the original identity too, since the running title retains that date.
 */
static void black_cp_profile_saved(uint32_t controller)
{
    uint32_t manager, profile, slot, payload;
    BlackCheckpoint *cp = black_cp_cached;
    if (!black_pc_ram_span(controller, 0x2A68u)) return;
    manager = GUEST32(BLACK_VA(0x2D19B4u));
    if (!black_pc_ram_span(manager, 4u)) return;
    profile = GUEST32(manager);
    if (!black_pc_ram_span(profile, 0x6A8u)) return;
    slot = GUEST32(controller + 1316u);
    if (slot > 2u) return;
    payload = controller + 4712u + slot * 1912u;
    if (GUEST32(payload) != 38u || black_cp_memory(payload)[0x76Du] ||
        !GUEST32(payload + 0xC8u) ||
        memcmp(black_cp_memory(profile + 8u), black_cp_memory(payload + 0xD0u), 80u)) return;
    black_cp_binding.origin_stamp = GUEST32(profile);
    black_cp_binding.saved_stamp = GUEST32(payload + 0xC8u);
    black_cp_binding.campaign_hash = black_cp_hash(black_cp_memory(payload + 0x124u), 1024u);
    memcpy(black_cp_binding.name, black_cp_memory(profile + 8u), 80u);
    if (!cp || GUEST32(profile) != black_cp_origin_stamp ||
        memcmp(cp->name, black_cp_memory(profile + 8u), 80u)) return;
    if (cp->campaign_hash != black_cp_binding.campaign_hash) {
        black_cp_discard_cached();
        return;
    }
    black_cp_bind_cached();
}

/* All hooks are game-thread calls. Preserve the caller's complete FP controls
 * and sticky flags across host allocation, filesystem and diagnostic code. */
void black_checkpoint_event(uint32_t event, uint32_t core)
{
    const unsigned csr = _mm_getcsr();
    fenv_t environment;
    fegetenv(&environment);
    fesetenv(FE_DFL_ENV);
    _mm_setcsr(0x1F80u);
    if (event == 2u && getenv("RECOMP_BLACK_CHECKPOINT_TRACE")) {
        BlackCheckpointContext c;
        const int valid = black_cp_context(&c, 1);
        fprintf(stderr, "[CHECKPOINT_TRACE] init core=%08X context=%d profile=%08X inventory=%08X objectives=%u backup=%08X live=%u\n",
            core, valid, c.profile, c.inventory, c.objective_count, c.objective_backup,
            black_cp_memory(BLACK_VA(0x2F3BF8u))[0]);
    }
    if (event == 1u) black_cp_save();
    else if (event == 2u) black_cp_load(core);
    else if (event == 3u) black_cp_clear();
    else if (event == 4u) black_cp_profile_saved(core);
    fesetenv(&environment);
    _mm_setcsr(csr);
}

extern void sub_001C8D20_gen(void);
void sub_001C8D20(void)
{
    sub_001C8D20_gen();
    black_checkpoint_event(1u, 0u);
}

extern void sub_00124190_gen(void);
void sub_00124190(void)
{
    black_checkpoint_event(3u, 0u);
    sub_00124190_gen();
}

extern void sub_00124250_gen(void);
void sub_00124250(void)
{
    black_checkpoint_event(3u, 0u);
    sub_00124250_gen();
}
