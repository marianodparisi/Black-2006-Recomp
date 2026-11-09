/* Authored synthetic values exercise the production comparison helper. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <xmmintrin.h>
#include "pc_video.h"

#ifndef RECOMP_TLS
#ifdef _MSC_VER
#define RECOMP_TLS __declspec(thread)
#else
#define RECOMP_TLS __thread
#endif
#endif
static PcVideoSettings chosen;
static uint8_t ram[0x300000];
static intptr_t g_xbox_mem_offset;
#define BLACK_VA(va) (va) /* fixtures use the canonical (NTSC) layout */
#define GUEST32(a) (*(uint32_t *)(ram + (a)))
static int black_pc_ram_span(uint32_t address, uint32_t bytes)
{
    return address >= 0x10000u && (uint64_t)address + bytes <= sizeof(ram);
}
void pc_video_get(PcVideoSettings *settings) { *settings = chosen; }
#include "../../src/black_lod.inc"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static float from_bits(uint32_t bits) { float value; memcpy(&value, &bits, 4); return value; }
static uint32_t bits_of(float value) { uint32_t bits; memcpy(&bits, &value, 4); return bits; }

static void identity(void)
{
    static const uint32_t values[] = {0u, 0x80000000u, 0x00000001u, 0x3F800000u, 0x42F00000u,
        0x7F7FFFFFu, 0x7F800000u, 0xFF800000u, 0x7FC12345u, 0x7F812345u, 0xFFC12345u, 0xBF800000u};
    static const unsigned invalid[] = {0u, 1u, 99u, 100u, 101u, 400u, 1001u, UINT32_MAX};
    unsigned i, j, mode;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        chosen.lod_distance = invalid[i];
        for (j = 0; j < sizeof(values) / sizeof(values[0]); ++j)
            for (mode = 0; mode < 4u; ++mode)
                CHECK(bits_of(black_lod_value(from_bits(values[j]), mode)) == values[j]);
    }
    chosen.lod_distance = 1000u;
    for (j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
        const uint32_t bits = values[j];
        if (bits == 0u || bits >= 0x7F800000u)
            for (mode = 0; mode < 3u; ++mode)
                CHECK(bits_of(black_lod_value(from_bits(bits), mode)) == bits);
        CHECK(bits_of(black_lod_value(from_bits(bits), UINT32_MAX)) == bits);
    }
}

static void distances(void)
{
    static const unsigned percentages[] = {100u, 200u, 300u, 500u, 800u, 1000u};
    static const float values[] = {0.0f, 0.125f, 1.0f, 40.0f, 100.0f, 120.0f, 12345.5f};
    unsigned i, j;
    for (i = 0; i < sizeof(percentages) / sizeof(percentages[0]); ++i) {
        const float scale = (float)percentages[i] / 100.0f;
        chosen.lod_distance = percentages[i];
        for (j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
            const float value = values[j];
            CHECK(black_lod_value(value, BLACK_LOD_DISTANCE) == value / scale);
            CHECK(black_lod_value(value, BLACK_LOD_SQUARED_DISTANCE) == value / (scale * scale));
            CHECK(black_lod_value(value, BLACK_LOD_SCREEN_RATIO) == value * scale);
        }
        /* The authored visibility oracles cover each comparison direction. */
        CHECK(black_lod_value(40.0f * scale, BLACK_LOD_DISTANCE) <= 40.0f);
        CHECK(black_lod_value(nextafterf(40.0f * scale, INFINITY), BLACK_LOD_DISTANCE) > 40.0f);
        CHECK(black_lod_value(120.0f * scale, BLACK_LOD_DISTANCE) <= 120.0f);
        CHECK(black_lod_value(100.0f * scale, BLACK_LOD_DISTANCE) <= 100.0f);
        CHECK(black_lod_value(400.0f * scale * scale, BLACK_LOD_SQUARED_DISTANCE) <= 400.0f);
        CHECK(black_lod_value(0.05f / scale, BLACK_LOD_SCREEN_RATIO) >= 0.04999999f);
        CHECK(black_lod_value(80.0f * scale, BLACK_LOD_DISTANCE) == 80.0f);
    }
}

static void environment(void)
{
    static const int rounding[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    unsigned i, mode;
    for (i = 0; i < sizeof(rounding) / sizeof(rounding[0]); ++i) {
        for (mode = 0; mode < 3u; ++mode) {
            unsigned csr;
            int exceptions;
            fesetenv(FE_DFL_ENV);
            fesetround(rounding[i]);
            feraiseexcept(FE_INEXACT);
            csr = _mm_getcsr();
            exceptions = fetestexcept(FE_ALL_EXCEPT);
            chosen.lod_distance = 300u;
            CHECK(isfinite(black_lod_value(123.25f, mode)));
            CHECK(_mm_getcsr() == csr);
            CHECK(fegetround() == rounding[i]);
            CHECK(fetestexcept(FE_ALL_EXCEPT) == exceptions);
        }
    }
    fesetenv(FE_DFL_ENV);
}

int main(void)
{
    g_xbox_mem_offset = (intptr_t)ram;
    identity();
    distances();
    environment();
    /* Invalid trace objects are rejected before dereferencing guest data. */
    black_lod_trace(0x001C8360u, 0u);
    printf("LOD comparison helper: %u checks passed\n", checks);
    return 0;
}
