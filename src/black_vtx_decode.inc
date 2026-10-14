/* Vertex position decode (guest sub_000205A0), included by recomp_manual.c.
 *
 * thiscall-like: ecx = a mesh header, edx = a vertex number, eax = where to put the result (16 bytes). The header has three
 * signed bytes at +0xA..+0xC (a coarse position) and a pointer at +4 to an array of three unsigned 16-bit words per vertex
 * (the fine position). The result is
 *     { (float)fine[0] / C2 * C3 + (float)coarse[0] * C1, ... [1], ... [2], <the fourth lane> }
 * with C1, C2, C3 the floats at 0x295D84, 0x295D80 and 0x295D8C. The recompiled function builds the two vectors in guest
 * stack slots a scalar at a time and loads each back as one 16-byte vector: a store-forwarding stall on every call, and it
 * is called for every vertex of the meshes it decodes (a tenth of the main thread). The fourth lane the original computes
 * from an uninitialised stack word; here it is zero. The caller reads three components.
 *
 * RECOMP_BLACK_NATIVE_VTX=0 runs the generated function; =verify runs both and counts differing results. */
extern void sub_000205A0_gen(void);

void sub_000205A0(void)
{
    static int mode = -1;                 /* 0 generated, 1 native, 2 verify */
    uint32_t out = g_eax, self = g_ecx, idx = g_edx, sp = g_esp;
    const uint8_t *m = (const uint8_t *)g_xbox_mem_offset;
    __m128 r;
    if (mode < 0) {
        const char *e = getenv("RECOMP_BLACK_NATIVE_VTX");
        mode = e && *e == '0' ? 0 : e && !strcmp(e, "verify") ? 2 : 1;
    }
    if (mode == 0) { sub_000205A0_gen(); return; }
    {
        const uint16_t *fine = (const uint16_t *)(m + GUEST32(self + 4) + (size_t)idx * 6);
        float c1, c2, c3;
        __m128 t, v;
        memcpy(&c1, m + BLACK_VA(0x295D84u), 4);
        memcpy(&c2, m + BLACK_VA(0x295D80u), 4);
        memcpy(&c3, m + BLACK_VA(0x295D8Cu), 4);
        t = _mm_setr_ps((float)(int32_t)(int8_t)m[self + 0xA], (float)(int32_t)(int8_t)m[self + 0xB],
                        (float)(int32_t)(int8_t)m[self + 0xC], 0.0f);
        v = _mm_setr_ps((float)(int32_t)fine[0], (float)(int32_t)fine[1], (float)(int32_t)fine[2], 0.0f);
        r = _mm_add_ps(_mm_mul_ps(_mm_div_ps(v, _mm_set1_ps(c2)), _mm_set1_ps(c3)), _mm_mul_ps(t, _mm_set1_ps(c1)));
    }
    if (mode == 2) {
        static unsigned long long calls, bad;
        float have[4], want[4];
        _mm_storeu_ps(have, r);
        g_eax = out; g_ecx = self; g_edx = idx; g_esp = sp;
        sub_000205A0_gen();
        memcpy(want, m + out, 16);
        calls++;
        if (memcmp(have, want, 12) != 0 && bad++ < 8)
            fprintf(stderr, "[BLACK] vertex decode differs: %g %g %g vs %g %g %g\n", have[0], have[1], have[2], want[0], want[1], want[2]);
        if ((calls & (calls - 1)) == 0 && calls > 1000000) fprintf(stderr, "[BLACK] vertex decode: %llu calls, %llu differ\n", calls, bad);
        return;
    }
    _mm_storeu_ps((float *)(m + out), r);
    g_eax = out;
    g_esp = sp + 4;
}
