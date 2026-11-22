/* Authored actor fixtures; no game data or translated code. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fenv.h>
#include <xmmintrin.h>
#include "pc_video.h"
static uint8_t ram[0x300000];
static PcVideoSettings chosen;
static int disturb_settings_fp;
#define GUEST32(a) (*(uint32_t *)(ram + (a)))
#define GUEST8(a) (ram[(a)])
static int black_pc_ram_span(uint32_t address, uint32_t bytes)
{
    return address >= 0x10000u && (uint64_t)address + bytes <= sizeof(ram);
}
void pc_video_get(PcVideoSettings *out)
{
    *out = chosen;
    if (disturb_settings_fp) {
        fesetround(FE_UPWARD);
        feraiseexcept(FE_INVALID);
        _mm_setcsr(0x3F81u);
    }
}
#define BLACK_ACTOR_AT 0x30u
#define BLACK_ACTOR_BYTES 0x900u
static uint32_t black_p2_shadow, black_p2_core;
static int split_enabled, p2_playing;
static int pc_split_enabled(void) { return split_enabled; }
static int black_p2_playing(void) { return p2_playing; }
#include "../../src/black_gameplay_tuning.inc"
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static float from_bits(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }
static uint32_t bits_of(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
enum { ENEMY = 0x10000, CORE = 0x20000, SHOOTER = CORE + 0x30 };
static void reset(void)
{
    memset(ram, 0, sizeof(ram));
    memset(&chosen, 0, sizeof(chosen));
    chosen.enemy_health = chosen.bullet_damage = 100u;
    GUEST32(ENEMY) = 0x28C1E8u;
    GUEST32(ENEMY + 0xC4) = 1u;
    GUEST32(ENEMY + 0x324) = 0x40000u;
    GUEST32(0x2D45D4) = CORE;
    black_p2_core = black_p2_shadow = 0u;
    p2_playing = split_enabled = 0;
}
static void identity(void)
{
    static const uint32_t values[] = {0u, 0x80000000u, 1u, 0x42C80000u, 0x7F7FFFFFu,
        0xBF800000u, 0x7F800000u, 0xFF800000u, 0x7FC12345u, 0x7F812345u};
    unsigned i;
    reset();
    for (i = 0; i < sizeof(values)/sizeof(values[0]); ++i) {
        CHECK(bits_of(black_tuning_spawn_health(ENEMY, from_bits(values[i]))) == values[i]);
        CHECK(bits_of(black_tuning_bullet_damage(ENEMY, SHOOTER, from_bits(values[i]))) == values[i]);
        CHECK(black_tuning_headshot(ENEMY, SHOOTER, i, 7u) == 7u);
    }
    chosen.enemy_health = chosen.bullet_damage = 101u;
    CHECK(black_tuning_spawn_health(ENEMY, 100.0f) == 100.0f);
    CHECK(black_tuning_bullet_damage(ENEMY, SHOOTER, 10.0f) == 10.0f);
}
static void choices(void)
{
    static const unsigned health[] = {25,50,75,100,150,200,300,400};
    static const unsigned damage[] = {50,100,150,200,300,400,600,1000};
    unsigned i;
    reset();
    for (i = 0; i < sizeof(health)/sizeof(health[0]); ++i) {
        chosen.enemy_health = health[i];
        CHECK(black_tuning_spawn_health(ENEMY, 100.0f) == (float)health[i]);
        chosen.bullet_damage = damage[i];
        CHECK(black_tuning_bullet_damage(ENEMY, SHOOTER, 100.0f) == (float)damage[i]);
    }
    chosen.headshot_instakill = 1u;
    CHECK(black_tuning_headshot(ENEMY, SHOOTER, 0u, 0u) == 1u);
    CHECK(black_tuning_headshot(ENEMY, SHOOTER, 1u, 0u) == 0u);
    CHECK(black_tuning_headshot(ENEMY, SHOOTER, 4u, 0u) == 0u);
    CHECK(black_tuning_headshot(ENEMY, SHOOTER + 4, 0u, 0u) == 0u);
    GUEST8(ENEMY + 0x3B0) = 1u;
    CHECK(black_tuning_headshot(ENEMY, SHOOTER, 0u, 0u) == 0u);
    CHECK(black_tuning_headshot(ENEMY, SHOOTER, 0u, 1u) == 1u);
}
static void guards(void)
{
    unsigned i;
    const unsigned offsets[] = {0,0xC4,0x324,0x384};
    const unsigned invalid[] = {0,2,0,2};
    for (i = 0; i < 4; ++i) {
        reset();
        chosen.enemy_health = chosen.bullet_damage = 200u;
        chosen.headshot_instakill = 1u;
        GUEST32(ENEMY + offsets[i]) = invalid[i];
        CHECK(black_tuning_bullet_damage(ENEMY, SHOOTER, 10.0f) == 10.0f);
        CHECK(black_tuning_headshot(ENEMY, SHOOTER, 0, 0) == 0u);
        CHECK(black_tuning_spawn_health(ENEMY, 100.0f) == (i < 2 ? 100.0f : 200.0f));
    }
    reset();
    chosen.enemy_health = chosen.bullet_damage = 200u;
    CHECK(black_tuning_bullet_damage(ENEMY, SHOOTER + 4, 10.0f) == 10.0f);
    CHECK(black_tuning_spawn_health(UINT32_MAX - 16u, 100.0f) == 100.0f);
    CHECK(black_tuning_bullet_damage(0, SHOOTER, 10.0f) == 10.0f);
    GUEST32(0x2D45D4) = UINT32_MAX - 16u;
    CHECK(black_tuning_bullet_damage(ENEMY, SHOOTER, 10.0f) == 10.0f);
}
static void environment(void)
{
    static const int rounding[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    unsigned i;
    reset();
    chosen.enemy_health = 150u;
    chosen.bullet_damage = 300u;
    chosen.headshot_instakill = 1u;
    disturb_settings_fp = 1;
    for (i = 0; i < sizeof(rounding)/sizeof(rounding[0]); ++i) {
        unsigned csr; int exceptions;
        fesetenv(FE_DFL_ENV); fesetround(rounding[i]); feraiseexcept(FE_INEXACT);
        csr = _mm_getcsr(); exceptions = fetestexcept(FE_ALL_EXCEPT);
        CHECK(black_tuning_spawn_health(ENEMY, 100.0f) == 150.0f);
        CHECK(black_tuning_bullet_damage(ENEMY, SHOOTER, 10.0f) == 30.0f);
        CHECK(black_tuning_headshot(ENEMY, SHOOTER, 0u, 0u) == 1u);
        CHECK(_mm_getcsr() == csr);
        CHECK(fegetround() == rounding[i]);
        CHECK(fetestexcept(FE_ALL_EXCEPT) == exceptions);
    }
    fesetenv(FE_DFL_ENV);
}
static void invulnerability(int enabled)
{
    unsigned flag;
    uint8_t *snapshot = malloc(sizeof(ram));
    CHECK(snapshot != NULL);
    reset();
    GUEST32(SHOOTER + 0xC4u) = 2u;
    black_p2_core = CORE;
    black_p2_shadow = 0x40000u;
    GUEST32(black_p2_shadow + 0xC4u) = 2u;
    p2_playing = split_enabled = 1;
    memcpy(snapshot, ram, sizeof(ram));
    for (flag = 0; flag < 256u; ++flag) {
        unsigned csr; int exceptions;
        fesetenv(FE_DFL_ENV); fesetround(FE_UPWARD); feraiseexcept(FE_INEXACT);
        csr = _mm_getcsr(); exceptions = fetestexcept(FE_ALL_EXCEPT);
        CHECK(black_tuning_invulnerable(SHOOTER, flag) == (flag ? flag : (unsigned)enabled));
        CHECK(black_tuning_invulnerable(black_p2_shadow, flag) == (flag ? flag : (unsigned)enabled));
        CHECK(_mm_getcsr() == csr && fegetround() == FE_UPWARD);
        CHECK(fetestexcept(FE_ALL_EXCEPT) == exceptions);
        CHECK(black_tuning_invulnerable(ENEMY, flag) == flag);
        CHECK(black_tuning_invulnerable(SHOOTER + 4u, flag) == flag);
    }
    CHECK(!memcmp(snapshot, ram, sizeof(ram)));
    split_enabled = 0;
    CHECK(black_tuning_invulnerable(black_p2_shadow, 0u) == 0u);
    split_enabled = 1;
    p2_playing = 0;
    CHECK(black_tuning_invulnerable(black_p2_shadow, 0u) == 0u);
    p2_playing = 1;
    black_p2_core += 4u;
    CHECK(black_tuning_invulnerable(black_p2_shadow, 0u) == 0u);
    black_p2_core = CORE;
    GUEST32(black_p2_shadow + 0xC4u) = 1u;
    CHECK(black_tuning_invulnerable(black_p2_shadow, 0u) == 0u);
    black_p2_shadow = UINT32_MAX;
    CHECK(black_tuning_invulnerable(black_p2_shadow, 0u) == 0u);
    GUEST32(SHOOTER + 0xC4u) = 1u;
    CHECK(black_tuning_invulnerable(SHOOTER, 0u) == 0u);
    GUEST32(0x2D45D4u) = UINT32_MAX - 16u;
    CHECK(black_tuning_invulnerable(SHOOTER, 0u) == 0u);
    GUEST32(0x2D45D4u) = 0u;
    CHECK(black_tuning_invulnerable(SHOOTER, 0u) == 0u);
    CHECK(black_tuning_invulnerable(UINT32_MAX, 0u) == 0u);
    free(snapshot);
    fesetenv(FE_DFL_ENV);
}
int main(int argc, char **argv)
{
    invulnerability(argc > 1 && !strcmp(argv[1], "health-on"));
    identity(); choices(); guards(); environment();
    printf("gameplay tuning: %u checks passed\n", checks);
    return 0;
}
