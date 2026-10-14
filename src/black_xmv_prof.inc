/* XMV decoder profile (diagnostic), included by recomp_manual.c.
 *
 * RECOMP_BLACK_XMV_PROF=ms: time every call of the XMV frame decoder (sub_0023F45C, "GetNextFrame") and,
 * for the calls that take longer than ms, print how much of it its main parts took:
 *   [XMVP] 123.4 ms: idct 3300x 40.1 ms, mc 3100x 22.0 ms, clear 9000x 8.0 ms, mb 1200x, ...
 * Each part is a generated function wrapped here (renamed _gen in recomp_0013.c); times are inclusive.
 * Unset, the wrappers call straight through. */
extern void sub_0023F45C_gen(void);
extern void sub_00242535_gen(void);
extern void sub_0023FAD9_gen(void);
extern void sub_00240683_gen(void);
extern void sub_002415BA_gen(void);

enum { XP_FRAME, XP_IDCT, XP_MC, XP_CLEAR, XP_A, XP_B, XP_COUNT };
static int xmv_prof_ms = -1;
static long long xmv_prof_ticks[XP_COUNT], xmv_prof_calls[XP_COUNT];
static uint32_t xmv_last_caller, xmv_last_arg3, xmv_last_dec;

static long long xmv_qpc(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static int xmv_prof_on(void)
{
    if (xmv_prof_ms < 0) {
        const char *e = getenv("RECOMP_BLACK_XMV_PROF");
        xmv_prof_ms = e && *e ? atoi(e) : 0;
    }
    return xmv_prof_ms > 0;
}

#define XMV_PART(name, slot, callee) \
    void name(void) \
    { \
        long long t0; \
        if (!xmv_prof_on()) { callee(); return; } \
        t0 = xmv_qpc(); \
        callee(); \
        xmv_prof_ticks[slot] += xmv_qpc() - t0; \
        xmv_prof_calls[slot]++; \
    }

XMV_PART(sub_002420C8, XP_IDCT, idct_dispatch)
XMV_PART(sub_00242535, XP_MC, sub_00242535_gen)
XMV_PART(sub_0023FAD9, XP_CLEAR, sub_0023FAD9_gen)
XMV_PART(sub_00240683, XP_A, sub_00240683_gen)
XMV_PART(sub_002415BA, XP_B, sub_002415BA_gen)

void sub_0023F45C(void)
{
    long long t0, dt, freq;
    LARGE_INTEGER f;
    int i;
    if (!xmv_prof_on()) { sub_0023F45C_gen(); return; }
    xmv_last_caller = GUEST32(g_esp);
    xmv_last_dec = GUEST32(g_esp + 4);
    xmv_last_arg3 = GUEST32(g_esp + 12);
    memset(xmv_prof_ticks, 0, sizeof(xmv_prof_ticks));
    memset(xmv_prof_calls, 0, sizeof(xmv_prof_calls));
    t0 = xmv_qpc();
    sub_0023F45C_gen();
    dt = xmv_qpc() - t0;
    QueryPerformanceFrequency(&f);
    freq = f.QuadPart;
    {   /* one summary line per second: how many calls, how long in all, from where */
        static long long win_start, win_calls, win_ticks, win_max;
        static uint32_t callers[4]; static long long caller_n[4];
        uint32_t ret = xmv_last_caller;      /* the return address the call was made with */
        static uint32_t pairs[6][2]; static long long pair_n[6];
        uint32_t res = g_eax, st = GUEST32(xmv_last_arg3), dec = xmv_last_dec;
        static uint32_t first_dec, first_b8, first_vb, first_6c;
        int pk, pplaced = 0;
        int k, placed = 0;
        win_calls++; win_ticks += dt; if (dt > win_max) win_max = dt;
        {   /* a run of idle calls (status 0) followed by progress: report how long the run took */
            static long long run_calls, run_t0; static uint32_t run_vb0;
            uint32_t dev2 = GUEST32(BLACK_VA(0x27BFF8)), vb = dev2 ? GUEST32(dev2 + 0x1DE8) : 0;
            static uint32_t snap[0x60];
            if (st == 0 && run_calls == 20000) {
                int w;
                fprintf(stderr, "[XMVP] display: flags +1DDC=%08X +1DF8=%08X vblank +1DE8=%u\n",
                        dev2 ? GUEST32(dev2 + 0x1DDC) : 0, dev2 ? GUEST32(dev2 + 0x1DF8) : 0, vb);
                fprintf(stderr, "[XMVP] decoder dump after 20000 idle calls:");
                for (w = 0; w < 0x60; w++) { snap[w] = GUEST32(dec + 4 * w); if (!(w % 8)) fprintf(stderr, "\n  +%03X:", 4 * w); fprintf(stderr, " %08X", snap[w]); }
                fprintf(stderr, "\n");
            }
            if (st == 0 && (run_calls % 400000) == 399999)
                fprintf(stderr, "[XMVP] idle %lld calls: flags +1DDC=%08X +1DF8=%08X vblank %u\n", run_calls + 1,
                        dev2 ? GUEST32(dev2 + 0x1DDC) : 0, dev2 ? GUEST32(dev2 + 0x1DF8) : 0, vb);
            if (st == 0) { if (!run_calls) { run_t0 = t0; run_vb0 = vb; } run_calls++; }
            else {
                if (run_calls > 20000) {
                    int w;
                    fprintf(stderr, "[XMVP] decoder fields that changed by the end of the run:");
                    for (w = 0; w < 0x60; w++) if (GUEST32(dec + 4 * w) != snap[w]) fprintf(stderr, " +%03X %08X->%08X", 4 * w, snap[w], GUEST32(dec + 4 * w));
                    fprintf(stderr, "\n");
                }
                if (run_calls > 2000)
                    fprintf(stderr, "[XMVP] idle run of %lld calls lasted %.1f ms, vblanks %u -> %u (status now %u, decoder 0x%08X +0x6C=0x%08X)\n",
                            run_calls, (double)(t0 - run_t0) * 1000.0 / (double)freq, run_vb0, vb, st, dec, GUEST32(dec + 0x6C));
                run_calls = 0;
            }
        }
        for (pk = 0; pk < 6 && !pplaced; pk++) if (pair_n[pk] && pairs[pk][0] == res && pairs[pk][1] == st) { pair_n[pk]++; pplaced = 1; }
        for (pk = 0; pk < 6 && !pplaced; pk++) if (!pair_n[pk]) { pairs[pk][0] = res; pairs[pk][1] = st; pair_n[pk] = 1; pplaced = 1; }
        if (win_calls == 1) {
            uint32_t dev = GUEST32(BLACK_VA(0x27BFF8));
            first_dec = dec; first_b8 = GUEST32(dec + 0xB8); first_6c = GUEST32(dec + 0x6C);
            first_vb = dev ? GUEST32(dev + 0x1DE8) : 0;
        }
        for (k = 0; k < 4 && !placed; k++) if (callers[k] == ret) { caller_n[k]++; placed = 1; }
        for (k = 0; k < 4 && !placed; k++) if (!callers[k]) { callers[k] = ret; caller_n[k] = 1; placed = 1; }
        if (!win_start) win_start = t0;
        if (t0 - win_start >= freq) {
            fprintf(stderr, "[XMVP] second t=%.1f: %lld calls, %.1f ms in all, longest %.2f ms\n",
                    (double)t0 / (double)freq, win_calls, (double)win_ticks * 1000.0 / (double)freq, (double)win_max * 1000.0 / (double)freq);
            fprintf(stderr, "[XMVP]   decoder 0x%08X +0xB8=%u +0x6C=%u, vblank counter %u at the window's first call\n", first_dec, first_b8, first_6c, first_vb);
            for (pk = 0; pk < 6 && pair_n[pk]; pk++) fprintf(stderr, "[XMVP]   result %u status %u x%lld\n", pairs[pk][0], pairs[pk][1], pair_n[pk]);
            memset(pair_n, 0, sizeof(pair_n));
            for (k = 0; k < 4 && callers[k]; k++) fprintf(stderr, "[XMVP]   caller 0x%08X x%lld\n", callers[k], caller_n[k]);
            memset(callers, 0, sizeof(callers)); memset(caller_n, 0, sizeof(caller_n));
            win_start = t0; win_calls = win_ticks = win_max = 0;
        }
    }
    if (dt * 1000 / freq >= xmv_prof_ms) {
        static const char *const names[XP_COUNT] = { "frame", "idct", "mc", "clear", "sub_00240683", "sub_002415BA" };
        fprintf(stderr, "[XMVP] t=%.3f s, %.1f ms:", (double)t0 / (double)freq, (double)dt * 1000.0 / (double)freq);
        for (i = XP_IDCT; i < XP_COUNT; i++)
            fprintf(stderr, " %s %lldx %.1f ms,", names[i], xmv_prof_calls[i], (double)xmv_prof_ticks[i] * 1000.0 / (double)freq);
        fprintf(stderr, "\n");
    }
}
