/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fenv.h>
#include <xmmintrin.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xbox/xboxrecomp.h>
#include "pc_video.h"
#include "pc_video_menu.h"
#include "pc_movie_skip.h"
#include "pc_menu.h"
#include "black_menu_prompt_language.h"
#include "black_release.h"

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_ebx;
extern RECOMP_TLS uint32_t g_esi, g_edi, g_esp;
extern ptrdiff_t g_xbox_mem_offset;
/* Guest code span, set by the runtime from the XBE section table. */
extern uint32_t g_xbox_code_lo, g_xbox_code_hi;

/* Generated dispatch (recomp_dispatch.c). */
extern recomp_func_t recomp_lookup(uint32_t xbox_va);

/* ── Indirect-call tracing (diagnostic) ────────────────────── */

/*
 * RECOMP_TRACE_ICALL=va,va,... logs every indirect call to those guest
 * functions: the guest return address, the first four stack arguments and
 * ecx on entry, eax on return. It exists for callback tables -- BLACK's
 * engine init (sub_00090150) runs eleven class-registration callbacks
 * through `call [ebx+esi*4]` and silently rolls all of them back if any one
 * returns 0 -- where the question is which callee said no.
 *
 * Guest behaviour is unchanged: each wrapper calls the same generated
 * function recomp_lookup would have returned. Direct calls never pass
 * through here; tools.recomp --trace-functions covers those. Unset, the only
 * cost is one comparison per indirect call.
 */
#define ITRACE_SLOTS 16
static uint32_t g_itrace_va[ITRACE_SLOTS];
static recomp_func_t g_itrace_fn[ITRACE_SLOTS];
static int g_itrace_n = -1;

static void itrace_init(void)
{
    const char *env = getenv("RECOMP_TRACE_ICALL");
    int n = 0;

    while (env && *env && n < ITRACE_SLOTS) {
        char *next;
        uint32_t va = (uint32_t)strtoul(env, &next, 16);
        if (next == env)
            break;
        g_itrace_fn[n] = recomp_lookup(va);
        if (g_itrace_fn[n]) {
            g_itrace_va[n++] = va;
            fprintf(stderr, "[ITRACE] tracing indirect calls to 0x%08X\n", va);
        } else {
            fprintf(stderr, "[ITRACE] 0x%08X is not a translated function\n", va);
        }
        env = next;
        while (*env == ',' || *env == ' ')
            env++;
    }
    g_itrace_n = n;
}

static void itrace_call(int slot)
{
    uint32_t va = g_itrace_va[slot];
    uint32_t esp_in = g_esp;
    const uint32_t *sp = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + esp_in);

    fprintf(stderr, "[ITRACE] -> 0x%08X ret=%08X args=%08X %08X %08X %08X "
            "ecx=%08X esp=%08X\n", va, sp[0], sp[1], sp[2], sp[3], sp[4],
            g_ecx, esp_in);
    g_itrace_fn[slot]();
    fprintf(stderr, "[ITRACE] <- 0x%08X eax=%08X esp=%08X\n",
            va, g_eax, g_esp);
    fflush(stderr);
}

#define ITRACE_WRAPPER(i) static void itrace_##i(void) { itrace_call(i); }
ITRACE_WRAPPER(0)  ITRACE_WRAPPER(1)  ITRACE_WRAPPER(2)  ITRACE_WRAPPER(3)
ITRACE_WRAPPER(4)  ITRACE_WRAPPER(5)  ITRACE_WRAPPER(6)  ITRACE_WRAPPER(7)
ITRACE_WRAPPER(8)  ITRACE_WRAPPER(9)  ITRACE_WRAPPER(10) ITRACE_WRAPPER(11)
ITRACE_WRAPPER(12) ITRACE_WRAPPER(13) ITRACE_WRAPPER(14) ITRACE_WRAPPER(15)

static const recomp_func_t g_itrace_wrappers[ITRACE_SLOTS] = {
    itrace_0,  itrace_1,  itrace_2,  itrace_3,
    itrace_4,  itrace_5,  itrace_6,  itrace_7,
    itrace_8,  itrace_9,  itrace_10, itrace_11,
    itrace_12, itrace_13, itrace_14, itrace_15,
};

/* ── Movie skip ───────────────────────────────────────────── */

/*
 * BLACK plays every XMV movie through one pump, sub_000C4570 (esi = the
 * movie object). The object's byte at +0x62 is "ended": the pump sets it
 * when XMVDecoder_GetNextFrame (0x0023F45C) reports end of file, and the
 * load manager (0x00209DC0) then stops the decoder, closes the movie and
 * moves on -- or reopens it, for a movie loaded as looping (flag 1).
 *
 * Skipping sets the same byte, once the movie is playing: +0x48 is 0x1C only
 * after the open (0x000D3716) has shown the first frame. Before that the open
 * waits for a frame and fails without one, so it is left alone. Nothing else
 * changes: the game sees a movie that reached its end early.
 *
 *   RECOMP_BLACK_SKIP_MOVIES=1   end every movie at its first frame, except
 *                                a looping one
 *   Escape / Enter / Space, or controller A / B / Start:
 *     startup logos end on one distinct press; other non-looping movies show
 *     "Press again to skip" and require a released, second press.
 *
 * A looping movie (the front end's AK_n background, requested with flag 1
 * from 0x001299B9) is reopened at its end, so ending it early only reloads
 * it; the variable leaves those playing. The one load manager is at
 * [0x002D45F0] + 0x2026C: its first field is the movie object, +0x198 the
 * request flags.
 *
 * The automatic skip variable remains a lab convenience. Wrapping needs
 * tools.recomp --exclude-manual on this file (scripts/regen-and-build.ps1
 * passes it). The first-frame open and normal decoder teardown remain native.
 */
extern void sub_000C4570_gen(void);

static int movie_skip_all = -1;
static int black_pc_ram_span(uint32_t address, uint32_t bytes);
static void black_movie_skip_font(void);

#define GUEST32(va) (*(uint32_t *)((uintptr_t)g_xbox_mem_offset + (va)))

/* DirectSound's idle-voice completion edits the same intrusive lists as guest
 * threads at DISPATCH_LEVEL. On Xbox its busy-counter test runs on the same
 * CPU; the host ISR can otherwise pass that test while another host thread
 * starts a list edit. A captured freeze had the completion sentinel pointing
 * to a self-linked voice, so the cleanup DPC could never finish.
 *
 * Take dispatch ownership without waiting and retain the ISR's device IRQL.
 * If busy, use the native early-return ABI. The ISR still releases the FE trap;
 * the inactive voice remains linked and the APU retries it on a later frame.
 * The generated function retains native completion and acknowledgment work.
 * --exclude-manual keeps this wrapper on direct calls as well as dispatch. */
extern int xbox_IrqlTryEnterDispatch(void);
extern void xbox_IrqlLeaveDispatch(void);
extern void sub_0021C7EF_gen(void);

void sub_0021C7EF(void)
{
    if (!xbox_IrqlTryEnterDispatch()) {
        g_eax = GUEST32(g_esp + 4);
        g_esp += 8;
        return;
    }
    sub_0021C7EF_gen();
    xbox_IrqlLeaveDispatch();
}

/* Completion is a state-based work item, represented by one embedded link.
 * A voice can restart before its deferred cleanup runs. Enqueueing the same
 * link again would turn a one-node list into the captured head -> self-node
 * cycle. Retain the pending work item; the native caller still updates its
 * status, work flags and DPC notification on every completion. */
uint32_t black_audio_queue_completion(uint32_t voice)
{
    uint32_t node = voice + 0x54;
    uint32_t head = GUEST32(voice + 8) + 0x4A0;
    if (GUEST32(node) == node && GUEST32(node + 4) == node) {
        uint32_t tail = GUEST32(head + 4);
        GUEST32(node) = head;
        GUEST32(node + 4) = tail;
        GUEST32(tail) = node;
        GUEST32(head + 4) = node;
    }
    return head;
}

typedef struct BlackMovieSkipIdentity {
    uint32_t manager, movie, decoder;
    int kind;
    char stem[64];
} BlackMovieSkipIdentity;

static BlackMovieSkipIdentity movie_skip_identity;
static uint64_t movie_skip_generation;
static int movie_skip_playing;

/* sub_001F3630 stores the requested stem at manager+84 and the full path at
 * +44. Pending replacement/stop requests no longer describe the old decoder,
 * so do not expose skip input for them. Unknown layouts keep native playback. */
static int black_movie_skip_identify(uint32_t movie_va, BlackMovieSkipIdentity *out)
{
    uint32_t main, manager;
    unsigned i;
    const uint8_t *movie;
    const char *stem;
    memset(out, 0, sizeof(*out));
    if (!black_pc_ram_span(BLACK_VA(0x002D45F0u), 4u) ||
        !black_pc_ram_span(movie_va, 0x68u)) return 0;
    main = GUEST32(BLACK_VA(0x002D45F0u));
    if (!black_pc_ram_span(main, 0x20428u)) return 0;
    manager = main + 0x2026Cu;
    movie = (const uint8_t *)((uintptr_t)g_xbox_mem_offset + movie_va);
    if (GUEST32(manager) != movie_va || GUEST32(manager + 0x1A8u) != 0x1Cu ||
        GUEST32(movie_va + 0x48u) != 0x1Cu || movie[0x61] || movie[0x62] ||
        *(const uint8_t *)((uintptr_t)g_xbox_mem_offset + manager + 0x1B8u) ||
        *(const uint8_t *)((uintptr_t)g_xbox_mem_offset + manager + 0x1B9u) ||
        (GUEST32(manager + 0x198u) & 1u)) return 0;
    stem = (const char *)((uintptr_t)g_xbox_mem_offset + manager + 0x84u);
    for (i = 0; i < sizeof(out->stem); ++i) {
        out->stem[i] = stem[i];
        if (!stem[i]) break;
    }
    if (i == sizeof(out->stem) || !out->stem[0]) return 0;
    out->manager = manager;
    out->movie = movie_va;
    out->decoder = GUEST32(movie_va + 0x4Cu);
    if (!out->decoder) return 0;
    /* The installed LO_n_US.xmv contains both EA and Criterion startup logos.
     * C_N_Us.xmv is the end credits; every other stem keeps confirmation. */
    out->kind = !_stricmp(out->stem, "LO_n_US") ? PC_MOVIE_SKIP_INTRO : PC_MOVIE_SKIP_CUTSCENE;
    return 1;
}

static uint64_t black_movie_skip_sync(uint32_t movie_va)
{
    BlackMovieSkipIdentity identity;
    if (!black_movie_skip_identify(movie_va, &identity)) {
        movie_skip_playing = 0;
        memset(&movie_skip_identity, 0, sizeof(movie_skip_identity));
        pc_movie_skip_clear();
        return 0;
    }
    if (!movie_skip_playing || memcmp(&identity, &movie_skip_identity, sizeof(identity))) {
        if (!++movie_skip_generation) ++movie_skip_generation;
        movie_skip_identity = identity;
        fprintf(stderr, "[MOVIE] playing stem %s kind %s movie 0x%08X decoder 0x%08X session %llu\n",
                identity.stem, identity.kind == PC_MOVIE_SKIP_INTRO ? "intro" : "cutscene",
                identity.movie, identity.decoder, (unsigned long long)movie_skip_generation);
    }
    movie_skip_playing = 1;
    if (identity.kind == PC_MOVIE_SKIP_CUTSCENE) black_movie_skip_font();
    pc_movie_skip_update(movie_skip_generation, identity.kind, 1);
    pc_movie_skip_subtitle_target(movie_skip_generation, identity.stem);
    return movie_skip_generation;
}

void sub_000C4570(void)
{
    const uint32_t movie_va = g_esi;
    uint64_t session;
    int by_input, by_environment;
    unsigned csr = _mm_getcsr();
    fenv_t environment;
    fegetenv(&environment);
    fesetenv(FE_DFL_ENV);
    _mm_setcsr(0x1F80u);

    if (movie_skip_all < 0) {
        const char *env = getenv("RECOMP_BLACK_SKIP_MOVIES");
        movie_skip_all = env && *env && *env != '0';
        if (movie_skip_all)
            fprintf(stderr, "[MOVIE] RECOMP_BLACK_SKIP_MOVIES: every movie "
                            "but a looping one ends at its first frame\n");
    }
    session = black_movie_skip_sync(movie_va);
    by_input = session && pc_movie_skip_take_request(session);
    by_environment = session && movie_skip_all;
    if (by_input || by_environment) {
        uint8_t *movie = (uint8_t *)((uintptr_t)g_xbox_mem_offset + movie_va);
        movie[0x62] = 1;
        fprintf(stderr, "[MOVIE] skipped (%s): movie 0x%08X decoder 0x%08X "
                "stem %s caller 0x%08X\n", by_input ? "player" : "RECOMP_BLACK_SKIP_MOVIES",
                movie_va, *(uint32_t *)(movie + 0x4C), movie_skip_identity.stem,
                black_pc_ram_span(g_esp, 4u) ? GUEST32(g_esp) : 0u);
        fflush(stderr);
        black_movie_skip_sync(movie_va);
        fesetenv(&environment);
        _mm_setcsr(csr);
        g_esp += 4;     /* ret: the pump has no stack arguments */
        return;
    }
    fesetenv(&environment);
    _mm_setcsr(csr);
    sub_000C4570_gen();
    /* Keep EOF/open/decoder replacement separate from a pressed key; ending a
     * movie naturally never leaves confirmation armed for the next one. */
    csr = _mm_getcsr();
    fegetenv(&environment);
    fesetenv(FE_DFL_ENV);
    _mm_setcsr(0x1F80u);
    black_movie_skip_sync(movie_va);
    fesetenv(&environment);
    _mm_setcsr(csr);
}

/* Optional BLACK presentation interval experiment.
 *
 * BLACK initializes its presentation interval to adaptive TWO (0x80000002)
 * at guest instruction 0x001783ED. D3D copies PresentParameters+0x30 to
 * 0x0027C204 at 0x0027297F; its software flip handler 0x002740F0 then adds
 * two to the previous target vblank. The vblank clock remains 60 Hz.
 *
 * This opt-in experiment maps only BLACK's original adaptive TWO value to
 * adaptive ONE at the game's Present callback (registered by 0x0002C110).
 * Keep the cached presentation field in agreement with the active interval;
 * the generated callback still owns immediate/vsync requests and all fences.
 * This changes only presentation; BLACK also has a fixed simulation step, so
 * gameplay speed with this experiment is unverified. Unset, the generated
 * callback runs unchanged. --exclude-manual recognizes the declaration below,
 * so regeneration preserves the wrapper and emits the original body as _gen.
 */
extern void sub_0002BA70_gen(void);

static int black_60fps = -1;

/* ── Coupled 60 Hz simulation (RECOMP_BLACK_60HZ=1) ───────────────────────
 *
 * BLACK advances its world by a fixed step per main-loop iteration: the
 * global float seconds-per-tick at 0x002F3A38 (1/30 in NTSC mode, written at
 * 0x0020C2BE) is cached into the core and main timers' per-update deltas when
 * they are reset, and every update integrates with those. With presentation
 * interval ONE the loop runs at 60 iterations a second, so halving the step
 * keeps the world at real time. See reports/perf-codex-60fps-20260929/
 * engine-timing.md for the audit this follows. Three things need care:
 *   - the step is set once, at graphics init (0x00178350), after startup wrote
 *     1/30 and before any timer is reset. The shared constant 1/30 at
 *     0x00295FC0 is also a spatial range and stays unchanged;
 *   - the render oscillator (0x0016EA90) derives a shader phase from the raw
 *     tick times 0.01: at twice the tick rate it needs 0.005. Only that one
 *     site reads black_osc_factor() instead of the shared constant;
 *   - the angular control filter (0x001BD3F0) moves a fraction k of the error
 *     each update, with no time factor: for the same response at twice the
 *     rate that fraction is 1 - sqrt(1 - k). Its two multiplications read
 *     black_ctl_k(k). Spatial falloffs and range constants are untouched. */
/* RECOMP_BLACK_HZ=N (60, 120 or 240) runs the same coupled clock at N steps a second: the step is 1/N, the
 * oscillator factor is 0.01 * 30 / N and the angular filter moves the fraction 1 - (1 - k)^(30 / N) per
 * update (N = 60 gives 1 - sqrt(1 - k)). The emulated vblank runs at N Hz too (xbox_kernel_set_vblank_hz);
 * values that count vblanks for the game's 60 Hz clocks are scaled back (see black_xmv_field.inc).
 * RECOMP_BLACK_60HZ=1 is RECOMP_BLACK_HZ=60. */
static int black_60hz = -1;
static int black_step_halved;
static unsigned black_hz = 30;
static unsigned black_vblank_hz_v = 60;
extern void sub_00178350_gen(void);
extern void xbox_kernel_set_vblank_hz(unsigned hz);

static int black_hz60_enabled(void)
{
    if (black_60hz < 0) {
        const char *env = getenv("RECOMP_BLACK_HZ");
        unsigned n = env && *env ? (unsigned)atoi(env) : 0;
        if (n != 60 && n != 120 && n != 240) {
            env = getenv("RECOMP_BLACK_60HZ");
            n = env && *env && *env != '0' ? 60 : 0;
        }
        black_60hz = n != 0;
        if (n) black_hz = n;
        if (black_60hz)
            fprintf(stderr, "[BLACK] RECOMP_BLACK_HZ: simulation step 1/30 -> 1/%u with "
                            "presentation interval ONE\n", black_hz);
    }
    return black_60hz;
}

void sub_00178350(void)
{
    if (black_hz60_enabled() && *(volatile uint8_t *)((uintptr_t)g_xbox_mem_offset + BLACK_VA(0x002D198Cu)) == 0 &&
        GUEST32(BLACK_VA(0x002F3A38u)) == 0x3D088889u) {
        float step = 1.0f / (float)black_hz;
        uint32_t bits;
        memcpy(&bits, &step, sizeof(bits));
        if (black_hz == 60) bits = 0x3C888889u;                /* the value earlier builds wrote */
        GUEST32(BLACK_VA(0x002F3A38u)) = bits;
        black_step_halved = 1;
        black_vblank_hz_v = black_hz;
        {
            /* RECOMP_BLACK_VBLANK_HZ=n (diagnostic): a vblank faster than the step, so frames are not locked to it and the
             * frame rate shows what the engine can do; the game then runs ahead of real time. */
            const char *e = getenv("RECOMP_BLACK_VBLANK_HZ");
            unsigned v = e && *e ? (unsigned)atoi(e) : 0;
            if (v >= 20 && v <= 1000) black_vblank_hz_v = v;
        }
        xbox_kernel_set_vblank_hz(black_vblank_hz_v);
        fprintf(stderr, "[BLACK] simulation step is now 1/%u s\n", black_hz);
    }
    sub_00178350_gen();
}

float black_osc_factor(void)
{
    uint32_t bits = black_step_halved ? 0x3C23D70Au : GUEST32(BLACK_VA(0x00295CA8u));   /* 0.01f */
    float f;
    memcpy(&f, &bits, sizeof(f));
    if (black_step_halved) f *= 30.0f / (float)black_hz;       /* a power of two: exact */
    return f;
}

float black_ctl_k(float k)
{
    if (black_step_halved && k > 0.0f && k < 1.0f) {
        if (black_hz == 60) return 1.0f - sqrtf(1.0f - k);
        return 1.0f - powf(1.0f - k, 30.0f / (float)black_hz);
    }
    return k;
}

unsigned black_sim_hz(void)
{
    return black_step_halved ? black_hz : 30;
}

/* The game's per-frame Sleep(1) (every third frame) is a yield for a single-core console; skipped at a stepped rate of
 * 120 Hz or more, where it is a tenth of a frame. RECOMP_BLACK_IDLE_YIELD=1 keeps it. */
int black_idle_yield_skip(void)
{
    static int keep = -1;
    if (keep < 0) { const char *e = getenv("RECOMP_BLACK_IDLE_YIELD"); keep = e && *e && *e != '0'; }
    return !keep && black_step_halved && black_hz >= 120;
}

/* BEGIN BLACK_WEAPON_RUNTIME_MODES */
/* Silver weapon selection is native by default; force modes affect this process only. */
int black_silver_weapons_mode(void)
{
    static int mode = -2;
    if (mode == -2) {
        const char *value = getenv("RECOMP_BLACK_SILVER_WEAPONS");
        mode = -1;
        if (value && strcmp(value, "1") == 0) mode = 1;
        else if (value && strcmp(value, "0") == 0) mode = 0;
        fprintf(stderr, "[BLACK] silver weapon runtime mode: %s\n",
                mode < 0 ? "native" : mode ? "forced on" : "forced off");
    }
    return mode;
}

/* Runtime-only infinite ammunition; this does not edit Profile data. */
int black_infinite_ammo_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("RECOMP_BLACK_INFINITE_AMMO");
        enabled = value && strcmp(value, "1") == 0;
        fprintf(stderr, "[BLACK] infinite ammunition runtime toggle: %s\n", enabled ? "on" : "off");
    }
    return enabled;
}

/* Resolve each consumer without changing the native reward byte. In particular,
 * forcing silver artwork must not grant ammunition, and replenishing ammunition
 * must not select a different weapon bank. Native rewards remain the default. */
int black_silver_weapon_flag(int native_flag)
{
    const int mode = black_silver_weapons_mode();
    return mode < 0 ? native_flag : mode;
}

int black_infinite_ammo_flag(int native_flag)
{
    return native_flag || black_infinite_ammo_enabled();
}
/* END BLACK_WEAPON_RUNTIME_MODES */

/* Lab hook: RECOMP_BLACK_LEVEL=n[,stage] starts the mission the frontend asked for from level n (and stage) instead, to
 * reach scenes a scripted run cannot walk to. Without the variable the values pass through. */
void black_level_override(uint8_t *level, uint8_t *stage)
{
    static int init;
    static int lv = -1, sg = -1;
    if (!init) {
        const char *e = getenv("RECOMP_BLACK_LEVEL");
        int a = -1, b = -1;
        init = 1;
        if (e && *e && sscanf(e, "%d,%d", &a, &b) >= 1) { lv = a; sg = b; }
        if (lv >= 0) fprintf(stderr, "[BLACK] level override: level %d stage %d\n", lv, sg);
    }
    if (lv >= 0) *level = (uint8_t)lv;
    if (sg >= 0) *stage = (uint8_t)sg;
}

/* The emulated vblank rate, for values that count vblanks against the game's 60 Hz clocks. */
unsigned black_vblank_hz(void)
{
    return black_step_halved ? black_vblank_hz_v : 60;
}

/* ---- APU registers without the fault ---------------------------------------------------------------------
 * The recompiled DirectSound code touches the APU's registers (0xFE800000..) with plain loads and stores, which
 * fault and are stepped over by the vectored handler in main.c: over a hundred exceptions a frame, each several
 * microseconds on the game's main thread. The generated code now calls these for the constant-address forms. They
 * make the call the handler makes (apu_hook_handle_mmio -> mcpx_apu_mmio_read/write, offset from 0xFE800000). With
 * no emulated APU yet they touch the address as before, which faults for the handler. */
extern MCPXAPUState *g_apu_state;

uint32_t black_apu_r32(uint32_t va)
{
    if (g_apu_state) return (uint32_t)mcpx_apu_mmio_read(g_apu_state, va - 0xFE800000u, 4);
    return *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + va);
}

void black_apu_w32(uint32_t va, uint32_t value)
{
    if (g_apu_state) { mcpx_apu_mmio_write(g_apu_state, va - 0xFE800000u, value, 4); return; }
    *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + va) = value;
}

/* The slow path of rep movsd (overlapping or device addresses) copies a dword at a time; a copy to or from the APU
 * span faults on every dword. The same loop, with that span treated as APU registers. */
void black_copy32_slow(uint32_t dst, uint32_t src, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; i++) {
        uint32_t d = dst + i * 4, s = src + i * 4, v;
        if (s - 0xFE800000u < 0x80000u) v = black_apu_r32(s);
        else v = *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + s);
        if (d - 0xFE800000u < 0x80000u) black_apu_w32(d, v);
        else *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + d) = v;
    }
}

#include "black_xmv_yuv.inc"
#include "black_xmv_idct.inc"
#include "black_xmv_prof.inc"
#include "black_xmv_field.inc"
#include "black_vtx_decode.inc"

/* Simulation clock diagnostic (RECOMP_BLACK_SIMLOG=n): every n Presents, the
 * global tick, the core timer's accumulated seconds and the wall clock. */
static void black_sim_log(void)
{
    static int every = -1;
    static unsigned count;
    static LARGE_INTEGER t0, f;
    static uint32_t tick0;
    static float sec0;
    if (every < 0) {
        const char *e = getenv("RECOMP_BLACK_SIMLOG");
        every = e ? atoi(e) : 0;
        QueryPerformanceFrequency(&f);
    }
    if (!every || ++count % (unsigned)every) return;
    {
        uint32_t timer = GUEST32(BLACK_VA(0x002D45D4u)), bits, tick = GUEST32(BLACK_VA(0x002F4368u));
        float seconds, step, dt;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (!timer) return;
        bits = GUEST32(timer + 0x20u); memcpy(&seconds, &bits, 4);
        bits = GUEST32(BLACK_VA(0x002F3A38u)); memcpy(&step, &bits, 4);
        bits = GUEST32(timer + 0x1Cu); memcpy(&dt, &bits, 4);
        if (t0.QuadPart) {
            double wall = (double)(now.QuadPart - t0.QuadPart) / (double)f.QuadPart;
            fprintf(stderr, "[SIM] tick %u (+%u in %.3f s = %.2f/s) core %.3f s (+%.3f = %.3fx real) "
                            "global step %.5f core dt %.5f\n", tick, tick - tick0, wall,
                    wall > 0 ? (tick - tick0) / wall : 0.0, seconds, seconds - sec0,
                    wall > 0 ? (seconds - sec0) / wall : 0.0, step, dt);
        }
        t0 = now; tick0 = tick; sec0 = seconds;
    }
}

extern volatile DWORD g_black_main_tid;
extern void xbox_PinThread(const char *role);

void sub_0002BA70(void)
{
    if (!g_black_main_tid) { g_black_main_tid = GetCurrentThreadId(); xbox_PinThread("MAIN"); }
    {
        /* RECOMP_FPE_TRAP=1 (lab diagnostic, with main.c's fpe_veh): unmask the invalid-operation exception again each frame. */
        static int fpe = -1;
        if (fpe < 0) fpe = getenv("RECOMP_FPE_TRAP") != NULL;
        if (fpe) _mm_setcsr(_mm_getcsr() & ~0x80u);
    }
    if (black_60fps < 0) {
        const char *env = getenv("RECOMP_BLACK_60FPS");
        black_60fps = env && *env && *env != '0';
        if (black_60fps)
            fprintf(stderr, "[BLACK] RECOMP_BLACK_60FPS: adaptive presentation "
                            "interval TWO -> ONE (simulation timing unverified)\n");
    }
    if (black_60fps || black_hz60_enabled()) {
        if (GUEST32(BLACK_VA(0x002B2A08u)) == 0x80000002u)
            GUEST32(BLACK_VA(0x002B2A08u)) = 0x80000001u;
        if (GUEST32(BLACK_VA(0x0027C204u)) == 0x80000002u)
            GUEST32(BLACK_VA(0x0027C204u)) = 0x80000001u;
    }
    black_sim_log();
    sub_0002BA70_gen();
}

/* ── PC camera aspect ─────────────────────────────────────── */

/* BEGIN BLACK_PC_ASPECT
 * BLACK stores each camera at renderer+0xD450+index*0xA0. Its setup writes
 * view-window multiplier (+0x70) and aspect (+0x74), consumed unchanged by
 * sub_000C3200. Select the game's native widescreen branch privately, then
 * widen its horizontal window for ultrawide while retaining its vertical
 * field of view. The framebuffer remains the original anamorphic 640x480.
 * No shared XBE constants, simulation state or offscreen cameras are patched.
 * --exclude-manual emits the original setup body as sub_0015A960_gen.
 */
extern void sub_0015A960_gen(void);
static uint32_t black_aspect_bits;
static uint32_t black_view_window_bits;
static unsigned char black_aspect_wide;
static volatile LONG black_aspect_wide_written;   /* the camera setup forced the widescreen flag on */

/* The camera aspect is a video setting (pc_video.c: the Video Settings page, local\video.ini, or RECOMP_BLACK_ASPECT).
 * It can change while the game runs, so every user of the three values below brings them up to date first; the
 * common case is one interlocked read. */
static void black_aspect_refresh(void)
{
    static volatile LONG applied = -1;
    const unsigned aspect = pc_video_aspect();
    unsigned int caller_csr;
    if ((LONG)aspect == applied) return;
    caller_csr = _mm_getcsr();
    switch (aspect) {
    case PCV_ASPECT_4_3:
        black_aspect_bits = 0x3FAAAAABu; black_view_window_bits = 0x3F800000u; black_aspect_wide = 0; break;
    case PCV_ASPECT_16_9:
        black_aspect_bits = 0x3FE38E39u; black_view_window_bits = 0x3F99999Au; black_aspect_wide = 1; break;
    case PCV_ASPECT_21_9:
        black_aspect_bits = 0x40155555u; black_view_window_bits = 0x3FC9999Au; black_aspect_wide = 1; break;
    case PCV_ASPECT_32_9:
        black_aspect_bits = 0x40638E39u; black_view_window_bits = 0x4019999Au; black_aspect_wide = 1; break;
    default:
        black_aspect_bits = 0; black_view_window_bits = 0; black_aspect_wide = 0; break;
    }
    applied = (LONG)aspect;
    if (black_aspect_bits)
        fprintf(stderr, "[BLACK] camera aspect %s (projection setting)\n", pc_video_aspect_name(aspect));
    else
        fprintf(stderr, "[BLACK] camera aspect original\n");
    _mm_setcsr(caller_csr);
}

static int black_pc_ram_span(uint32_t address, uint32_t bytes)
{
    const uint64_t end = (uint64_t)address + bytes;
    /* The title allocates its renderer/UI in the separate contiguous RAM
     * window. Accept only actual primary RAM or that mapped window; mirrors,
     * tiled aliases, and MMIO need separate alias proofs. Never fold a VA. */
    if (address >= 0x10000u && xbox_GetMemoryBase() != NULL &&
        end <= (uint64_t)xbox_GetMappedSize())
        return 1;
    return address >= XBOX_CONTIG_BASE &&
        end <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE &&
        xbox_GetPhysicalMemoryBase() != NULL;
}

void sub_0015A960(void)
{
    const uint32_t renderer = g_edi, index = g_eax;
    const unsigned int caller_csr = _mm_getcsr();
    black_aspect_refresh();
    _mm_setcsr(caller_csr);
    /* Only the two world views have this layout. Preserve original handling
     * for an unexpected index or pointer rather than extending the write. */
    const int configured = black_aspect_bits && index < 2u &&
        black_pc_ram_span(renderer, 0xD568u);
    if (configured) {
        *(unsigned char *)((uintptr_t)g_xbox_mem_offset + renderer + 0xCu) =
            black_aspect_wide;
        if (black_aspect_wide) black_aspect_wide_written = 1;
    } else if (black_aspect_wide_written && !black_aspect_bits && index < 2u &&
               black_pc_ram_span(renderer, 0xD568u)) {
        /* Back to the original camera: the setup has not run since the flag was forced on. */
        *(unsigned char *)((uintptr_t)g_xbox_mem_offset + renderer + 0xCu) = 0;
        if (index == 1u) black_aspect_wide_written = 0;
    }
    sub_0015A960_gen();
    if (configured) {
        const uint32_t camera = renderer + index * 0xA0u + 0xD450u;
        /* Writing IEEE bits introduces no caller-MXCSR-dependent arithmetic. */
        GUEST32(camera + 0x70u) = black_view_window_bits;
        GUEST32(camera + 0x74u) = black_aspect_bits;
    }
}
/* END BLACK_PC_ASPECT */

/* BEGIN BLACK_PC_FOV
 * The native world camera is renderer[2D67F4]+D450, bound to the main
 * camera entity[2D12B8]+740. D2F40 reads that entity's horizontal lens angle
 * (+10), then produces halfX=window70*tan(angle/2), halfY=halfX/aspect74.
 * 203894 establishes this binding; native scripted camera mode clears the
 * entity's FOV-enable byte+16A4 at201457 and restores it at201922. Only that
 * enabled player-world view receives a custom vertical FOV. UI, secondary,
 * offscreen and scripted-camera views keep their original producer.
 *
 * Native gameplay starts at70 horizontal degrees (2888C8). Aim writes70/zoom
 * at1BB2CA. Temporarily changing window70 retains exactly this native lens
 * animation and tangent-space zoom ratio; never rewrite the angle or build
 * another projection. Restore the window after the original producer once,
 * so repeated calls cannot compound. First-person draw ownership separately
 * preserves the original weapon framing, including its attached suppressor.
 */
extern void sub_000D2F40_gen(void);
extern void kgpu_set_ao_camera(float half_x, float half_y);

/* BEGIN BLACK_AO_CAMERA
 * D2F40 has just produced the actual half-frustum at +48/+4C. Publish only
 * the primary world camera, including native aim zoom and scripted lenses.
 * The renderer consumes a coherent pair; UI and offscreen cameras cannot
 * overwrite it. Preserve the native producer's returned FP environment.
 */
static void black_ao_camera_publish(uint32_t camera)
{
    fenv_t environment;
    unsigned csr;
    uint32_t renderer, hx_bits, hy_bits;
    float hx, hy;
    static int trace = -1, trace_count;
    if (!black_pc_ram_span(BLACK_VA(0x002D67F4u), 4u)) return;
    renderer = GUEST32(BLACK_VA(0x002D67F4u));
    if (!black_pc_ram_span(renderer, 0xD4F0u) ||
        camera != renderer + 0xD450u) return;
    hx_bits = GUEST32(camera + 0x48u);
    hy_bits = GUEST32(camera + 0x4Cu);
    /* Reject invalid float encodings before any host arithmetic. */
    if (!hx_bits || hx_bits >= 0x7F800000u ||
        !hy_bits || hy_bits >= 0x7F800000u) hx_bits = hy_bits = 0u;
    csr = _mm_getcsr();
    fegetenv(&environment);
    fesetenv(FE_DFL_ENV);
    _mm_setcsr(0x1F80u);
    memcpy(&hx, &hx_bits, sizeof(hx));
    memcpy(&hy, &hy_bits, sizeof(hy));
    kgpu_set_ao_camera(hx, hy);
    if (trace < 0) {
        const char *e = getenv("RECOMP_AO_CAMERA_TRACE");
        trace = e && *e && *e != '0';
    }
    if (trace && trace_count < 8) {
        fprintf(stderr, "[BLACK] AO camera=%08X halfX=%.6f halfY=%.6f\n",
                camera, (double)hx, (double)hy);
        ++trace_count;
    }
    fesetenv(&environment);
    _mm_setcsr(csr);
}
/* END BLACK_AO_CAMERA */

/* BEGIN BLACK_VIEWMODEL_OWNER
 * 1D25D0 submits the first-person mesh and its suppressor using the same
 * actor+2C8 material parameter. 176F60 preserves that parameter in its native
 * queue; 172EB0 consumes it. World actors use different parameters even when
 * their vertex program is shared. Capture this provenance anew for each queue.
 *
 * 16F610 uploads the transposed model/view/projection matrix to c10..c13.
 * Scale only the x/y rows of that temporary upload. Native depth, W, camera,
 * skeleton, attachment transforms and queued asynchronous GPU state stay intact.
 */
static __declspec(thread) uint32_t black_vm_owner, black_vm_active;
static __declspec(thread) uint32_t black_vm_scale_bits = 0x3F800000u;
static int black_vm_enabled = -1, black_vm_trace;
static unsigned black_vm_trace_count, black_vm_muzzle_trace_count;

/* Called only with the host's masked nearest environment. */
static void black_viewmodel_scale(uint32_t original_window_bits, uint32_t wide_window_bits)
{
    float original, wide, scale = 1.0f;
    if (black_vm_enabled < 0) {
        const char *off = getenv("RECOMP_BLACK_VIEWMODEL");
        const char *trace = getenv("RECOMP_BLACK_VIEWMODEL_TRACE");
        black_vm_enabled = !(off && *off == '0');
        black_vm_trace = trace && *trace && *trace != '0';
    }
    if (black_vm_enabled && original_window_bits > 0u && original_window_bits < 0x7F800000u &&
        wide_window_bits > 0u && wide_window_bits < 0x7F800000u) {
        memcpy(&original, &original_window_bits, 4);
        memcpy(&wide, &wide_window_bits, 4);
        scale = (float)((double)wide / (double)original);
        if (!(scale > 0.0f && scale < 16.0f)) scale = 1.0f;
    }
    memcpy(&black_vm_scale_bits, &scale, 4);
}

void black_viewmodel_owner(uint32_t actor)
{
    black_vm_owner = black_pc_ram_span(actor, 0x4A0u) ? actor : 0u;
}

uint32_t black_viewmodel_draw_begin(uint32_t material_parameters)
{
    const uint32_t previous = black_vm_active;
    uint32_t renderer;
    black_vm_active = 0u;
    if (!black_vm_owner || material_parameters != black_vm_owner + 0x2C8u ||
        black_vm_scale_bits == 0x3F800000u || !black_pc_ram_span(BLACK_VA(0x2D67F4u), 4u)) return previous;
    renderer = GUEST32(BLACK_VA(0x2D67F4u));
    if (black_pc_ram_span(renderer, 0xD594u) && GUEST32(renderer + 0xD590u) == renderer + 0xD450u)
        black_vm_active = 1u;
    return previous;
}

void black_viewmodel_draw_end(uint32_t previous)
{
    black_vm_active = previous;
}

static void black_viewmodel_constants_kind(uint32_t upload, int muzzle)
{
    fenv_t environment;
    unsigned csr, i;
    float scale, values[8];
    int valid = 1;
    if (!black_vm_active || !black_pc_ram_span(upload, 64u)) return;
    /* Invalid original constants remain the native renderer's responsibility. */
    for (i = 0; i < 8u; ++i)
        if ((GUEST32(upload + i * 4u) & 0x7FFFFFFFu) >= 0x7F800000u) return;
    csr = _mm_getcsr();
    fegetenv(&environment);
    fesetenv(FE_DFL_ENV);
    _mm_setcsr(0x1F80u);
    memcpy(&scale, &black_vm_scale_bits, 4);
    for (i = 0; i < 8u; ++i) {
        const uint32_t bits = GUEST32(upload + i * 4u);
        double product;
        memcpy(&values[i], &bits, 4);
        product = (double)values[i] * (double)scale;
        if (product > 3.40282346638528859812e38 || product < -3.40282346638528859812e38) valid = 0;
        values[i] = valid ? (float)product : 0.0f;
    }
    if (valid) {
        memcpy((void *)((uintptr_t)g_xbox_mem_offset + upload), values, sizeof(values));
        unsigned *count = muzzle ? &black_vm_muzzle_trace_count : &black_vm_trace_count;
        if (black_vm_trace && (*count)++ < 12u)
            fprintf(stderr, "[BLACK] first-person %sprojection owner=%08X scale=%.6f upload=%08X\n",
                    muzzle ? "muzzle " : "", black_vm_owner, (double)scale, upload);
    }
    fesetenv(&environment);
    _mm_setcsr(csr);
}

void black_viewmodel_constants(uint32_t upload)
{
    black_viewmodel_constants_kind(upload, 0);
}

/* The player's muzzle flash is drawn directly by 16B1E0 rather than through
 * the mesh queue. Its D4A50 call uploads another temporary transposed MVP at
 * c112..c115. Restrict that boundary to the exact flash caller and the native
 * player owning this frame's first-person mesh; other immediate effects keep
 * their world projection. Do not retain draw state outside this upload. */
void black_viewmodel_muzzle_constants(uint32_t upload, uint32_t caller)
{
    uint32_t player, previous;
    if (caller != BLACK_VA(0x0016B49Eu) || !black_vm_owner ||
        !black_pc_ram_span(BLACK_VA(0x2D45D4u), 4u)) return;
    player = GUEST32(BLACK_VA(0x2D45D4u));
    if (!black_pc_ram_span(player, 0x4D0u) || player + 0x30u != black_vm_owner) return;
    previous = black_viewmodel_draw_begin(black_vm_owner + 0x2C8u);
    black_viewmodel_constants_kind(upload, 1);
    black_viewmodel_draw_end(previous);
}
/* END BLACK_VIEWMODEL_OWNER */


static double black_fov_vertical, black_fov_window_per_aspect;
static volatile LONG black_fov_announced;

/* The gameplay FOV is a video setting too (pc_video.c): the Video Settings page changes it while the game runs. */
static void black_fov_refresh(void)
{
    static double applied = -1.0;
    const double degrees = pc_video_fov();
    if (degrees == applied) return;
    applied = degrees;
    if (degrees >= 35.0 && degrees <= 100.0) {
        /* Original C3200/D2F40: float(35 * float(pi/180)), FPTAN, float.
         * This nearest-rounded native70-degree tangent is0x3F3340CD. */
        const double native_tangent = 0.700207531452178955078125;
        black_fov_vertical = degrees;
        black_fov_window_per_aspect = tan(degrees *
            0.00872664625997164788461845384244) / native_tangent;
        fprintf(stderr, "[BLACK] gameplay FOV %.3f vertical degrees (native zoom retained)\n", degrees);
    } else {
        black_fov_vertical = 0.0;
        black_fov_window_per_aspect = 0.0;
        fprintf(stderr, "[BLACK] gameplay FOV original\n");
    }
}

void sub_000D2F40(void)
{
    const uint32_t camera = g_edi;
    const unsigned caller_csr = _mm_getcsr();
    fenv_t caller_environment;
    uint32_t saved_window = 0, new_window = 0;
    int adjusted = 0;
    /* Host parsing/math/logging uses a masked nearest environment, then puts
     * every caller control/status bit back before entering the guest producer.
     * No guest registers, virtual FP/MMX/SIMD state or guest stack are borrowed. */
    fegetenv(&caller_environment);
    fesetenv(FE_DFL_ENV);
    _mm_setcsr(0x1F80u);
    black_fov_refresh();
    if (black_fov_vertical && black_pc_ram_span(BLACK_VA(0x002D67F4u), 4u) &&
        black_pc_ram_span(BLACK_VA(0x002D12B8u), 4u)) {
        const uint32_t renderer = GUEST32(BLACK_VA(0x002D67F4u));
        const uint32_t owner = GUEST32(BLACK_VA(0x002D12B8u));
        if (black_pc_ram_span(renderer, 0xD4F0u) &&
            camera == renderer + 0xD450u &&
            black_pc_ram_span(owner, 0x16A5u) &&
            GUEST32(camera + 0x68u) == owner + 0x740u &&
            *(const unsigned char *)((uintptr_t)g_xbox_mem_offset + owner + 0x16A4u)) {
            const uint32_t aspect_bits = GUEST32(camera + 0x74u);
            const uint32_t angle_bits = GUEST32(owner + 0x750u);
            float aspect, window, angle;
            saved_window = GUEST32(camera + 0x70u);
            /* Integer guards reject zeros, negatives, infinities and NaNs
             * before any host FP arithmetic can consume an invalid guest. */
            if (aspect_bits > 0u && aspect_bits < 0x7F800000u &&
                angle_bits > 0u && angle_bits < 0x43340000u &&
                saved_window > 0u && saved_window < 0x7F800000u) {
                memcpy(&aspect, &aspect_bits, sizeof(aspect));
                window = (float)((double)aspect * black_fov_window_per_aspect);
                memcpy(&new_window, &window, sizeof(new_window));
                adjusted = new_window > 0u && new_window < 0x7F800000u;
                if (adjusted && InterlockedCompareExchange(&black_fov_announced, 1, 0) == 0) {
                    memcpy(&angle, &angle_bits, sizeof(angle));
                    fprintf(stderr, "[BLACK] gameplay FOV applied: view=%08X owner=%08X vertical=%.3f native lens=%.3f aspect=%.6f window=%08X->%08X\n",
                            camera, owner, black_fov_vertical, angle, aspect,
                            saved_window, new_window);
                }
            }
        }
    }
    /* Clear the compensation when the primary view returns to its native lens.
     * Secondary/UI camera production cannot replace this world-camera ratio. */
    if (black_pc_ram_span(BLACK_VA(0x002D67F4u), 4u)) {
        const uint32_t renderer = GUEST32(BLACK_VA(0x002D67F4u));
        if (black_pc_ram_span(renderer, 0xD4F0u) && camera == renderer + 0xD450u)
            black_viewmodel_scale(adjusted ? saved_window : 0u, adjusted ? new_window : 0u);
    }
    fesetenv(&caller_environment);
    _mm_setcsr(caller_csr);
    if (adjusted) GUEST32(camera + 0x70u) = new_window;
    sub_000D2F40_gen();
    if (adjusted) GUEST32(camera + 0x70u) = saved_window;
    black_ao_camera_publish(camera);
}
/* END BLACK_PC_FOV */

/* BEGIN BLACK_WEAPON_TRACE
 * Native death/drop observations only. The title's actor policy, remaining
 * ammunition, allocator and spatial registration all retain their decisions.
 * Enable with RECOMP_BLACK_WEAPON_TRACE=1; at most 256 records are written.
 * Raw float bits avoid evaluating a guest NaN while diagnosing its producer.
 */
void black_weapon_trace(uint32_t stage, uint32_t object)
{
    static int enabled = -1;
    static unsigned records;
    unsigned csr;
    fenv_t environment;
    uint32_t manager = 0;
    const uint8_t *memory = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    if (enabled == 0 || records >= 256) return;
    csr = _mm_getcsr();
    fegetenv(&environment);
    fesetenv(FE_DFL_ENV);
    _mm_setcsr(0x1F80u);
    if (enabled < 0) {
        const char *value = getenv("RECOMP_BLACK_WEAPON_TRACE");
        enabled = value && *value && *value != '0';
    }
    if (!enabled) goto done;
    ++records;
    if (black_pc_ram_span(BLACK_VA(0x002D0D64u), 4u)) manager = GUEST32(BLACK_VA(0x002D0D64u));
    fprintf(stderr, "[WEAPON] n=%u stage=%u object=%08X", records, stage, object);
    if ((stage == 1u || stage == 2u) && black_pc_ram_span(object, 0x3B0u)) {
        uint32_t weapon = GUEST32(object + 0x29Cu), slots = GUEST32(object + 0x258u);
        uint32_t current = 0, index = 0, entries[3] = {0, 0, 0};
        unsigned slots_valid = black_pc_ram_span(slots, 0x1Bu), slot_flags = 0, count = 0;
        if (slots_valid) {
            entries[0] = GUEST32(slots + 0xCu);
            entries[1] = GUEST32(slots + 0x10u);
            entries[2] = GUEST32(slots + 0x14u);
            index = memory[slots + 0x19u];
            slot_flags = memory[slots + 0x18u];
            count = memory[slots + 0x1Au];
            if (index < 3u) current = entries[index];
        }
        fprintf(stderr, " type=%u policy=%u death=%u disarm=%u dropped=%u ended=%u weapon=%08X id=%u model=%08X slot=%u",
                GUEST32(object + 0xC4u), memory[object + 0x2C4u],
                memory[object + 0x3ADu], memory[object + 0x3ACu],
                memory[object + 0x3AEu], memory[object + 0x3AFu], weapon,
                black_pc_ram_span(weapon, 1u) ? memory[weapon] : 255u, current, index);
        fprintf(stderr, " slots=%08X slots_valid=%u entries=%08X,%08X,%08X slot_flags=%u count=%u render=%u asset=%08X xyz=%08X,%08X,%08X",
                slots, slots_valid, entries[0], entries[1], entries[2], slot_flags, count,
                memory[object + 0x3A5u], GUEST32(object + 0x320u),
                GUEST32(object + 0xA0u), GUEST32(object + 0xA4u), GUEST32(object + 0xA8u));
    }
    if (stage == 4u && black_pc_ram_span(object, 0x160u)) {
        fprintf(stderr, " type=%u subtype=%u desc=%u flags=%u disabled=%u xyz=%08X,%08X,%08X",
                GUEST32(object + 0xC4u), GUEST32(object + 0x140u), memory[object + 0x148u],
                memory[object + 0x152u], memory[object + 0x154u],
                GUEST32(object + 0xA0u), GUEST32(object + 0xA4u), GUEST32(object + 0xA8u));
    }
    if (black_pc_ram_span(manager, 0x5851u)) {
        unsigned occupied = 0, disabled = 0, i;
        for (i = 0; i < 64u; ++i) {
            occupied += memory[manager + 0x5804u + i] != 0;
            disabled += memory[manager + i * 0x160u + 0x154u] != 0;
        }
        fprintf(stderr, " pool=%u disabled_slots=%u selected=%08X", occupied, disabled,
                GUEST32(manager + 0x5848u));
    }
    fputc('\n', stderr);
done:
    fesetenv(&environment);
    _mm_setcsr(csr);
}
/* END BLACK_WEAPON_TRACE */

/* BEGIN BLACK_WEAPON_MODEL_TRACE
 * Native AI gun-model lookup/assignment observations, never a replacement.
 * Stage 1 is the asset resolver result; stage 2 follows its native slot write.
 * Compact names are printed as raw 64-bit tokens for offline decoding.
 */
void black_weapon_model_trace(uint32_t stage, uint32_t actor,
    uint32_t name_low, uint32_t name_high, uint32_t model_low,
    uint32_t model_high, uint32_t asset)
{
    static int enabled = -1;
    static unsigned records;
    unsigned csr, actor_valid, slots_valid = 0;
    fenv_t environment;
    uint32_t slots = 0, entry0 = 0, current = 0, index = 255, count = 0;
    const uint8_t *memory = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    if (enabled == 0 || records >= 128u) return;
    csr = _mm_getcsr();
    fegetenv(&environment);
    fesetenv(FE_DFL_ENV);
    _mm_setcsr(0x1F80u);
    if (enabled < 0) {
        const char *value = getenv("RECOMP_BLACK_WEAPON_TRACE");
        enabled = value && *value && *value != '0';
    }
    if (!enabled) goto done;
    actor_valid = black_pc_ram_span(actor, 0x3B0u);
    if (actor_valid) {
        slots = GUEST32(actor + 0x258u);
        slots_valid = black_pc_ram_span(slots, 0x1Bu);
        if (slots_valid) {
            entry0 = GUEST32(slots + 0xCu);
            index = memory[slots + 0x19u];
            count = memory[slots + 0x1Au];
            if (index < 3u) current = GUEST32(slots + 0xCu + index * 4u);
        }
    }
    fprintf(stderr, "[WEAPON_MODEL] n=%u stage=%u actor=%08X actor_valid=%u name=%08X%08X model_name=%08X%08X asset=%08X asset_valid=%u slots=%08X slots_valid=%u entry0=%08X slot=%u current=%08X count=%u render=%u\n",
        ++records, stage, actor, actor_valid, name_high, name_low, model_high,
        model_low, asset, black_pc_ram_span(asset, 0x6Cu), slots, slots_valid,
        entry0, index, current, count, actor_valid ? memory[actor + 0x3A5u] : 0u);
done:
    fesetenv(&environment);
    _mm_setcsr(csr);
}
/* END BLACK_WEAPON_MODEL_TRACE */

/* BEGIN BLACK_WEAPON_RENDER_TRACE
 * Read-only provenance for the separate native NPC attachment and player
 * flash paths. Idle frames cannot consume the log budget: emit only changed
 * snapshots, with at most 128 attachment and 64 flash records. Raw bits keep
 * invalid guest float encodings out of the host FP environment. */
static void black_weapon_attachment_snapshot(uint32_t stage, uint32_t actor,
    uint32_t values[12])
{
    const uint8_t *memory = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t weapon = 0, model = 0;
    memset(values, 0, 12u * sizeof(*values));
    values[0] = stage;
    values[1] = actor;
    values[2] = black_pc_ram_span(actor, 0x3B0u);
    if (values[2]) {
        values[3] = GUEST32(actor + 0xC4u);
        weapon = values[4] = GUEST32(actor + 0x29Cu);
        values[5] = black_pc_ram_span(weapon, 0x109u);
        if (values[5]) {
            values[6] = memory[weapon];
            values[7] = memory[weapon + 0x108u];
        }
        values[8] = memory[actor + 0x3A5u];
        /* 1C5490 draws this NPC attachment model, independently of the
         * player's 1D25D0 attachment slot at +26C. Record the correct path. */
        model = values[9] = GUEST32(actor + (stage == 1u ? 0x26Cu : 0x35Cu));
        values[10] = black_pc_ram_span(model, stage == 1u ? 0x1Bu : 0x6Cu);
        if (values[10]) values[11] = stage == 1u ? memory[model + 0x1Au] : GUEST32(model + 0x24u);
    }
}

void black_weapon_attachment_trace(uint32_t stage, uint32_t actor)
{
    static int enabled = -1;
    static unsigned records;
    static uint32_t previous[128][12];
    uint32_t values[12];
    unsigned i, csr;
    fenv_t environment;
    if (enabled == 0 || records >= 128u) return;
    csr = _mm_getcsr();
    fegetenv(&environment);
    fesetenv(FE_DFL_ENV);
    _mm_setcsr(0x1F80u);
    if (enabled < 0) {
        const char *value = getenv("RECOMP_BLACK_WEAPON_TRACE");
        enabled = value && *value && *value != '0';
    }
    if (!enabled) goto done;
    black_weapon_attachment_snapshot(stage, actor, values);
    for (i = 0; i < 128u; ++i)
        if (previous[i][0] == stage && previous[i][1] == actor) break;
    if (i == 128u) {
        for (i = 0; i < 128u && previous[i][0]; ++i) {}
        if (i == 128u) goto done;
    } else if (!memcmp(previous[i], values, sizeof(values))) goto done;
    memcpy(previous[i], values, sizeof(values));
    fprintf(stderr, "[WEAPON_ATTACHMENT] n=%u stage=%u actor=%08X valid=%u type=%u weapon=%08X weapon_valid=%u id=%u suppressed=%u render=%u model=%08X model_valid=%u count=%u\n",
        ++records, values[0], values[1], values[2], values[3], values[4], values[5],
        values[6], values[7], values[8], values[9], values[10], values[11]);
done:
    fesetenv(&environment);
    _mm_setcsr(csr);
}

static void black_viewmodel_muzzle_snapshot(uint32_t effect, uint32_t values[10])
{
    const uint8_t *memory = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t entry = 0, settings = 0;
    memset(values, 0, 10u * sizeof(*values));
    values[0] = effect;
    values[1] = black_pc_ram_span(effect, 0xCAu);
    if (values[1]) {
        values[2] = memory[effect + 0xC9u];
        values[3] = memory[effect + 0xC8u];
        entry = values[4] = GUEST32(effect + 0xC4u);
        if (black_pc_ram_span(entry, 0x14u)) settings = GUEST32(entry + 0x10u);
        values[5] = settings;
        if (black_pc_ram_span(settings, 0x4Cu)) {
            values[6] = GUEST32(settings + 0x20u);
            if (values[3] < 3u) values[7] = GUEST32(settings + 0x20u + 4u * values[3]);
            if (values[2] < 3u) {
                values[8] = GUEST32(settings + 0x30u + 4u * values[2]);
                values[9] = GUEST32(settings + 0x3Cu + 4u * values[2]);
            }
        }
    }
}

void black_viewmodel_muzzle_gate(uint32_t effect)
{
    static int enabled = -1;
    static unsigned records;
    static uint32_t previous[10];
    uint32_t values[10];
    unsigned csr;
    fenv_t environment;
    if (enabled == 0 || records >= 64u) return;
    csr = _mm_getcsr();
    fegetenv(&environment);
    fesetenv(FE_DFL_ENV);
    _mm_setcsr(0x1F80u);
    if (enabled < 0) {
        const char *value = getenv("RECOMP_BLACK_VIEWMODEL_TRACE");
        enabled = value && *value && *value != '0';
    }
    if (!enabled) goto done;
    black_viewmodel_muzzle_snapshot(effect, values);
    if (records && !memcmp(previous, values, sizeof(values))) goto done;
    memcpy(previous, values, sizeof(values));
    fprintf(stderr, "[MUZZLE_GATE] n=%u effect=%08X valid=%u frame=%u variant=%u entry=%08X settings=%08X texture0=%08X selected_texture=%08X opacity=%08X size_bits=%08X\n",
        ++records, values[0], values[1], values[2], values[3], values[4], values[5],
        values[6], values[7], values[8], values[9]);
done:
    fesetenv(&environment);
    _mm_setcsr(csr);
}
/* END BLACK_WEAPON_RENDER_TRACE */

/* BEGIN BLACK_TEST_WEAPON_LOADOUT
 * Explicit offline test control only. Native 1B7800 initializes the player's
 * loadout, then 1DEC70 loads and constructs its ordinary weapon/animations.
 * Select an existing SVD table token before those native steps; never patch a
 * live descriptor or pretend that this was a natural pickup. */
static int black_test_weapon_loadout_valid(uint32_t record)
{
    const uint8_t *memory = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t core, inventory, header, entries, count, i, matches = 0;
    if (!black_pc_ram_span(BLACK_VA(0x2D45D4u), 4u) || !black_pc_ram_span(BLACK_VA(0x2D12D0u), 4u)) return 0;
    core = GUEST32(BLACK_VA(0x2D45D4u));
    if (!black_pc_ram_span(core, 0x5CB2u) || record != core + 0x3F0u ||
        !black_pc_ram_span(record, 0x50u) || memory[core + 0x5CB1u] ||
        (memory[core + 0x5AECu] != 5u && memory[core + 0x5AECu] != 6u)) return 0;
    inventory = GUEST32(BLACK_VA(0x2D12D0u));
    if (!black_pc_ram_span(inventory, 4u)) return 0;
    header = GUEST32(inventory);
    if (!black_pc_ram_span(header, 8u)) return 0;
    count = memory[header + 1u];
    entries = GUEST32(header + 4u);
    /* Native IDs are signed bytes, and -1 is the not-found sentinel. */
    if (!count || count > 127u || !black_pc_ram_span(entries, count * 32u)) return 0;
    for (i = 0; i < count; ++i) {
        const uint32_t entry = entries + i * 32u;
        if (GUEST32(entry) == 0xB8230000u && GUEST32(entry + 4u) == 0x54461524u) {
            const uint32_t config = GUEST32(entry + 8u);
            /* 1B7800 uses clip count at +60 and ammo category at +64. */
            if (!black_pc_ram_span(config, 0x65u) || !GUEST32(config + 0x60u) ||
                GUEST32(config + 0x60u) > 65535u || memory[config + 0x64u] >= 10u) return 0;
            ++matches;
        }
    }
    return matches == 1u;
}

void black_test_weapon_loadout(uint32_t record)
{
    static int enabled = -1;
    static unsigned records;
    unsigned csr;
    fenv_t environment;
    if (enabled == 0) return;
    csr = _mm_getcsr();
    fegetenv(&environment);
    fesetenv(FE_DFL_ENV);
    _mm_setcsr(0x1F80u);
    if (enabled < 0) {
        const char *value = getenv("RECOMP_BLACK_TEST_WEAPON");
        enabled = value && !strcmp(value, "svd");
    }
    if (enabled) {
        const int valid = black_test_weapon_loadout_valid(record);
        if (valid) {
            GUEST32(record) = 0xB8230000u;
            GUEST32(record + 4u) = 0x54461524u;
        }
        if (records++ < 8u)
            fprintf(stderr, "[BLACK_TEST_WEAPON] record=%08X accepted=%d token=54461524B8230000 native_initialization=1\n",
                    record, valid);
    }
    fesetenv(&environment);
    _mm_setcsr(csr);
}
/* END BLACK_TEST_WEAPON_LOADOUT */

/* BEGIN BLACK_TEST_STREAM_UNIT
 * Explicit offline checkpoint control. Select a unit from the loaded mission's
 * own bounded table before native streaming chooses its two initial units. */
static int black_test_stream_unit_valid(uint32_t core, unsigned unit)
{
    const uint8_t *memory = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t header;
    unsigned count;
    if (!black_pc_ram_span(BLACK_VA(0x2D45D4u), 4u) || core != GUEST32(BLACK_VA(0x2D45D4u)) ||
        !black_pc_ram_span(core, 0x5AF8u) ||
        (memory[core + 0x5AECu] != 5u && memory[core + 0x5AECu] != 6u) ||
        memory[core + 0x5AEDu] != 1u) return 0;
    header = GUEST32(core + 0x5AF4u);
    if (!black_pc_ram_span(header, 0x13u)) return 0;
    count = memory[header + 0x11u];
    return count && count <= 127u && unit && unit <= count;
}

void black_test_stream_unit(uint32_t core)
{
    static int initialized;
    static unsigned unit, records;
    unsigned csr = _mm_getcsr();
    fenv_t environment;
    fegetenv(&environment);
    fesetenv(FE_DFL_ENV);
    _mm_setcsr(0x1F80u);
    if (!initialized) {
        const char *value = getenv("RECOMP_BLACK_TEST_UNIT");
        unsigned parsed = 0;
        initialized = 1;
        if (value && *value) {
            const char *p = value;
            while (*p >= '0' && *p <= '9' && parsed <= 127u)
                parsed = parsed * 10u + (unsigned)(*p++ - '0');
            if (!*p && parsed && parsed <= 127u) unit = parsed;
        }
    }
    if (unit) {
        int valid = black_test_stream_unit_valid(core, unit);
        /* Native streaming uses the literal StUnit%02d.bin identifier. */
        if (valid) GUEST32(core + 0x5AF0u) = unit;
        if (records++ < 8u)
            fprintf(stderr, "[BLACK_TEST_UNIT] core=%08X unit=%u accepted=%d native_streaming=1\n",
                    core, unit, valid);
    }
    fesetenv(&environment);
    _mm_setcsr(csr);
}
/* END BLACK_TEST_STREAM_UNIT */


/* BEGIN BLACK_PC_MOTION_BLUR
 * The original 0x168A30 producer updates camera history before 0x173C30
 * tests strength (+0x20) and draws the camera-motion blur. Keep that producer
 * intact; Off takes the existing no-blur branch without skipping history or
 * touching the separate spatial glow/tint passes. --exclude-manual preserves
 * the generated producer as sub_00168A30_gen.
 */
extern void sub_00168A30_gen(void);
static int black_motion_blur_off;

static void black_motion_blur_refresh(void)
{
    static int applied = -1;
    const int off = pc_video_motion_blur_off();
    if (off == applied) return;
    applied = off;
    black_motion_blur_off = off;
    fprintf(stderr, "[BLACK] motion blur %s (original guest effect)\n", off ? "off" : "original");
}

void sub_00168A30(void)
{
    const uint32_t effect = g_esi;
    const unsigned int caller_csr = _mm_getcsr();
    black_motion_blur_refresh();
    /* Native environment/logging/initialization must not change the guest
     * producer's controls or sticky flags, including the first call. */
    _mm_setcsr(caller_csr);
    sub_00168A30_gen();
    if (black_motion_blur_off)
        GUEST32(effect + 0x20u) = 0u;
}
/* END BLACK_PC_MOTION_BLUR */

/* BEGIN BLACK_PC_DEPTH_OF_FIELD
 * The retail action blur (reload and aim) is a separate full-screen
 * composite at 0x1772E0, after its producer (0x15E260) and spatial glow
 * preparation. It has its own setting, Action Blur (Original/Off, video.ini
 * action_blur), independent of the camera-motion blur and of the GPU DoF
 * setting, which only controls the added depth-buffer pass.
 * Mode zero takes the original no-blur branch before any graphics writes.
 * Mask it only during this draw; retain the producer, tint, timers and mode
 * so live re-enabling resumes the original effect. --exclude-manual keeps
 * the unchanged generated renderer as sub_001772E0_gen.
 */
extern void sub_001772E0_gen(void);

void sub_001772E0(void)
{
    const uint32_t effect = GUEST32(g_esp + 4u);
    const uint32_t mode = GUEST32(effect + 0x34u);
    const unsigned int caller_csr = _mm_getcsr();
    PcVideoSettings video;
    pc_video_get(&video);
    _mm_setcsr(caller_csr);
    if (video.action_blur_off) GUEST32(effect + 0x34u) = 0u;
    sub_001772E0_gen();
    GUEST32(effect + 0x34u) = mode;
}
/* END BLACK_PC_DEPTH_OF_FIELD */

/* BEGIN BLACK_PC_HUD_ASPECT
 * The normal UI affine path pairs 0x115B40's X scale with 0x113E70's
 * centering offset. Native wide values are 0.75 and 106.666664. Reuse
 * BLACK_PC_ASPECT's immutable selector for proportional, centered HUDs at
 * 21:9/32:9; leave Original/4:3/16:9 and every Y field unchanged.
 * --exclude-manual keeps both generated original bodies as *_gen.
 */
extern void sub_00115B40_gen(void);
extern void sub_00113E70_gen(void);

static int black_hud_span(uint32_t address, uint32_t bytes)
{
    return black_pc_ram_span(address, bytes);
}

static int black_hud_disjoint(uint32_t a, uint32_t an,
                              uint32_t b, uint32_t bn)
{
    return (uint64_t)a + an <= b || (uint64_t)b + bn <= a;
}

static uint32_t black_hud_scale_bits(void)
{
    return black_aspect_bits == 0x40155555u ? 0x3F124925u :
           black_aspect_bits == 0x40638E39u ? 0x3EC00000u : 0u;
}

static __m128 black_hud_scalar(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return _mm_set_ss(value);
}

static uint32_t black_hud_result(__m128 value)
{
    float scalar;
    uint32_t bits;
    _mm_store_ss(&scalar, value);
    memcpy(&bits, &scalar, sizeof(bits));
    return bits;
}

void sub_00115B40(void)
{
    const uint32_t ui = g_eax, stack = g_esp;
    const unsigned int caller_csr = _mm_getcsr();
    black_aspect_refresh();
    _mm_setcsr(caller_csr);
    const uint32_t scale_bits = black_hud_scale_bits();
    const uint32_t renderer = scale_bits ? GUEST32(BLACK_VA(0x2D67F4u)) : 0u;
    const uint32_t stack_begin = stack >= 0x10040u ?
        ((stack - 4u) & ~15u) - 0x20u : 0u;
    const uint32_t stack_bytes = stack_begin ? stack + 4u - stack_begin : 0u;
    const int adjust = scale_bits && black_hud_span(ui, 0x37A0u) &&
        black_hud_span(renderer, 0xD430u) &&
        black_hud_span(stack_begin, stack_bytes) &&
        *(const unsigned char *)((uintptr_t)g_xbox_mem_offset + renderer + 0xCu) &&
        black_hud_disjoint(ui, 0x37A0u, stack_begin, stack_bytes) &&
        black_hud_disjoint(ui, 0x37A0u, renderer + 0xD428u, 8u) &&
        black_hud_disjoint(ui, 0x37A0u, renderer + 0xCu, 1u) &&
        black_hud_disjoint(ui, 0x37A0u, BLACK_VA(0x5629A4u), 4u) &&
        black_hud_disjoint(ui, 0x37A0u, BLACK_VA(0x2D67F4u), 4u) &&
        black_hud_disjoint(stack_begin, stack_bytes, renderer + 0xD428u, 8u) &&
        black_hud_disjoint(stack_begin, stack_bytes, BLACK_VA(0x5629A4u), 4u) &&
        black_hud_disjoint(stack_begin, stack_bytes, renderer + 0xCu, 1u) &&
        black_hud_disjoint(stack_begin, stack_bytes, BLACK_VA(0x2D67F4u), 4u);
    const uint32_t width = adjust ? GUEST32(renderer + 0xD428u) : 0u;
    const uint32_t virtual_width = adjust ? GUEST32(BLACK_VA(0x5629A4u)) : 0u;
    sub_00115B40_gen();
    if (adjust) {
        const unsigned int original_csr = _mm_getcsr();
        /* New policy arithmetic must not add traps or sticky flags to the
         * original guest execution. Retain its RC/FTZ/DAZ, mask exceptions,
         * then restore every returned CSR bit and leave guest XMM/x87 state. */
        _mm_setcsr(original_csr | 0x1F80u);
        __m128 x = _mm_cvtsi32_ss(_mm_setzero_ps(), (int32_t)width);
        x = _mm_div_ss(x, black_hud_scalar(virtual_width));
        x = _mm_mul_ss(x, black_hud_scalar(scale_bits));
        GUEST32(ui + 0x3710u) = black_hud_result(x);
        _mm_setcsr(original_csr);
    }
}

void sub_00113E70(void)
{
    const uint32_t stack = g_esp;
    const unsigned int caller_csr = _mm_getcsr();
    black_aspect_refresh();
    _mm_setcsr(caller_csr);
    const uint32_t scale_bits = black_hud_scale_bits();
    const uint32_t renderer = scale_bits ? GUEST32(BLACK_VA(0x2D67F4u)) : 0u;
    const uint32_t ui = scale_bits ? GUEST32(BLACK_VA(0x2D199Cu)) : 0u;
    const uint32_t input = scale_bits && black_hud_span(stack, 8u) ?
        GUEST32(stack + 4u) : 0u;
    const int adjust = scale_bits && black_hud_span(ui, 0x37A0u) &&
        black_hud_span(renderer, 0xD430u) && black_hud_span(input, 24u) &&
        stack >= 0x10004u && black_hud_span(stack - 4u, 12u) &&
        *(const unsigned char *)((uintptr_t)g_xbox_mem_offset + renderer + 0xCu) &&
        black_hud_disjoint(input, 24u, ui + 0x3740u, 24u) &&
        black_hud_disjoint(input, 24u, stack - 4u, 12u) &&
        black_hud_disjoint(ui + 0x3740u, 24u, stack - 4u, 12u) &&
        black_hud_disjoint(ui + 0x3740u, 24u, renderer + 0xCu, 1u) &&
        black_hud_disjoint(ui + 0x3740u, 24u, BLACK_VA(0x2D67F4u), 4u) &&
        black_hud_disjoint(ui + 0x3740u, 24u, BLACK_VA(0x2D199Cu), 4u) &&
        black_hud_disjoint(stack - 4u, 12u, renderer + 0xCu, 1u);
    const uint32_t input_x = adjust ? GUEST32(input + 0x10u) : 0u;
    sub_00113E70_gen();
    if (adjust) {
        const unsigned int original_csr = _mm_getcsr();
        const uint32_t offset_bits = scale_bits == 0x3F124925u ?
            0x43700000u : 0x44055555u; /* 240 or 533.333313 */
        _mm_setcsr(original_csr | 0x1F80u);
        const __m128 x = _mm_add_ss(black_hud_scalar(input_x),
                                    black_hud_scalar(offset_bits));
        GUEST32(ui + 0x3750u) = black_hud_result(x);
        _mm_setcsr(original_csr);
    }
}
/* END BLACK_PC_HUD_ASPECT */

/* BEGIN BLACK_PC_SCOPE_MASK
 * WpnScope exports only sniperhud. Its character 27 wraps leaf 25: an
 * untextured black 8x8 quad, placed twice with a tall affine transform.
 * The proportional HUD chart reaches past these finite side masks on
 * ultrawide screens. Extend only each outside edge at the leaf draw, then
 * restore the native affine matrix. The aperture and reticle never enter
 * this path. Native shape dispatch uses 0x562BD8 -> 0x121B50; the sibling
 * 0x562BA8 -> 0x121BC0 callback draws glyphs. Resource/character/geometry
 * ownership is resolved afresh;
 * no remembered guest address or asset bytes are modified.
 */
extern void sub_00121B50_gen(void);

static int black_scope_separate(uint32_t matrix, uint32_t address, uint32_t bytes)
{
    return black_hud_span(address, bytes) &&
        black_hud_disjoint(matrix, 24u, address, bytes);
}

static int black_scope_owned(uint32_t ui, uint32_t shape)
{
    const uint32_t matrix = ui + 0x3740u;
    uint32_t slot;
    if (!black_hud_span(ui, 0x37A0u) || !black_scope_separate(matrix, shape, 8u)) return 0;
    for (slot = 0u; slot < 13u; ++slot) {
        uint32_t resource, table, entry, name, block, data, constants, movie;
        uint32_t chars, leaf, pack, geometries, subshape, vertices, points;
        uint32_t index;
        resource = GUEST32(ui + 0x36B8u + slot * 4u);
        if (!black_scope_separate(matrix, resource, 16u) || GUEST32(resource + 8u) != 1u) continue;
        table = GUEST32(resource + 12u);
        if (!black_scope_separate(matrix, table, 4u)) continue;
        entry = GUEST32(table);
        if (!black_scope_separate(matrix, entry, 8u)) continue;
        name = GUEST32(entry);
        if (!black_scope_separate(matrix, name, 10u) ||
            memcmp((const void *)((uintptr_t)g_xbox_mem_offset + name), "sniperhud", 10u)) continue;
        block = GUEST32(entry + 4u);
        if (!black_scope_separate(matrix, block, 0x1Cu)) continue;
        data = GUEST32(block + 0xCu);
        constants = GUEST32(block + 0x10u);
        if (!black_scope_separate(matrix, constants, 0x18u)) continue;
        /* 0x10BB30 restores this offset after 0x1099F0 relocates the movie. */
        if ((uint64_t)data + GUEST32(constants + 0x14u) > UINT32_MAX) continue;
        movie = data + GUEST32(constants + 0x14u);
        if (!black_scope_separate(matrix, movie, 0x1Cu) ||
            GUEST32(movie) != 9u || GUEST32(movie + 0x14u) != 39u) continue;
        chars = GUEST32(movie + 0x18u);
        if (!black_scope_separate(matrix, chars, 39u * 4u) || GUEST32(chars) != movie) continue;
        leaf = GUEST32(chars + 25u * 4u);
        if (!black_scope_separate(matrix, leaf, 0x1Cu) ||
            GUEST32(leaf) != 1u || GUEST32(leaf + 4u) != movie ||
            GUEST32(leaf + 8u) || GUEST32(leaf + 0xCu) ||
            GUEST32(leaf + 0x10u) != 0x41000000u ||
            GUEST32(leaf + 0x14u) != 0x41000000u || GUEST32(leaf + 0x18u) != shape) continue;
        pack = GUEST32(block + 0x18u);
        if (!black_scope_separate(matrix, pack, 16u) ||
            GUEST32(pack) != 0x7244EA15u || GUEST32(pack + 4u) != 0x96F6A8E2u ||
            GUEST32(pack + 8u) != 10u) continue;
        geometries = GUEST32(pack + 12u);
        if (!black_scope_separate(matrix, geometries, 10u * 16u)) continue;
        for (index = 0u; index < 10u; ++index) {
            const uint32_t item = geometries + index * 16u;
            /* Compact resource name 25, as looked up by 0x112B00/0x17FC0. */
            if (!GUEST32(item) && GUEST32(item + 4u) == 0x1E449A94u &&
                GUEST32(item + 8u) == shape) break;
        }
        if (index == 10u || GUEST32(shape + 4u) != 1u) continue;
        subshape = GUEST32(shape);
        if (!black_scope_separate(matrix, subshape, 12u) ||
            GUEST32(subshape) != 0x80000000u || GUEST32(subshape + 4u)) continue;
        vertices = GUEST32(subshape + 8u);
        if (!black_scope_separate(matrix, vertices, 8u) || GUEST32(vertices + 4u) != 6u) continue;
        points = GUEST32(vertices);
        if (!black_scope_separate(matrix, points, 48u)) continue;
        /* Six vertices of the same 8x8 primitive; a different shape fails closed. */
        for (index = 0u; index < 12u; ++index) {
            const uint32_t eight = (0x0B52u >> index) & 1u;
            if (GUEST32(points + index * 4u) != (eight ? 0x41000000u : 0u)) break;
        }
        if (index == 12u) return 1;
    }
    return 0;
}

/* Opt-in evidence for a rejected live draw. Never spend the bounded budget on
 * startup/menu draws before an exact sniperhud resource is loaded. Raw words
 * are intentional: printing floats would hide relocation/rounding differences.
 */
static int black_scope_trace_enabled = -1;
static unsigned black_scope_trace_records;
static uint32_t black_scope_trace_keys[48][12];

static uint32_t black_scope_trace_word(uint32_t base, uint32_t offset)
{
    const uint64_t address = (uint64_t)base + offset;
    return address <= UINT32_MAX && black_hud_span((uint32_t)address, 4u) ?
        GUEST32((uint32_t)address) : 0u;
}

static unsigned black_scope_trace_entries, black_scope_trace_slot_records;
static unsigned black_scope_trace_tall_records;
static uint32_t black_scope_trace_last_slots[105];
static uint32_t black_scope_trace_tall_keys[64][8];

/* Independent evidence budgets precede every resource/ownership gate. The
 * first calls prove dispatch, resource changes reveal loaded header layout,
 * and tall affine tuples remain observable even when the name lookup fails.
 * None of these looser diagnostic gates authorize a mask write. */
static void black_scope_mask_trace_entry(uint32_t stack)
{
    uint32_t ui = black_scope_trace_word(BLACK_VA(0x2D199Cu), 0u), slots[105], key[8];
    unsigned i, j, changed, first, tall = 0u, csr;
    fenv_t environment;
    first = black_scope_trace_entries < 4u;
    if (first) ++black_scope_trace_entries;
    slots[0] = ui;
    for (i = 0u; i < 13u; ++i) {
        const uint32_t resource = black_scope_trace_word(ui, 0x36B8u + i * 4u);
        const uint32_t table = black_scope_trace_word(resource, 12u);
        const uint32_t entry = black_scope_trace_word(table, 0u);
        const uint32_t name = black_scope_trace_word(entry, 0u);
        slots[1u + i * 8u] = resource;
        for (j = 0u; j < 4u; ++j) slots[2u + i * 8u + j] = black_scope_trace_word(resource, j * 4u);
        slots[6u + i * 8u] = entry;
        slots[7u + i * 8u] = name;
        slots[8u + i * 8u] = black_scope_trace_word(name, 0u);
    }
    changed = black_scope_trace_slot_records < 32u &&
        (!black_scope_trace_slot_records || memcmp(slots, black_scope_trace_last_slots, sizeof(slots)));
    if (changed) {
        memcpy(black_scope_trace_last_slots, slots, sizeof(slots));
        ++black_scope_trace_slot_records;
    }
    key[0] = black_scope_trace_word(stack, 4u);
    key[1] = black_scope_trace_word(stack, 8u);
    for (i = 0u; i < 6u; ++i) key[i + 2u] = black_scope_trace_word(ui, 0x3740u + i * 4u);
    if (black_scope_trace_tall_records < 64u &&
        (key[5] & 0x7FFFFFFFu) >= 0x42000000u && (key[5] & 0x7FFFFFFFu) <= 0x43800000u) {
        for (i = 0u; i < black_scope_trace_tall_records; ++i)
            if (!memcmp(key, black_scope_trace_tall_keys[i], sizeof(key))) break;
        if (i == black_scope_trace_tall_records) {
            memcpy(black_scope_trace_tall_keys[i], key, sizeof(key));
            ++black_scope_trace_tall_records; tall = 1u;
        }
    }
    if (!first && !changed && !tall) return;
    csr = _mm_getcsr(); fegetenv(&environment);
    fesetenv(FE_DFL_ENV); _mm_setcsr(0x1F80u);
    fprintf(stderr, "[BLACK_SCOPE_ENTRY] entry=%u slotRecords=%u tallRecords=%u first=%u changed=%u tall=%u stack=%08X stackValid=%u ret=%08X shape=%08X mode=%08X ui=%08X uiValid=%u renderer=%08X chart=%08X,%08X affine=%08X,%08X,%08X,%08X,%08X,%08X\n",
        black_scope_trace_entries, black_scope_trace_slot_records, black_scope_trace_tall_records,
        first, changed, tall, stack, black_hud_span(stack, 12u), black_scope_trace_word(stack, 0u),
        key[0], key[1], ui, black_hud_span(ui, 0x37A0u), black_scope_trace_word(BLACK_VA(0x2D67F4u), 0u),
        black_scope_trace_word(BLACK_VA(0x5629A4u), 0u), black_scope_trace_word(BLACK_VA(0x5629C8u), 0u),
        key[2], key[3], key[4], key[5], key[6], key[7]);
    if (changed) for (i = 0u; i < 13u; ++i) {
        const uint32_t entry = slots[6u + i * 8u], name = slots[7u + i * 8u];
        fprintf(stderr, "[BLACK_SCOPE_SLOTS] snapshot=%u slot=%u resource=%08X words=%08X,%08X,%08X,%08X entry=%08X words=%08X,%08X nameWords=%08X,%08X,%08X,%08X\n",
            black_scope_trace_slot_records, i, slots[1u + i * 8u], slots[2u + i * 8u],
            slots[3u + i * 8u], slots[4u + i * 8u], slots[5u + i * 8u], entry,
            black_scope_trace_word(entry, 0u), black_scope_trace_word(entry, 4u),
            black_scope_trace_word(name, 0u), black_scope_trace_word(name, 4u),
            black_scope_trace_word(name, 8u), black_scope_trace_word(name, 12u));
    }
    fflush(stderr); fesetenv(&environment); _mm_setcsr(csr);
}

static void black_scope_mask_trace(uint32_t stack)
{
    uint32_t ui, renderer, slot, resource = 0u, table = 0u, entry = 0u;
    uint32_t block, data, constants, movie, chars, leaf, pack, geometries;
    uint32_t shape, subshape, vertices, points, mode, m[6], key[12], scale;
    uint32_t failed = 0u, left, right, offset;
    unsigned i, j, csr;
    uint64_t movie_address;
    fenv_t environment;
    if (black_scope_trace_enabled < 0) {
        const char *value;
        csr = _mm_getcsr(); fegetenv(&environment);
        fesetenv(FE_DFL_ENV); _mm_setcsr(0x1F80u);
        value = getenv("RECOMP_BLACK_SCOPE_MASK_TRACE");
        black_scope_trace_enabled = value && !strcmp(value, "1");
        fesetenv(&environment); _mm_setcsr(csr);
    }
    if (!black_scope_trace_enabled) return;
    black_scope_mask_trace_entry(stack);
    if (black_scope_trace_records == 48u ||
        !black_hud_span(stack, 12u)) return;
    ui = black_scope_trace_word(BLACK_VA(0x2D199Cu), 0u);
    if (!black_hud_span(ui, 0x37A0u)) return;
    for (slot = 0u; slot < 13u; ++slot) {
        uint32_t count, name, n;
        resource = GUEST32(ui + 0x36B8u + slot * 4u);
        if (!black_hud_span(resource, 16u)) continue;
        count = GUEST32(resource + 8u);
        table = GUEST32(resource + 12u);
        if (!count || count > 32u || !black_hud_span(table, count * 4u)) continue;
        for (n = 0u; n < count; ++n) {
            entry = GUEST32(table + n * 4u);
            if (!black_hud_span(entry, 8u)) continue;
            name = GUEST32(entry);
            if (black_hud_span(name, 10u) && !memcmp(
                (const void *)((uintptr_t)g_xbox_mem_offset + name), "sniperhud", 10u)) break;
        }
        if (n < count) break;
    }
    if (slot == 13u) return;
    shape = GUEST32(stack + 4u); mode = GUEST32(stack + 8u);
    for (i = 0u; i < 6u; ++i) m[i] = GUEST32(ui + 0x3740u + i * 4u);
    block = GUEST32(entry + 4u);
    data = black_scope_trace_word(block, 0xCu);
    constants = black_scope_trace_word(block, 0x10u);
    movie_address = (uint64_t)data + black_scope_trace_word(constants, 0x14u);
    movie = movie_address <= UINT32_MAX ? (uint32_t)movie_address : 0u;
    chars = black_scope_trace_word(movie, 0x18u);
    leaf = black_scope_trace_word(chars, 25u * 4u);
    /* Permit a mismatched transform or relocation, but not ordinary small HUD
     * widgets: capture the resource's expected leaf or a tall side-like draw. */
    if (shape != black_scope_trace_word(leaf, 0x18u) &&
        ((m[3] & 0x7FFFFFFFu) < 0x42000000u ||
         (m[3] & 0x7FFFFFFFu) > 0x43800000u)) return;
    key[0] = resource; key[1] = leaf; key[2] = shape; key[3] = mode;
    memcpy(key + 4u, m, sizeof(m));
    key[10] = black_scope_trace_word(BLACK_VA(0x5629A4u), 0u);
    key[11] = black_scope_trace_word(BLACK_VA(0x5629C8u), 0u);
    for (i = 0u; i < black_scope_trace_records; ++i)
        if (!memcmp(key, black_scope_trace_keys[i], sizeof(key))) return;
    memcpy(black_scope_trace_keys[black_scope_trace_records], key, sizeof(key));
    ++black_scope_trace_records;
    renderer = black_scope_trace_word(BLACK_VA(0x2D67F4u), 0u);
    pack = black_scope_trace_word(block, 0x18u);
    geometries = black_scope_trace_word(pack, 12u);
    subshape = black_scope_trace_word(shape, 0u);
    vertices = black_scope_trace_word(subshape, 8u);
    points = black_scope_trace_word(vertices, 0u);
    scale = black_hud_scale_bits();
    if (!scale) failed |= 1u;
    if (stack < 0x10020u || !black_hud_span(stack - 0x20u, 0x2Cu)) failed |= 2u;
    if (mode) failed |= 4u;
    if (!black_hud_span(renderer, 0xD430u) ||
        !black_hud_span(renderer, 13u) || !*(const unsigned char *)
        ((uintptr_t)g_xbox_mem_offset + renderer + 12u)) failed |= 8u;
    if (!black_hud_disjoint(ui + 0x3740u, 24u, stack - 0x20u, 0x2Cu)) failed |= 16u;
    if (key[10] != 0x44200000u || key[11] != 0x43F00000u) failed |= 32u;
    if (m[0] != 0x41DBFFF0u || m[1] || m[2] || m[3] != 0x427A0090u ||
        m[5] != 0xC1200000u) failed |= 64u;
    if (!black_scope_owned(ui, shape)) failed |= 128u;
    csr = _mm_getcsr(); fegetenv(&environment);
    fesetenv(FE_DFL_ENV); _mm_setcsr(0x1F80u);
    offset = scale == 0x3F124925u ? 0x43700000u : 0x44055555u;
    left = black_hud_result(_mm_add_ss(black_hud_scalar(0xC31A0000u), black_hud_scalar(offset)));
    right = black_hud_result(_mm_add_ss(black_hud_scalar(0x440EC000u), black_hud_scalar(offset)));
    if (m[4] != left && m[4] != right) failed |= 256u;
    fprintf(stderr, "[BLACK_SCOPE_MASK] record=%u reject=%03X stack=%08X ret=%08X shape=%08X mode=%08X ui=%08X renderer=%08X aspect=%08X scale=%08X chart=%08X,%08X expectedTX=%08X,%08X\n",
        black_scope_trace_records, failed, stack, GUEST32(stack), shape, mode, ui, renderer,
        black_aspect_bits, scale, key[10], key[11], left, right);
    fprintf(stderr, "[BLACK_SCOPE_MASK] affine=%08X,%08X,%08X,%08X,%08X,%08X slot=%u resource=%08X count=%08X table=%08X entry=%08X block=%08X data=%08X constants=%08X movieOffset=%08X movie=%08X chars=%08X leaf25=%08X\n",
        m[0], m[1], m[2], m[3], m[4], m[5], slot, resource,
        black_scope_trace_word(resource, 8u), table, entry, block, data, constants,
        black_scope_trace_word(constants, 0x14u), movie, chars, leaf);
    fprintf(stderr, "[BLACK_SCOPE_MASK] movieWords=");
    for (i = 0u; i < 7u; ++i) fprintf(stderr, "%s%08X", i ? "," : "", black_scope_trace_word(movie, i * 4u));
    fprintf(stderr, " leafWords=");
    for (i = 0u; i < 7u; ++i) fprintf(stderr, "%s%08X", i ? "," : "", black_scope_trace_word(leaf, i * 4u));
    fprintf(stderr, " chars0=%08X pack=%08X words=%08X,%08X,%08X,%08X geometries=%08X\n",
        black_scope_trace_word(chars, 0u), pack, black_scope_trace_word(pack, 0u),
        black_scope_trace_word(pack, 4u), black_scope_trace_word(pack, 8u),
        black_scope_trace_word(pack, 12u), geometries);
    fprintf(stderr, "[BLACK_SCOPE_MASK] shapeWords=");
    for (i = 0u; i < 8u; ++i) fprintf(stderr, "%s%08X", i ? "," : "", black_scope_trace_word(shape, i * 4u));
    fprintf(stderr, " subshape=%08X words=%08X,%08X,%08X vertices=%08X words=%08X,%08X points=%08X words=",
        subshape, black_scope_trace_word(subshape, 0u), black_scope_trace_word(subshape, 4u),
        black_scope_trace_word(subshape, 8u), vertices, black_scope_trace_word(vertices, 0u),
        black_scope_trace_word(vertices, 4u), points);
    for (i = 0u; i < 12u; ++i) fprintf(stderr, "%s%08X", i ? "," : "", black_scope_trace_word(points, i * 4u));
    fprintf(stderr, "\n[BLACK_SCOPE_MASK] geometryItems=");
    for (i = 0u; i < 10u; ++i) {
        fprintf(stderr, "%s", i ? ";" : "");
        for (j = 0u; j < 4u; ++j) fprintf(stderr, "%s%08X", j ? "," : "",
            black_scope_trace_word(geometries, i * 16u + j * 4u));
    }
    fprintf(stderr, "\n"); fflush(stderr);
    fesetenv(&environment); _mm_setcsr(csr);
}

static int black_scope_mask_begin(uint32_t stack, uint32_t *ui_out, uint32_t saved[2])
{
    const uint32_t scale = black_hud_scale_bits();
    uint32_t ui, renderer, shape, offset, left, right, x;
    unsigned int csr;
    __m128 shifted_left, shifted_right, edge, width, sx;
    if (!scale || stack < 0x10020u || !black_hud_span(stack - 0x20u, 0x2Cu) ||
        GUEST32(stack + 8u)) return 0; /* Ordinary leaf draw, never a stencil pass. */
    ui = GUEST32(BLACK_VA(0x2D199Cu));
    renderer = GUEST32(BLACK_VA(0x2D67F4u));
    shape = GUEST32(stack + 4u);
    if (!black_hud_span(ui, 0x37A0u) || !black_hud_span(renderer, 0xD430u) ||
        !*(const unsigned char *)((uintptr_t)g_xbox_mem_offset + renderer + 0xCu) ||
        !black_hud_disjoint(ui + 0x3740u, 24u, stack - 0x20u, 0x2Cu) ||
        GUEST32(BLACK_VA(0x5629A4u)) != 0x44200000u || GUEST32(BLACK_VA(0x5629C8u)) != 0x43F00000u ||
        GUEST32(ui + 0x3740u) != 0x41DBFFF0u || GUEST32(ui + 0x3744u) ||
        GUEST32(ui + 0x3748u) || GUEST32(ui + 0x374Cu) != 0x427A0090u ||
        GUEST32(ui + 0x3754u) != 0xC1200000u || !black_scope_owned(ui, shape)) return 0;
    offset = scale == 0x3F124925u ? 0x43700000u : 0x44055555u;
    csr = _mm_getcsr();
    _mm_setcsr(0x1F80u); /* Policy arithmetic: no guest traps, flags, or rounding dependence. */
    shifted_left = _mm_add_ss(black_hud_scalar(0xC31A0000u), black_hud_scalar(offset));
    shifted_right = _mm_add_ss(black_hud_scalar(0x440EC000u), black_hud_scalar(offset));
    left = black_hud_result(shifted_left);
    right = black_hud_result(shifted_right);
    x = GUEST32(ui + 0x3750u);
    if (x != left && x != right) { _mm_setcsr(csr); return 0; }
    saved[0] = GUEST32(ui + 0x3740u);
    saved[1] = x;
    sx = black_hud_scalar(saved[0]);
    if (x == left) {
        /* Keep the aperture-side edge; pad only the outside by one chart unit. */
        edge = _mm_add_ss(shifted_left, _mm_mul_ss(sx, black_hud_scalar(0x41000000u)));
        sx = _mm_div_ss(_mm_add_ss(edge, black_hud_scalar(0x3F800000u)), black_hud_scalar(0x41000000u));
        GUEST32(ui + 0x3750u) = 0xBF800000u;
    } else {
        width = _mm_add_ss(black_hud_scalar(0x44200000u),
            _mm_mul_ss(black_hud_scalar(offset), black_hud_scalar(0x40000000u)));
        sx = _mm_div_ss(_mm_sub_ss(_mm_add_ss(width, black_hud_scalar(0x3F800000u)), shifted_right),
            black_hud_scalar(0x41000000u));
    }
    GUEST32(ui + 0x3740u) = black_hud_result(sx);
    *ui_out = ui;
    _mm_setcsr(csr);
    return 1;
}

void sub_00121B50(void)
{
    uint32_t ui = 0u, saved[2];
    const unsigned int csr = _mm_getcsr();
    black_aspect_refresh();
    _mm_setcsr(csr);
    black_scope_mask_trace(g_esp);
    const int adjusted = black_scope_mask_begin(g_esp, &ui, saved);
    sub_00121B50_gen();
    if (adjusted) {
        GUEST32(ui + 0x3740u) = saved[0];
        GUEST32(ui + 0x3750u) = saved[1];
    }
}
/* END BLACK_PC_SCOPE_MASK */

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    int i;

    /* The first indirect call is on the boot thread, before any other
     * guest thread exists, so this runs once without racing. */
    if (g_itrace_n < 0)
        itrace_init();
    for (i = 0; i < g_itrace_n; i++)
        if (g_itrace_va[i] == xbox_va)
            return g_itrace_wrappers[i];
    /* This XBE thunk shares the implementation of the discovered 0x0020B3B0
     * async worker thunk. Keep the missing-file recovery callback resolvable. */
    if (xbox_va == BLACK_VA(0x0020B6D0u))
        return recomp_lookup(BLACK_VA(0x0020B3B0u));
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Capture guest state the first time each distinct target misses,
     * without changing guest execution. RECOMP_ITAIL/RECOMP_ICALL leave the
     * caller's guest return on the stack, so the code-address scan below
     * names the caller. Bounded so a wild-pointer loop cannot flood the log;
     * the per-call line above still records every occurrence. */
    {
        enum { MAX_CAPTURED = 64 };
        static uint32_t captured[MAX_CAPTURED];
        static int n_captured;
        int i, seen = 0;

        for (i = 0; i < n_captured; i++)
            if (captured[i] == va) { seen = 1; break; }
        if (!seen && n_captured < MAX_CAPTURED && g_xbox_mem_offset &&
            g_esp >= 0x10000u && g_esp < 0x03FFF000u) {
            const uint32_t *sp = (const uint32_t *)
                ((uintptr_t)g_xbox_mem_offset + g_esp);
            int shown = 0;
            captured[n_captured++] = va;
            fprintf(stderr, "  [first miss 0x%08X] guest registers: eax=%08X "
                    "ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X esp=%08X\n",
                    va, g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp);
            for (i = 0; i < 8; i++)
                fprintf(stderr, "  guest stack raw [esp+%02X]=%08X\n",
                        i * 4, sp[i]);
            for (i = 0; i < 128 && shown < 12; i++) {
                if (sp[i] > g_xbox_code_lo && sp[i] < g_xbox_code_hi) {
                    fprintf(stderr, "  guest code stack [esp+%03X]=%08X\n",
                            i * 4, sp[i]);
                    shown++;
                }
            }
        }
    }

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* Called by generated unresolved stubs only in diagnostic builds. */
void recomp_unresolved_stub_log(uint32_t target_va)
{
    enum { SLOTS = 32 };
    static SRWLOCK lock = SRWLOCK_INIT;
    static uint32_t targets[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    uint32_t caller = 0, args[4] = {0, 0, 0, 0};
    uint64_t hit_count;
    int i;

    AcquireSRWLockExclusive(&lock);
    for (i = 0; i < count; i++)
        if (targets[i] == target_va)
            break;
    if (i == count) {
        if (count == SLOTS) {
            ReleaseSRWLockExclusive(&lock);
            return;
        }
        targets[count] = target_va;
        hits[count] = 0;
        count++;
    }
    hit_count = ++hits[i];
    ReleaseSRWLockExclusive(&lock);

    if (g_xbox_mem_offset && g_esp >= 0x10000u && g_esp < 0x03FFF000u) {
        const uint32_t *sp = (const uint32_t *)
            ((uintptr_t)g_xbox_mem_offset + g_esp);
        caller = sp[0];
        for (int j = 0; j < 4; j++)
            args[j] = sp[j + 1];
    }

    /* Log the first few calls and decimal powers thereafter to bound spam
     * while retaining a useful hit-rate signal for loops. */
    {
        uint64_t n = hit_count;
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr,
            "[UNRESOLVED_STUB] target=0x%08X hits=%llu caller_ret=0x%08X "
            "eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X "
            "esp=%08X args=%08X,%08X,%08X,%08X\n",
            target_va, (unsigned long long)hit_count, caller,
            g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp,
            args[0], args[1], args[2], args[3]);
    fflush(stderr);
}


/* BEGIN BLACK_PC_NATIVE_MOVIE
 * Bounded original4:3 fullscreen movie presentation at selected camera aspect.
 * Exact original121AAF caller/decoded-buffer guards; widgets and loops remain
 * original. Uses BLACK_PC_ASPECT/shared mapped-RAM predicate above.
 */
/* SOURCE-ONLY: include beside BLACK_PC_ASPECT after independent validation.
 * Original D6C00's generated body must be retained as sub_000D6C00_gen.
 * This is a bounded original-4:3 presentation policy for the bundled 640x480
 * movies, not a codec pixel-aspect decoder. Other draws remain original.
 */
extern void sub_000D6C00_gen(void);
extern int kelvin_active(void);
extern int kelvin_mark_frame_aspect(uint32_t, uint32_t, uint32_t);
extern uint32_t xbox_PhysicalAddressOf(uint32_t);

/* The title's render thread emits the next FLIP_STALL for the frame it just
 * drew. A deferred stall is emitted at the next SetRenderTarget, before any
 * writes to that target. Transfer the aspect at that exact command word;
 * never let the asynchronous GPU sample current movie state. */
static RECOMP_TLS uint32_t black_movie_frame_device;
static RECOMP_TLS uint32_t black_movie_frame_crop;

static uint32_t black_movie_frame_capable(void)
{
    uint32_t device=GUEST32(BLACK_VA(0x27BFF8u));
    if (!kelvin_active() || !black_pc_ram_span(device,0x2480u) ||
        (GUEST32(device+8u)&0x4000u)) return 0;
    return device;
}

/* Called only beside the four original FLIP_STALL emissions in generated
 * D3D code, before the original command publication. Guest registers and
 * floating-point control stay exactly as the caller left them. */
void black_movie_tag_flip(uint32_t parameter_va)
{
    const unsigned csr=_mm_getcsr();
    uint32_t device=black_movie_frame_device, crop=black_movie_frame_crop, physical;
    black_movie_frame_device=0;
    black_movie_frame_crop=0;
    if (device && device==GUEST32(BLACK_VA(0x27BFF8u)) &&
        black_pc_ram_span(parameter_va,4u)) {
        physical=xbox_PhysicalAddressOf(parameter_va);
        if (physical<0x08000000u)
            kelvin_mark_frame_aspect(0x80000000u+physical,crop ? 20u : 4u,crop ? 11u : 3u);
    }
    _mm_setcsr(csr);
}

typedef struct BlackMovieRect {
    uint32_t address;
    uint32_t crop;
    uint32_t saved[4];
    uint32_t replacement[4];
} BlackMovieRect;

/* Reuse the actual camera/HUD mapped-span contract. Primary RAM and the
 * separate contiguous window are accepted; mirrors/tiled/MMIO are rejected.
 * The VA is never folded, so disjoint checks use real distinct mappings. */
static int black_movie_span(uint32_t address, uint32_t bytes)
{
    return black_pc_ram_span(address, bytes);
}

static int black_movie_disjoint(uint32_t a, uint32_t an,
                                uint32_t b, uint32_t bn)
{
    return (uint64_t)a + an <= b || (uint64_t)b + bn <= a;
}

static int black_movie_prepare(uint32_t aspect, BlackMovieRect *result)
{
    const uint32_t stack = g_esp, rect = g_eax;
    uint32_t main_object, ui, renderer, manager, movie, buffer, texture;
    uint32_t frame, i, left, right;
    struct { uint32_t address, bytes; } reads[15];

    if (aspect == 0x3FE38E39u) { left=0x42A00000u; right=0x440C0000u; }
    else if (aspect == 0x40155555u) { left=0x43092492u; right=0x43FB6DB7u; }
    else if (aspect == 0x40638E39u) { left=0x43480000u; right=0x43DC0000u; }
    else if (!aspect || aspect == 0x3FAAAAABu) { left=0u; right=0x44200000u; }
    else return 0;

    /* The original 121AAF call constructs these exact borrowed stack views:
     * origin=S+58, LTRB=S+60, pixel-UV=S+70, color=S+80, S=callee entry ESP.
     * Fixing only this call cannot affect DA540's movie widgets. */
    if (stack < 0x10100u || !black_movie_span(stack-0x100u,0x190u) ||
        GUEST32(stack) != BLACK_VA(0x00121AB4u) || GUEST32(stack+8u) != 1u ||
        rect != stack+0x60u || GUEST32(stack+4u) != stack+0x58u ||
        GUEST32(stack+12u) != stack+0x70u || g_ecx != stack+0x80u)
        return 0;
    if (GUEST32(stack+0x58u) != 0u || GUEST32(stack+0x5Cu) != 0u ||
        GUEST32(rect) != 0u || GUEST32(rect+4u) != 0u ||
        GUEST32(rect+8u) != 0x44200000u || GUEST32(rect+12u) != 0x43F00000u ||
        GUEST32(stack+0x70u) != 0u || GUEST32(stack+0x74u) != 0u ||
        GUEST32(stack+0x78u) != 0x44200000u ||
        GUEST32(stack+0x7Cu) != 0x43F00000u ||
        GUEST32(BLACK_VA(0x2F3710u)) != 0u || GUEST32(BLACK_VA(0x2F36D4u)) == 2u)
        return 0;

    main_object=GUEST32(BLACK_VA(0x2D45F0u)); ui=GUEST32(BLACK_VA(0x2D199Cu));
    renderer=GUEST32(BLACK_VA(0x2D67F4u));
    if (!black_movie_span(main_object,0x20408u) ||
        !black_movie_span(ui,0x391Cu) ||
        !black_movie_span(renderer,0xD430u)) return 0;
    manager=main_object+0x2026Cu;
    if (GUEST32(ui+0x3918u) != manager ||
        (GUEST32(manager+0x198u)&1u) != 0u ||
        GUEST32(renderer+0xD428u) != 640u ||
        GUEST32(renderer+0xD42Cu) != 480u) return 0;
    movie=GUEST32(manager);
    if (!black_movie_span(movie,0x68u) ||
        GUEST32(movie+0x48u) != 0x1Cu ||
        (GUEST32(movie+0x44u)&1u) != 0u) return 0;
    frame=*(const unsigned char *)((uintptr_t)g_xbox_mem_offset+movie+0x65u);
    /* Original signed +65 permits nonnegative indices; two buffers are
     * actually allocated at D36CB, so this policy supports exactly 0/1. */
    if (frame > 1u) return 0;
    buffer=movie+frame*0x20u;
    if (g_edi != buffer || GUEST32(BLACK_VA(0x56EF30u)) != buffer ||
        GUEST32(buffer+0x18u) != 640u ||
        GUEST32(buffer+0x1Cu) != 480u) return 0;
    texture=GUEST32(buffer+0x14u);
    if (!black_movie_span(texture,0x18u) ||
        (GUEST32(texture)&0x70000u) != 0x50000u) return 0;

    reads[0].address=main_object; reads[0].bytes=0x20408u;
    reads[1].address=ui; reads[1].bytes=0x391Cu;
    reads[2].address=renderer; reads[2].bytes=0xD430u;
    reads[3].address=movie; reads[3].bytes=0x68u;
    reads[4].address=texture; reads[4].bytes=0x18u;
    reads[5].address=BLACK_VA(0x2D199Cu); reads[5].bytes=4u;
    reads[6].address=BLACK_VA(0x2D45F0u); reads[6].bytes=4u;
    reads[7].address=BLACK_VA(0x2D67F4u); reads[7].bytes=4u;
    reads[8].address=BLACK_VA(0x2F3640u); reads[8].bytes=0xE4u;
    reads[9].address=BLACK_VA(0x56E600u); reads[9].bytes=0x2000u;
    reads[10].address=BLACK_VA(0x563120u); reads[10].bytes=0x1800u;
    reads[11].address=BLACK_VA(0x27CE88u); reads[11].bytes=32u;
    reads[12].address=BLACK_VA(0x279B58u); reads[12].bytes=1u;
    reads[13].address=BLACK_VA(0x28B470u); reads[13].bytes=24u;
    reads[14].address=BLACK_VA(0x295CB4u); reads[14].bytes=4u;
    for (i=0;i<15u;i++)
        if (!black_movie_disjoint(stack-0x100u,0x190u,
                                  reads[i].address,reads[i].bytes)) return 0;

    result->address=rect;
    /* The owned 02_n opening cinematic has a 640x352 picture at rows
     * 64..415 in its 640x480 source. This explicit per-frame tag removes
     * only that padding; credits, logos, widgets and other cuts retain
     * their full frame. Do not infer a crop from dark/fading pixels. */
    {
        const char *stem=(const char *)((uintptr_t)g_xbox_mem_offset+manager+0x84u);
        result->crop=stem[0]=='0' && stem[1]=='2' && stem[2]=='_' &&
            (stem[3]=='n' || stem[3]=='N') && !stem[4];
    }
    if ((!aspect || aspect == 0x3FAAAAABu) && !result->crop) return 0;
    for (i=0;i<4u;i++) result->saved[i]=GUEST32(rect+i*4u);
    result->replacement[0]=left; result->replacement[1]=0u;
#ifdef BLACK_MOVIE_MUTANT_WIDTH_AS_RIGHT
    if (aspect == 0x40155555u) right=0x43B6DB6Eu;
#endif
    result->replacement[2]=right; result->replacement[3]=0x43F00000u;
    return 1;
}

void sub_000D6C00(void)
{
    const unsigned int caller_csr=_mm_getcsr();
    BlackMovieRect owned;
    unsigned i;
    int adjusted, native_frame;
    uint32_t device;
    black_aspect_refresh();
#ifndef BLACK_MOVIE_MUTANT_NO_CSR_RESTORE
    _mm_setcsr(caller_csr);
#endif
    adjusted=black_movie_prepare(black_aspect_bits,&owned);
    device=adjusted ? black_movie_frame_capable() : 0;
    native_frame=device!=0;
    if (native_frame) {
        black_movie_frame_device=device;
        black_movie_frame_crop=owned.crop;
    }
    if (adjusted && !native_frame)
        for (i=0;i<4u;i++) GUEST32(owned.address+i*4u)=owned.replacement[i];
    sub_000D6C00_gen(); /* exactly once, including every fallback */
#ifndef BLACK_MOVIE_MUTANT_NO_BORROW_RESTORE
    if (adjusted && !native_frame)
        for (i=0;i<4u;i++) GUEST32(owned.address+i*4u)=owned.saved[i];
#endif
    /* Deliberately do NOT restore CSR, registers, or XMM after the original
     * call. Its genuine FP/control/return effects remain authoritative. */
}

/* END BLACK_PC_NATIVE_MOVIE */

/* BEGIN BLACK_PC_DIRECT_HUD_ASPECT */
/* ISOLATED BLACK_PC_DIRECT_HUD_ASPECT proposal: no production integration.
 * 20BAC0 -> 13C050 -> 13BC30 -> DA4B0 is the owned two-bank HUD route.
 * Borrow the root's local X/SX before actual CFC40 hierarchy preparation;
 * preserve native child modes, every Y field, colors, clipping and callbacks.
 * Preserve the actual native 4:3 chart (mode 0) or 16:9 chart (mode 1)
 * in a centered safe area, with proportional pixels for the selected aspect.
 */
extern void sub_000DA4B0_gen(void);
static int black_direct_disjoint(uint32_t a,uint32_t an,uint32_t b,uint32_t bn)
{ return (uint64_t)a+an<=b || (uint64_t)b+bn<=a; }
static int black_direct_finite(uint32_t bits)
{ return (bits&0x7F800000u)!=0x7F800000u; }
static uint32_t black_direct_scale_bits(void)
{
    unsigned native;
    if(black_aspect_bits!=0x3FE38E39u && black_aspect_bits!=0x40155555u &&
       black_aspect_bits!=0x40638E39u)return 0;
    if(!black_pc_ram_span(BLACK_VA(0x2F4348u),1u))return 0;
    native=*(const unsigned char *)((uintptr_t)g_xbox_mem_offset+BLACK_VA(0x2F4348u));
    if(native){
        if(!black_pc_ram_span(BLACK_VA(0x2AF460u),8u) || GUEST32(BLACK_VA(0x2AF460u))!=0x3F800000u ||
           GUEST32(BLACK_VA(0x2AF464u))!=0x3FAAAAABu)return 0;
        return black_aspect_bits==0x40155555u ? 0x3F430C31u :
               black_aspect_bits==0x40638E39u ? 0x3F000000u : 0u;
    }
    return black_aspect_bits==0x3FE38E39u ? 0x3F400000u :
           black_aspect_bits==0x40155555u ? 0x3F124925u : 0x3EC00000u;
}
/* Validate only inputs that can alias the two temporarily borrowed words.
 * A finite traversal budget rejects cycles/deep trees, and keeps that root
 * outside the stack interval used by this known callback hierarchy. */
static int black_direct_tree(uint32_t node,uint32_t root,unsigned depth,unsigned *left)
{
    if(depth>16u)return 0;
    while(node){
        if(!*left)return 0; --*left;
        if(!black_pc_ram_span(node,0x58u) ||
           !black_direct_disjoint(root,0x50u,node,0x58u))return 0;
        if(!black_direct_tree(GUEST32(node+0x4Cu),root,depth+1u,left))return 0;
        node=GUEST32(node);
    }
    return 1;
}
static int black_direct_input(uint32_t root,uint32_t pointer,uint32_t bytes)
{ return black_pc_ram_span(pointer,bytes) &&
         black_direct_disjoint(root,0x50u,pointer,bytes); }
static int black_direct_texture(uint32_t root,uint32_t pack,uint32_t index)
{
    uint32_t view,bytes;
    if(index>4096u)return 0;
    bytes=0x18u+index*4u;
    if(!black_direct_input(root,pack,bytes))return 0;
    view=GUEST32(pack+0x14u);
    if(view && !black_direct_input(root,view,0x18u))return 0;
    view=GUEST32(pack+0x14u+index*4u);
    return !view || black_direct_input(root,view,0x18u);
}
static int black_direct_font(uint32_t root,uint32_t font,uint32_t text)
{
    unsigned j;
    if(!black_direct_input(root,font,0x220u) ||
       !black_direct_texture(root,GUEST32(font+4u),0u))return 0;
    for(j=0;j<512u;j++){
        uint32_t ch,glyph,end;unsigned k;
        if((uint64_t)text+j*2u>0xFFFFFFFFu ||
           !black_direct_input(root,text+j*2u,2u))return 0;
        ch=*(const uint16_t *)((uintptr_t)g_xbox_mem_offset+text+j*2u);
        if(!ch)return 1;
        glyph=GUEST32(font+0x20u+(ch&0x7Fu)*4u);end=GUEST32(font+0x1Cu);
        for(k=0;k<512u;k++){
            if(!black_direct_input(root,glyph,0x28u))return 0;
            if(*(const uint16_t *)((uintptr_t)g_xbox_mem_offset+glyph+0x1Cu)==ch || glyph==end)break;
            if(glyph>0xFFFFFFFFu-0x28u)return 0;
            glyph+=0x28u;
        }
        if(k==512u)return 0;
    }
    return 0;
}
static int black_direct_records(uint32_t slots,uint32_t n,uint32_t root,uint32_t start,uint32_t count)
{
    unsigned left=512u;
    uint32_t i;
    /* This is an ownership/alias check, never a callback whitelist. The live
     * bank has sprite, styled-font, numeric-font and solid-quad callbacks.
     * Actual DA4B0 remains responsible for flags, selection and dispatch. */
    for(i=start;i<start+count;i++){
        uint32_t node=GUEST32(slots+i*16u+8u);
        if(GUEST32(slots+i*16u)&1u)continue;
        while(node){
            uint32_t transform;
            if(!left--)return 0;
            if(!black_direct_input(root,node,0x48u))return 0;
            transform=GUEST32(node+0x40u);
            if(transform!=root && !black_direct_input(root,transform,0x50u))return 0;
            if(!black_pc_ram_span(transform,0x50u))return 0;
            {uint32_t callback=GUEST32(node+0x44u),object;
             /* Known external input aliases are checked; names never reject an
              * otherwise-owned bank. Unselected/disabled slots are not read. */
             if(callback==0xD8D90u || callback==0xD9110u){
                object=GUEST32(node+0x38u);
                if(!black_direct_input(root,object,0x10u) ||
                   !black_direct_texture(root,GUEST32(object+8u),GUEST32(node+0x3Cu)))return 0;
             }else if(callback==0xD9480u){
                if(!black_direct_texture(root,GUEST32(node+0x38u),GUEST32(node+0x3Cu)))return 0;
             }else if(callback==0xD84B0u){
                object=GUEST32(node+0x2Cu);
                if(!black_direct_input(root,object,0x10u) ||
                   !black_direct_font(root,GUEST32(object+8u),GUEST32(node+0x28u)))return 0;
             }else if(callback==0xD8B80u || callback==BLACK_VA(0x135370u) || callback==BLACK_VA(0x1355B0u)){
                if(!black_direct_font(root,GUEST32(node+0x28u),GUEST32(node+0x2Cu)))return 0;
             }}
            node=GUEST32(node);
        }
    }
    (void)n;
    return 1;
}
static uint32_t black_direct_root(uint32_t context,uint32_t stack)
{
    uint32_t hud,renderer,root,slots,n,start,count,other,stack_begin,device,write,end,i,banks;
    unsigned left=512u;
    if(!black_pc_ram_span(stack,8u) || GUEST32(stack)!=BLACK_VA(0x13C02Fu))return 0;
    if(!black_pc_ram_span(BLACK_VA(0x2D12E4u),4u) || !black_pc_ram_span(BLACK_VA(0x2D67F4u),4u))return 0;
    hud=GUEST32(BLACK_VA(0x2D12E4u));renderer=GUEST32(BLACK_VA(0x2D67F4u));
    if(!black_pc_ram_span(hud,0x240u) || !black_pc_ram_span(renderer,0xD568u) ||
       !*(const unsigned char *)((uintptr_t)g_xbox_mem_offset+renderer+0xCu) ||
       (context!=hud+0x40u && context!=hud+0xE8u))return 0;
    banks=*(const unsigned char *)((uintptr_t)g_xbox_mem_offset+hud+0x23Cu);
    if(!banks || banks>2u || (context==hud+0xE8u && banks<2u))return 0;
    root=GUEST32(context+0x14u);slots=GUEST32(context+0x2Cu);n=GUEST32(context+0x30u);
    start=g_eax;count=GUEST32(stack+4u);
    if(n>34u || !n || start>n || count>n-start || (root&15u) ||
       !black_pc_ram_span(root,0x50u) || !black_pc_ram_span(slots,n*16u))return 0;
    if(GUEST32(root+0x40u)!=context || GUEST32(root+0x4Cu)!=context ||
       GUEST32(root+0x48u)!=0u)return 0;
    other=GUEST32((context==hud+0x40u?hud+0xE8u:hud+0x40u)+0x14u);
    stack_begin=stack>=0x20000u ? stack-0x10000u : 0u;
    if(!stack_begin || !black_pc_ram_span(stack_begin,0x10008u) ||
       !black_direct_disjoint(root,0x50u,stack_begin,0x10008u) ||
       !black_direct_disjoint(hud,0x240u,stack_begin,0x10008u) ||
       !black_direct_disjoint(renderer,0xD568u,stack_begin,0x10008u) ||
       !black_direct_disjoint(slots,n*16u,stack_begin,0x10008u) ||
       !black_direct_disjoint(BLACK_VA(0x2D12E4u),4u,stack_begin,0x10008u) ||
       !black_direct_disjoint(BLACK_VA(0x2D67F4u),4u,stack_begin,0x10008u) ||
       !black_direct_disjoint(root,0x50u,hud,0x240u) ||
       !black_direct_disjoint(root,0x50u,renderer,0xD568u) ||
       !black_direct_disjoint(root,0x50u,slots,n*16u) ||
       (other && !black_direct_disjoint(root,0x50u,other,0x50u)) ||
       !black_direct_disjoint(root,0x50u,BLACK_VA(0x2D12E4u),4u) ||
       !black_direct_disjoint(root,0x50u,BLACK_VA(0x2D67F4u),4u) ||
       !black_direct_disjoint(root,0x50u,BLACK_VA(0x2F4348u),1u) ||
       !black_direct_disjoint(root,0x50u,BLACK_VA(0x2AF460u),8u) ||
       !black_direct_disjoint(root,0x50u,BLACK_VA(0x295C00u),0x1000u) ||
       !black_direct_disjoint(root,0x50u,BLACK_VA(0x2EDB00u),0x3000u) ||
       !black_direct_disjoint(root,0x50u,BLACK_VA(0x2F3600u),0xE00u) ||
       !black_direct_disjoint(root,0x50u,BLACK_VA(0x2F4518u),0x20u) ||
       !black_direct_disjoint(root,0x50u,BLACK_VA(0x563120u),0x1810u) ||
       !black_direct_disjoint(root,0x50u,BLACK_VA(0x56EF30u),4u))return 0;
    /* Device setup/binding can update current resource reference words and
     * its command buffer. Keep the borrowed root outside those inputs too. */
    device=GUEST32(BLACK_VA(0x27BFF8u));
    if(!black_direct_input(root,device,0x1A10u))return 0;
    write=GUEST32(device);end=GUEST32(device+4u);
    if(end<write || !black_direct_input(root,write,end-write))return 0;
    for(i=0;i<4u;i++){uint32_t view=GUEST32(device+0xF98u+i*4u);
        if(view && !black_direct_input(root,view,0x18u))return 0;}
    {uint32_t target=GUEST32(device+0x1A04u);
        if(target && !black_direct_input(root,target,0x18u))return 0;}
    if(!black_direct_finite(GUEST32(root)) || !black_direct_finite(GUEST32(root+8u)) ||
       (GUEST32(root+8u)&0x80000000u) || !(GUEST32(root+8u)&0x7FFFFFFFu))return 0;
    if(!black_direct_tree(GUEST32(root+0x44u),root,0u,&left) ||
       !black_direct_records(slots,n,root,start,count))return 0;
    return root;
}
void sub_000DA4B0(void)
{
    const uint32_t context=g_ecx,stack=g_esp;
    const unsigned caller_csr=_mm_getcsr();
    uint32_t root=0,old_x=0,old_sx=0,new_x=0,new_sx=0;
    black_aspect_refresh();
#ifndef DIRECT_HUD_MUTANT_CSR
    _mm_setcsr(caller_csr);
#endif
    if(black_direct_scale_bits())root=black_direct_root(context,stack);
    if(root){
        __m128 scale=black_hud_scalar(black_direct_scale_bits());
        old_x=GUEST32(root);old_sx=GUEST32(root+8u);
        /* Policy arithmetic is masked RN and cannot add guest sticky flags.
         * Original hierarchy/draw arithmetic then uses the caller's CSR. */
        _mm_setcsr(0x1F80u);
#ifdef DIRECT_HUD_MUTANT_CENTER
        new_x=black_hud_result(_mm_mul_ss(black_hud_scalar(old_x),scale));
#else
        new_x=black_hud_result(_mm_add_ss(
            _mm_mul_ss(_mm_sub_ss(black_hud_scalar(old_x),_mm_set_ss(320.0f)),scale),
            _mm_set_ss(320.0f)));
#endif
        new_sx=black_hud_result(_mm_mul_ss(black_hud_scalar(old_sx),scale));
        /* Reject a new invalid scale/coordinate instead of changing original
         * exceptional-input behavior. Integer tests introduce no FP flags. */
        if(black_direct_finite(new_x) && black_direct_finite(new_sx) &&
           (new_sx&0x7FFFFFFFu)){
            GUEST32(root)=new_x;GUEST32(root+8u)=new_sx;
        }else root=0;
#ifndef DIRECT_HUD_MUTANT_CSR
        _mm_setcsr(caller_csr);
#endif
    }
    sub_000DA4B0_gen();
#ifndef DIRECT_HUD_MUTANT_RESTORE
    if(root){
        /* Respect an original callback that deliberately changes a borrowed
         * local word. Restore only each still-owned temporary value. */
        if(GUEST32(root)==new_x)GUEST32(root)=old_x;
        if(GUEST32(root+8u)==new_sx)GUEST32(root+8u)=old_sx;
    }
#endif
}

/* END BLACK_PC_DIRECT_HUD_ASPECT */

/* BEGIN BLACK_PC_INPUT
 * Native keyboard and mouse (xboxrecomp/src/input/pc_input.c, merged into the pad report the
 * guest already reads). Three guest-facing pieces live here; none changes the controller path:
 *
 * 1. black_pc_input_resolver: the live action map. [0x2F3BD4] points at 37 u32 control ids (BLACK's
 *    own 28-control numbering, 28 = none), one per action slot; sub_001F1520 fills it from the
 *    selected preset. The keyboard lands on whatever control the map gives each slot, so presets
 *    and custom controls move it exactly as they move the pad.
 *
 * 2. sub_001BD3F0, the camera's look integrator (called once per update by sub_001EC450 at
 *    0x001ECF71 with ESI = the camera object). Angles are degrees: yaw [+0x18], pitch [+0x1C]
 *    (clamped +-70, constants 0x2888C8 / 0x295E38), yaw cache [+0x10], zoom compensation from
 *    [[camera+0x8C]+0x2A4]. The original body runs untouched; raw mouse counts then add degrees
 *    directly, bypassing its dead zone, power curve, hold acceleration, smoothing and turn-rate
 *    cap, scaled by the same zoom compensation, with the same pitch clamp and yaw wrap and the
 *    yaw cache kept equal. Every call also tells the host the player is in control, which is
 *    what captures the pointer and switches the keyboard from menu keys to gameplay keys.
 *
 * 3. sub_00113BF0, the prompt formatter: one cdecl argument, a UTF-16 buffer rewritten in place
 *    with NO capacity argument. While keyboard/mouse prompts are active the tokens (U+F001..)
 *    are replaced first by pc_input_expand_prompt, bounded by the capacity each call site proves
 *    (conversion limit and the next field of the owning object, read from the original code):
 *      0x00118630  sub_001184D0  stack buffer, sub_000BFD30 limit 0x100 -> 256
 *      0x00129EB7  sub_00129D80  object+0x140, limit 0x78, next field +0x230 -> 120
 *      0x0012B4EF  sub_0012B3C0  object+0x2114, limit 0xC8, next field +0x22A4 -> 200
 *      0x0012B4E0  sub_0012B3C0  object+0x22C0, limit 0x40 ("to continue") -> 64
 *    Any other caller is left to the original. The original then runs in every mode (it finds no
 *    tokens in text already expanded, and does exactly what it always did for controller text).
 *
 * --exclude-manual preserves the generated originals as sub_001BD3F0_gen and sub_00113BF0_gen.
 */
#include "pc_input.h"

/* BEGIN BLACK_PC_NATIVE_INPUT_ACK
 * C5A80 is the connected-pad decoder (vtable 28CE00+4). Native 207A70 first
 * refreshes the four 32-byte hardware records, then calls this decoder for
 * every active input component, including frontend/paused menus. Its final
 * C5F20 loop writes 24 current down bytes at self+25 from normalized controls
 * self+44+i*4; these are what the subsequent game/UI consumers actually see.
 * A USB poll alone cannot acknowledge a short PC tap. Capture a generation
 * before native decode, then acknowledge only this primary connected pad's
 * observed controls after native decode. Focus loss still clears pending
 * host input immediately; no guest input values or original predicates change.
 */
extern void sub_000C5A80_gen(void);

void sub_000C5A80(void)
{
    const uint32_t self = g_ecx;
    uint64_t token = 0;
    int sampled = 0;
    if (black_pc_ram_span(BLACK_VA(0x002D45E8u), 4u)) {
        const uint32_t manager = GUEST32(BLACK_VA(0x002D45E8u));
        if (black_pc_ram_span(manager, 0x1B8u) &&
            self == manager + 0x98u && GUEST32(self) == BLACK_VA(0x0028CE00u)) {
            const uint32_t records = GUEST32(self + 0x118u);
            const uint32_t port = GUEST32(self + 0x11Cu);
            const uint64_t record = (uint64_t)records + (uint64_t)port * 32u;
            if (records && port < 4u && record <= UINT32_MAX &&
                black_pc_ram_span((uint32_t)record, 32u) &&
                GUEST32((uint32_t)record) == 2u) {
                const unsigned csr = _mm_getcsr();
                fenv_t environment;
                fegetenv(&environment);
                fesetenv(FE_DFL_ENV);
                _mm_setcsr(0x1F80u);
                token = pc_input_native_sample_begin();
                fesetenv(&environment);
                _mm_setcsr(csr);
                sampled = 1;
            }
        }
    }
    sub_000C5A80_gen();
    if (sampled && *(const uint8_t *)((uintptr_t)g_xbox_mem_offset + self + 8u)) {
        const uint8_t *down = (const uint8_t *)((uintptr_t)g_xbox_mem_offset + self + 0x25u);
        uint32_t observed = 0;
        unsigned i;
        const unsigned csr = _mm_getcsr();
        fenv_t environment;
        for (i = 0; i < 24u; ++i) if (down[i]) observed |= 1u << i;
        fegetenv(&environment);
        fesetenv(FE_DFL_ENV);
        _mm_setcsr(0x1F80u);
        pc_input_native_sample_end(token, observed);
        fesetenv(&environment);
        _mm_setcsr(csr);
    }
}
/* END BLACK_PC_NATIVE_INPUT_ACK */

/* Native pause and modal-HUD authority; unknown layouts add no veto.
 * 20D45F/20D57B own main+210C8, while BE490/BE4B0 (including 31C39)
 * own the world-clock running byte+28. Camera+821 is scripted mode.
 * Completed-frame camera observation remains the positive gameplay evidence.
 */
int black_pc_input_gameplay_blocked_query(void)
{
    if (black_pc_ram_span(BLACK_VA(0x002D45F0u), 4u)) {
        const uint32_t main = GUEST32(BLACK_VA(0x002D45F0u));
        if (black_pc_ram_span(main, 0x210C9u) &&
            *(const uint8_t *)((uintptr_t)g_xbox_mem_offset + main + 0x210C8u))
            return 1;
    }
    if (black_pc_ram_span(BLACK_VA(0x002D45D4u), 4u)) {
        const uint32_t clock = GUEST32(BLACK_VA(0x002D45D4u));
        if (black_pc_ram_span(clock, 0x29u) &&
            !*(const uint8_t *)((uintptr_t)g_xbox_mem_offset + clock + 0x28u))
            return 1;
    }
    return 0;
}
/* END BLACK_PC_NATIVE_INPUT_CONTEXT */



extern void sub_001BD3F0_gen(void);
extern void sub_00113BF0_gen(void);
extern void sub_001F3A80_gen(void);
extern void sub_00102840_gen(void);
extern void sub_00102D90_gen(void);
extern void sub_00114030_gen(void);

static float black_input_f32(uint32_t va)
{
    float value;
    memcpy(&value, (const void *)((uintptr_t)g_xbox_mem_offset + va), sizeof(value));
    return value;
}

static void black_input_put_f32(uint32_t va, float value)
{
    memcpy((void *)((uintptr_t)g_xbox_mem_offset + va), &value, sizeof(value));
}

/* Called from the OHCI thread: a control id 0..27, -1 for "none".
 * Until the live action map is ready, read the default preset from the user's
 * loaded XBE. No retail control table is embedded in the host source. */
int black_pc_input_resolver(int slot)
{
    const uint32_t map_pointer = BLACK_VA(0x002F3BD4u);
    uint32_t map, control;

    if (slot < 0 || slot > 36) return -1;
    map = black_pc_ram_span(map_pointer, 4u) ? GUEST32(map_pointer) : 0;
    if (!map || !black_pc_ram_span(map, 37u * 4u)) map = BLACK_VA(0x002A7AF0u);
    if (!black_pc_ram_span(map, 37u * 4u)) return -1;
    control = GUEST32(map + (uint32_t)slot * 4u);
    return control < 28u ? (int)control : -1;
}

static void black_mouse_look(uint32_t camera)
{
    double yaw_delta, pitch_delta, zoom_scale = 1.0, yaw, pitch, upper, lower;
    uint32_t zoom_object;
    const unsigned int guest_csr = _mm_getcsr();

    if (!black_pc_ram_span(camera, 0x140u)) return;
    _mm_setcsr(0x1F80u);
    if (!pc_input_consume_look(&yaw_delta, &pitch_delta)) {
        _mm_setcsr(guest_csr);
        return;
    }
    zoom_object = GUEST32(camera + 0x8Cu);
    if (black_pc_ram_span(zoom_object, 0x2A8u)) {
        /* the original's own expression: 1 / (((zoom - 1) * k) + 1) */
        const double one = black_input_f32(BLACK_VA(0x00295C74u)), k = black_input_f32(BLACK_VA(0x00295DB8u));
        const double zoom = black_input_f32(zoom_object + 0x2A4u);
        const double denominator = (zoom - one) * k + one;
        if (isfinite(denominator) && denominator > 0.0 && isfinite(one) && one > 0.0)
            zoom_scale = one / denominator;
    }
    /* Policy arithmetic runs masked and round-to-nearest, then the guest's control word is put
     * back: it adds no traps and no sticky flags to the guest's floating-point state. */
    yaw = (double)black_input_f32(camera + 0x18u) + yaw_delta * zoom_scale;
    pitch = (double)black_input_f32(camera + 0x1Cu) + pitch_delta * zoom_scale;
    upper = black_input_f32(BLACK_VA(0x002888C8u));       /* +70 */
    lower = black_input_f32(BLACK_VA(0x00295E38u));       /* -70 */
    if (isfinite(yaw) && isfinite(pitch) && isfinite(upper) && isfinite(lower) && lower < upper) {
        float yaw_value, pitch_value;
        yaw = fmod(yaw + 180.0, 360.0);          /* any size of turn, into [-180, 180) */
        if (yaw < 0.0) yaw += 360.0;
        yaw -= 180.0;
        if (pitch > upper) pitch = upper;
        if (pitch < lower) pitch = lower;
        yaw_value = (float)yaw;
        pitch_value = (float)pitch;
        if (pc_input_trace_enabled())
            fprintf(stderr, "[INPUT] look: yaw %.3f -> %.3f (asked %+.3f deg), pitch %.3f -> %.3f (asked %+.3f deg), zoom scale %.3f\n",
                    black_input_f32(camera + 0x18u), yaw_value, yaw_delta * zoom_scale,
                    black_input_f32(camera + 0x1Cu), pitch_value, pitch_delta * zoom_scale, zoom_scale);
        black_input_put_f32(camera + 0x18u, yaw_value);
        black_input_put_f32(camera + 0x1Cu, pitch_value);
        black_input_put_f32(camera + 0x10u, yaw_value);
    }
    _mm_setcsr(guest_csr);
}

void sub_001BD3F0(void)
{
    const uint32_t camera = g_esi;
    const uint32_t stack = g_esp;
    const unsigned int caller_csr = _mm_getcsr();
    pc_input_init();
    _mm_setcsr(caller_csr);              /* first-call logging must not touch the guest's control word */
    pc_input_gameplay_tick();
    /* A mouse takes over immediately after a pad turn. Leave actual stick
     * input/controller mode alone; with neutral stick arguments, discard
     * only the pad's two smoothing-history terms before native integration.
     */
    if (black_pc_ram_span(camera, 0xA8u) && black_pc_ram_span(stack, 12u) &&
        (GUEST32(stack + 4u) & 0x7FFFFFFFu) == 0u &&
        (GUEST32(stack + 8u) & 0x7FFFFFFFu) == 0u &&
        (pc_input_input_mode() == PC_MODE_KEYBOARD_MOUSE ||
         (pc_input_input_mode() == PC_MODE_AUTO &&
          pc_input_active_device() == PC_DEVICE_KEYBOARD_MOUSE))) {
        GUEST32(camera + 0xA0u) = 0u;
        GUEST32(camera + 0xA4u) = 0u;
    }
    _mm_setcsr(caller_csr);
    sub_001BD3F0_gen();
    black_mouse_look(camera);

}

#include "black_hud_input.inc"

void sub_001314E0(void) { black_hint_tick(); }

void sub_00113BF0(void)
{
    const uint32_t stack = g_esp;
    const unsigned int caller_csr = _mm_getcsr();
    BlackHintField *hint = NULL;
    uint32_t buffer = 0;
    int device;
    pc_input_init();
    _mm_setcsr(caller_csr);
    device = (int)pc_input_prompt_device();
    if (black_pc_ram_span(stack, 8u)) {
        const uint32_t caller = GUEST32(stack);
        buffer = GUEST32(stack + 4u);
        hint = black_hint_capture(caller, buffer, device);
    }
    if (device == PC_DEVICE_KEYBOARD_MOUSE && black_pc_ram_span(stack, 8u)) {
        const uint32_t caller = GUEST32(stack);
        uint32_t capacity = 0;
        int confirm_only = 0;
        switch (caller) {
        case BLACK_VA(0x00118630u): capacity = 256u; break;
        case BLACK_VA(0x00129EB7u): capacity = 120u; break;
        case BLACK_VA(0x0012B4EFu): capacity = 200u; break;
        case BLACK_VA(0x0012B4E0u): capacity = 64u; confirm_only = 1; break;   /* "\r\n\r\n<A> to continue" */
        default: break;
        }
        if (capacity && black_pc_ram_span(buffer, capacity * 2u))
            pc_input_expand_prompt((uint16_t *)((uintptr_t)g_xbox_mem_offset + buffer), capacity, confirm_only);
        _mm_setcsr(caller_csr);
    }
    sub_00113BF0_gen();
    if (hint) hint->valid = black_hint_copy(hint->rendered, buffer, hint->capacity);
}
/*
 * 4. sub_001F3A80, the localized-string fetch (thiscall: ECX = the text manager, one stack argument,
 *    the name hash; returns a UTF-8 pointer or 0). The menu legends and a few "press ..." lines
 *    are plain strings holding the pad's button glyphs (MainUS.bin: U+00B2 is A, U+00B9 is B,
 *    U+00B3 is X, U+00B0 is Y) and "press start". They are not tokens, so the formatter never sees
 *    them. While keyboard prompts are active the eleven known ones come back as text naming the
 *    menu keys instead: stable guest-memory copies made once, so whoever keeps the pointer keeps
 *    a valid string, and the pad's own strings are untouched in controller mode.
 */
typedef struct BlackMenuPrompt {
    uint32_t hash;
    int key;                     /* PcMenuKey, or -1 for the literal text below */
    const char *before, *after;  /* text around the key label */
    uint32_t guest;              /* where the copy lives */
} BlackMenuPrompt;

static BlackMenuPrompt black_menu_prompts[] = {
    { 0xE0CA7C5Fu, PC_MENU_CONFIRM, "", " confirm", 0 },                    /* FE_CONFIRM */
    { 0x64199B75u, PC_MENU_BACK, "", " back", 0 },                           /* FE_BACK */
    { 0xD3242A98u, PC_MENU_CONFIRM, "", "", 0 },                               /* A */
    { 0x27F09FC9u, PC_MENU_BACK, "", "", 0 },                                  /* B */
    { 0xD449EE81u, PC_MENU_X, "", "", 0 },                                     /* X */
    { 0xB0224641u, PC_MENU_Y, "", "", 0 },                                     /* Y */
    { 0x01BBE675u, PC_MENU_CONFIRM, "press ", " for info", 0 },
    { 0x44C20325u, PC_MENU_CONFIRM, "press ", " to start selected challenge", 0 },
    { 0x51363FD2u, PC_MENU_CONFIRM, "press ", " to save", 0 },
    { 0x7F5886E4u, PC_MENU_CONFIRM, "press ", " to start", 0 },
    { 0x2FCCE936u, PC_MENU_CONFIRM, "press ", "", 0 },                         /* "press start" */
    { 0x661A3A37u, PC_MENU_CONFIRM, "press ", "", 0 },
    { 0x905E4EE0u, PC_MENU_CONFIRM, "press ", "", 0 },
};
static INIT_ONCE black_menu_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK black_menu_init(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    size_t i;
    const char *language = getenv("RECOMP_LANGUAGE");
    (void)once; (void)parameter; (void)context;
    for (i = 0; i < sizeof(black_menu_prompts) / sizeof(black_menu_prompts[0]); i++) {
        BlackMenuPrompt *prompt = &black_menu_prompts[i];
        char label[16], text[96];
        uint32_t guest;
        if (!pc_input_menu_text((PcMenuKey)prompt->key, label, sizeof(label))) continue;
        snprintf(text, sizeof(text), "%s%s%s",
            black_menu_prompt_localize(prompt->before, language), label,
            black_menu_prompt_localize(prompt->after, language));
        guest = xbox_HeapAlloc((uint32_t)(strlen(text) + 1u), 16u);
        if (!guest || !black_pc_ram_span(guest, (uint32_t)(strlen(text) + 1u))) continue;
        memcpy((void *)((uintptr_t)g_xbox_mem_offset + guest), text, strlen(text) + 1u);
        prompt->guest = guest;
    }
    return TRUE;
}

#include "black_menu_input.inc"

/* Keep exported wrappers visible to --exclude-manual's source scanner. */
void sub_00102840(void) { black_menu_render(); }
void sub_00102D90(void) { black_menu_text_prepare(); }
void sub_00114030(void) { black_menu_text_destroy(); }

extern BOOL xbox_translate_path(const char *xbox_path, WCHAR *host_path_buf, DWORD buf_size);
#include "black_pc_branding.h"
extern RECOMP_TLS uint32_t g_ebp, g_seh_ebp;

/* BEGIN BLACK_PC_BRANDING_RUNTIME
 * Stable process-lifetime guest copies; localized asset bytes remain untouched.
 * A source address can be recycled, so the cache is keyed by complete content.
 * Localized text fetch runs on the game thread, like the native menu bridge.
 */
#define BLACK_PC_BRAND_GUEST_BYTES 4096u
#define BLACK_PC_BRAND_GUEST_SLOTS 64u
typedef struct BlackPcBrandGuestString {
    char source[BLACK_PC_BRAND_GUEST_BYTES];
    uint32_t guest;
    size_t source_bytes;
} BlackPcBrandGuestString;
static BlackPcBrandGuestString black_pc_brand_guest_strings[BLACK_PC_BRAND_GUEST_SLOTS];

static uint32_t black_pc_brand_guest_raw(uint32_t source)
{
    char original[BLACK_PC_BRAND_GUEST_BYTES], branded[BLACK_PC_BRAND_GUEST_BYTES];
    size_t bytes, i, output_bytes;
    int changed = 0;
    BlackPcBrandGuestString *empty = NULL;
    for (bytes = 0; bytes < sizeof(original); ++bytes) {
        const uint64_t address = (uint64_t)source + bytes;
        if (address > 0xFFFFFFFFu || !black_pc_ram_span((uint32_t)address, 1u)) return source;
        original[bytes] = *(const char *)((uintptr_t)g_xbox_mem_offset + (uint32_t)address);
        if (!original[bytes]) break;
    }
    if (bytes == sizeof(original)) return source;
    for (i = 0; i < bytes; ++i)
        if (black_pc_brand_match(original + i, bytes - i, "xbox")) { changed = 1; break; }
    if (!changed) return source;
    ++bytes; /* Include NUL in both cache identity and validation. */
    for (i = 0; i < BLACK_PC_BRAND_GUEST_SLOTS; ++i) {
        BlackPcBrandGuestString *entry = &black_pc_brand_guest_strings[i];
        if (!entry->guest && !empty) empty = entry;
        if (entry->guest && entry->source_bytes == bytes && memcmp(entry->source, original, bytes) == 0)
            return entry->guest;
    }
    if (!empty) return source;
    output_bytes = black_pc_brand_text(branded, sizeof(branded), original, bytes);
    if (output_bytes == BLACK_PC_BRAND_ERROR) return source;
    empty->guest = xbox_HeapAlloc((uint32_t)(output_bytes + 1u), 16u);
    if (!empty->guest || !black_pc_ram_span(empty->guest, (uint32_t)(output_bytes + 1u))) {
        empty->guest = 0;
        return source;
    }
    memcpy((void *)((uintptr_t)g_xbox_mem_offset + empty->guest), branded, output_bytes + 1u);
    memcpy(empty->source, original, bytes);
    empty->source_bytes = bytes;
    return empty->guest;
}

/* Both the localized-fetch hook and native menu callbacks use this entrypoint.
 * Preserve the complete guest register/stack and MXCSR state even on first-use
 * allocation, failure, or cache hit; only the returned host value is new.
 */
static uint32_t black_pc_brand_guest(uint32_t source)
{
    const unsigned int guest_csr = _mm_getcsr();
    const uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, ebx = g_ebx, esi = g_esi, edi = g_edi;
    const uint32_t esp = g_esp, ebp = g_ebp, seh_ebp = g_seh_ebp;
    const uint32_t branded = black_pc_brand_guest_raw(source);
    g_eax = eax; g_ecx = ecx; g_edx = edx; g_ebx = ebx; g_esi = esi; g_edi = edi;
    g_esp = esp; g_ebp = ebp; g_seh_ebp = seh_ebp;
    _mm_setcsr(guest_csr);
    return branded;
}
/* END BLACK_PC_BRANDING_RUNTIME */

#include "black_native_menu.inc"
#include "black_localization.h"

/* Retry the two known frontend labels with the exact caller-supplied manager.
 * FE__LOADING is an alias: its text is stored under FE_LOADING in MainUS.bin. */
static uint32_t black_pc_localized_fallback(uint32_t manager, uint32_t query, uint32_t source)
{
    const uint32_t record_id = black_localization_fallback_record(query);
    const uint32_t localized = record_id ? bnm_string_from_manager(manager, record_id) : 0u;
    static int trace_enabled = -1;
    static unsigned trace_count;
    if (trace_enabled < 0) {
        const char *e = getenv("RECOMP_BLACK_LOCALIZATION_TRACE");
        trace_enabled = e && *e && *e != '0';
    }
    if (trace_enabled && record_id && trace_count < 32u) {
        fprintf(stderr, "[LOC-FALLBACK] query=%08X manager=%08X record=%08X source=%08X result=%08X\n",
                query, manager, record_id, source, localized);
        ++trace_count;
    }
    return localized ? localized : source;
}

/* Bounded opt-in trace for the two PT-BR front-end labels under investigation. */
static void black_pc_localized_trace(uint32_t query, uint32_t result)
{
    static int enabled = -1;
    static unsigned count;
    const uint32_t record_id = black_localization_fallback_record(query);
    char text[80];
    unsigned i;
    if (enabled < 0) { const char *e = getenv("RECOMP_BLACK_LOCALIZATION_TRACE"); enabled = e && *e && *e != '0'; }
    if (!enabled || !record_id || count >= 32u || !black_pc_ram_span(result, 1u)) return;
    text[0] = 0;
    {
        const char *s = (const char *)((uintptr_t)g_xbox_mem_offset + result);
        for (i = 0; i + 1u < sizeof(text) && black_pc_ram_span(result + i, 1u) && s[i]; ++i) text[i] = (s[i] >= 32 && s[i] < 127) ? s[i] : '.';
        text[i] = 0;
    }
    fprintf(stderr, "[LOC-TRACE] query=%08X manager=%08X result=%08X text=%s\n", query, g_ecx, result, text);
    ++count;
}

/* The confirmation hint also works with RECOMP_NATIVE_MENUS=0, and before a
 * native page has loaded its fonts. The font is copied by the runtime, so the
 * game's file stays untouched. Retry-free on a missing installation asset. */
static void black_movie_skip_font(void)
{
    static int attempted;
    if (pc_menu_has_font(PCM_FONT_SMALL)) return;
    if (!attempted) {
        attempted = 1;
        bnm_load_font(PCM_FONT_SMALL, "D:\\language\\fonts\\Small.bin");
    }
}

/* BEGIN BLACK_PC_BRANDING_FETCH */
void sub_001F3A80(void)
{
    const uint32_t stack = g_esp;
    const uint32_t manager = g_ecx;
    const uint32_t hash = black_pc_ram_span(stack, 8u) ? GUEST32(stack + 4u) : 0u;
    size_t i;
    sub_001F3A80_gen();
    black_pc_localized_trace(hash, g_eax);
    g_eax = black_pc_localized_fallback(manager, hash, g_eax);
    if (!g_eax) return;
    {
        const unsigned int guest_csr = _mm_getcsr();
        const uint32_t ecx = g_ecx, edx = g_edx, ebx = g_ebx, esi = g_esi, edi = g_edi;
        const uint32_t esp = g_esp, ebp = g_ebp, seh_ebp = g_seh_ebp;
        const uint32_t branded = black_pc_brand_guest(g_eax);
        g_ecx = ecx; g_edx = edx; g_ebx = ebx; g_esi = esi; g_edi = edi;
        g_esp = esp; g_ebp = ebp; g_seh_ebp = seh_ebp;
        g_eax = branded;
        _mm_setcsr(guest_csr);
    }
    if (black_hint_native_formatting) return;
    for (i = 0; i < sizeof(black_menu_prompts) / sizeof(black_menu_prompts[0]); i++) {
        if (black_menu_prompts[i].hash != hash) continue;
        if (pc_input_prompt_device() == PC_DEVICE_KEYBOARD_MOUSE) {
            const unsigned int guest_csr = _mm_getcsr();
            InitOnceExecuteOnce(&black_menu_once, black_menu_init, NULL, NULL);
            _mm_setcsr(guest_csr);
            if (black_menu_prompts[i].guest) g_eax = black_menu_prompts[i].guest;
        }
        break;
    }
}
/* END BLACK_PC_BRANDING_FETCH */
/* END BLACK_PC_INPUT */
