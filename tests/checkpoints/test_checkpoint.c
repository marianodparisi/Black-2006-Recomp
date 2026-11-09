/* Executes the production persistence hooks against synthetic guest memory. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fenv.h>
#include <xmmintrin.h>
static uint8_t ram[0x800000], before[sizeof(ram)];
static ptrdiff_t g_xbox_mem_offset;
#define BLACK_VA(va) (va) /* fixtures use the canonical (NTSC) layout */
#define GUEST32(a) (*(uint32_t *)(ram + (a)))
static int black_pc_ram_span(uint32_t address, uint32_t bytes)
{
    return address >= 0x10000u && (uint64_t)address + bytes <= sizeof(ram);
}
static int captured, restarted;
void sub_001C8D20_gen(void) { ++captured; }
void sub_00124190_gen(void) { ++restarted; ram[0x2F3BF8] = 0; }
void sub_00124250_gen(void) { ++restarted; ram[0x2F3BF8] = 0; }
#include "../../src/black_checkpoint.inc"
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void setup(int relocated)
{
    unsigned i;
    uint32_t bias = relocated ? 0x100000u : 0;
    memset(ram, 0, sizeof(ram));
    g_xbox_mem_offset = (ptrdiff_t)ram;
    GUEST32(0x2D45D4) = 0x10000;
    GUEST32(0x2D45F0) = 0x20000;
    GUEST32(0x2D19B4) = 0x60000;
    GUEST32(0x60000) = 0x50000;
    GUEST32(0x50000) = 123456;
    memcpy(ram + 0x50008, "Checkpoint Test", 16);
    GUEST32(0x20000 + 0x2014C) = 1;
    ram[0x10000 + 0x5AED] = 1;
    GUEST32(0x10000 + 0x5AF4) = 0x61000;
    ram[0x61011] = 4;
    ram[0x61012] = 1;
    GUEST32(0x2D12D0) = 0x62000;
    GUEST32(0x62000) = 0x63000;
    ram[0x63001] = 2;
    GUEST32(0x63004) = 0x64000;
    memcpy(ram + 0x64000, "PRIMARY!", 8);
    memcpy(ram + 0x64020, "SECOND!!", 8);
    GUEST32(0x10000 + 0x934) = 3;
    GUEST32(0x10000 + 0x930) = 0x65000 + bias;
    GUEST32(0x2F3BFC) = 0x66000 + bias;
    GUEST32(0x10000 + 0x440) = 0x67000 + bias;
    for (i = 0; i < 3; ++i) {
        uint32_t descriptor = 0x68000 + bias + 32 * i;
        GUEST32(descriptor) = 0xABC00000 + i;
        GUEST32(descriptor + 4) = 0x76543210 + i;
        GUEST32(0x65000 + bias + 12 * i) = descriptor;
        GUEST32(0x65004 + bias + 12 * i) = 0;
        GUEST32(0x65008 + bias + 12 * i) = 0;
        GUEST32(0x66000 + bias + 12 * i) = descriptor;
        GUEST32(0x66004 + bias + 12 * i) = 7 + i;
        GUEST32(0x66008 + bias + 12 * i) = i;
    }
}

static void capture_data(void)
{
    unsigned i;
    memcpy(ram + 0x103F0, "PRIMARY!SECOND!!", 16);
    GUEST32(0x10400) = 0x44960000; /* 1200 native health */
    for (i = 20; i <= 64; i += 4) GUEST32(0x103F0 + i) = i;
    GUEST32(0x10438) = 0;
    ram[0x67000] = 1;
    GUEST32(0x2A7EDC) = 3;
    ram[0x2F3BF8] = 1;
    GUEST32(0x2F3BF4) = 0x42C80000;
    for (i = 0; i < 16; ++i) ram[0x2F3C00 + i] = (uint8_t)i;
}

static void invoke(unsigned event)
{
    fenv_t environment;
    unsigned csr;
    fesetround(FE_DOWNWARD);
    feraiseexcept(FE_INEXACT);
    fegetenv(&environment);
    csr = _mm_getcsr();
    black_checkpoint_event(event, event == 4 ? 0x70000 : 0x10000);
    CHECK(_mm_getcsr() == csr);
    CHECK(fegetround() == FE_DOWNWARD);
    CHECK(fetestexcept(FE_ALL_EXCEPT) == FE_INEXACT);
    fesetenv(&environment);
}

static void profile_saved(void)
{
    const uint32_t payload = 0x70000 + 4712;
    GUEST32(payload) = 38;
    memcpy(ram + payload + 0xC8, ram + 0x50000, 0x6A8);
    GUEST32(payload + 0xC8) = 654321;
    invoke(4);
}

int main(int argc, char **argv)
{
    BlackCheckpointContext c;
    char path[160];
    CHECK(argc == 2);
    setup(strcmp(argv[1], "save") && strncmp(argv[1], "binding-", 8));
    if (!strcmp(argv[1], "load-rebound") || !strcmp(argv[1], "load-latest")) GUEST32(0x50000) = 654321;
    CHECK(black_cp_context(&c, 1));
    black_cp_path(&c, path);
    if (!strcmp(argv[1], "save")) {
        capture_data();
        sub_001C8D20();
        CHECK(captured == 1);
        CHECK(GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES);
    } else if (!strncmp(argv[1], "binding-", 8)) {
        /* Autosave can precede the first capture or occur between captures. */
        if (!strcmp(argv[1], "binding-before")) profile_saved();
        capture_data();
        invoke(1);
        if (!strcmp(argv[1], "binding-after")) profile_saved();
        CHECK(black_cp_cached && black_cp_cached->profile_stamp == 654321);
        GUEST32(0x2A7EDC) = 4;
        invoke(1);
        CHECK(black_cp_cached->profile_stamp == 654321 && black_cp_cached->unit == 4);
    } else if (!strcmp(argv[1], "load") || !strcmp(argv[1], "load-rebound") || !strcmp(argv[1], "load-latest") ||
               !strcmp(argv[1], "rebind") || !strcmp(argv[1], "rebind-write") || !strcmp(argv[1], "retire")) {
        const uint32_t unit = !strcmp(argv[1], "load-latest") ? 4u : 3u;
        invoke(2);
        CHECK(ram[0x2F3BF8] == 1);
        CHECK(GUEST32(0x2A7EDC) == unit && GUEST32(0x2EC464) == 1);
        CHECK(GUEST32(0x10000 + 0x5AF0) == unit);
        CHECK(GUEST32(0x10000 + 0x440) == 0x167000); /* fresh allocation retained */
        CHECK(ram[0x167000] == 1);
        CHECK(!memcmp(ram + 0x103F0, "PRIMARY!SECOND!!", 16));
        CHECK(GUEST32(0x10400) == 0x44960000);
        CHECK(GUEST32(0x2F3BF4) == 0x42C80000);
        CHECK(GUEST32(0x166004) == 7 && GUEST32(0x16601C) == 9);
        CHECK(GUEST32(0x166000) == 0x168000); /* current descriptor, never the old pointer */
        CHECK(GUEST32(0x165008) == 0); /* live objectives wait for native restore */
        CHECK(GUEST32(0x10000 + 0x49D4) == 4);
        CHECK(GUEST32(0x10000 + 0x5254) == (unit == 4 ? UINT32_MAX : 3));
        CHECK(ram[0x10000 + 0x5AEE] == 1);
        /* A later hook cannot replace an authoritative in-session checkpoint. */
        GUEST32(0x2A7EDC) = 4;
        memcpy(before, ram, sizeof(ram));
        invoke(2);
        CHECK(!memcmp(before, ram, sizeof(ram)));
        if (strcmp(argv[1], "load") && strcmp(argv[1], "load-rebound") && strcmp(argv[1], "load-latest")) {
            uint32_t payload = 0x70000 + 4712;
            char rebound[160];
            GUEST32(payload) = 38;
            memcpy(ram + payload + 0xC8, ram + 0x50000, 0x6A8);
            GUEST32(payload + 0xC8) = 654321;
            if (!strcmp(argv[1], "retire")) GUEST32(payload + 0x124) = 2;
            invoke(4);
            if (!strcmp(argv[1], "retire")) {
                CHECK(black_cp_cached == NULL);
                CHECK(GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
            } else {
                CHECK(black_cp_cached && black_cp_cached->profile_stamp == 654321);
                black_cp_saved_path(black_cp_cached, rebound);
                CHECK(GetFileAttributesA(rebound) != INVALID_FILE_ATTRIBUTES);
                /* Native profile memory retains its old date until reloaded. */
                CHECK(GUEST32(0x50000) == 123456);
                if (strcmp(argv[1], "rebind-write")) {
                    sub_00124250();
                    CHECK(restarted == 1 && black_cp_cached == NULL);
                    CHECK(GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
                    CHECK(GetFileAttributesA(rebound) == INVALID_FILE_ATTRIBUTES);
                }
            }
        }
    } else if (!strcmp(argv[1], "levels")) {
        unsigned level;
        for (level = 0; level <= 9; ++level) {
            ram[0x10000 + 0x5AEC] = (uint8_t)level;
            CHECK(black_cp_context(&c, 1) == (level <= 8 && level != 2));
        }
    } else if (!strcmp(argv[1], "failed-write")) {
        HANDLE locked = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(locked != INVALID_HANDLE_VALUE);
        capture_data();
        GUEST32(0x2A7EDC) = 4;
        invoke(1);
        CloseHandle(locked);
    } else if (!strcmp(argv[1], "reject")) {
        memcpy(before, ram, sizeof(ram));
        invoke(2);
        CHECK(!memcmp(before, ram, sizeof(ram)));
    } else if (!strcmp(argv[1], "mismatch")) {
        GUEST32(0x168000) ^= 1; /* wrong level objective identifier */
        memcpy(before, ram, sizeof(ram));
        invoke(2);
        CHECK(!memcmp(before, ram, sizeof(ram)));
    } else if (!strcmp(argv[1], "clear")) {
        sub_00124190();
        CHECK(restarted == 1 && GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
        memcpy(before, ram, sizeof(ram));
        invoke(2);
        CHECK(!memcmp(before, ram, sizeof(ram)));
    } else return 2;
    printf("%s: %u checkpoint checks passed\n", argv[1], checks);
    return 0;
}
