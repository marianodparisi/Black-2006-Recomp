/* Retail BLACK speech stream groups: 0x151AA0 binds an SSH entry (+0x40)
 * and its bank UID (+0x20/+0x24); 0x14EB10 starts its decoded voices.
 * Neither SSH loads nor 0x1411C0's prefetch requests display a caption.
 * Mission banks share UID 90BC79C164AE0000, so the current level is part
 * of the key. The first mission montage is Ambience[4]; other ambience and
 * global barks stay separate.
 * Generated definitions are renamed _gen; all existing calls keep these
 * wrappers, including the generated dispatch table. */
extern void sub_0014EB10_gen(void);
extern void sub_0014EBF0_gen(void);
extern void sub_00146EA0_gen(void);

void sub_0014EB10(void)
{
    const uint32_t group = black_pc_ram_span(g_esp, 8) ? GUEST32(g_esp + 4) : 0;
    unsigned level = 99, sector = 0, sectors = 0, rate = 0;
    int speech = 0;
    if (black_pc_ram_span(group, 0x68) &&
        ((GUEST32(group + 0x20) == 0x64AE0000u && GUEST32(group + 0x24) == 0x90BC79C1u) ||
         (GUEST32(group + 0x20) == 0x8E6D9000u && GUEST32(group + 0x24) == 0x4F5C5EAFu))) {
        const uint32_t entry = GUEST32(group + 0x40);
        const uint32_t world = GUEST32(BLACK_VA(0x002D45F0u));
        if (black_pc_ram_span(entry, 8) && black_pc_ram_span(world, 0x2020D)) {
            level = GUEST8(world + 0x2020C);
            sector = GUEST32(entry) & 0xFFFFu;
            sectors = GUEST32(entry) >> 16;
            rate = GUEST32(entry + 4);
            /* "Four days earlier" is the stereo montage in Ambience[4],
             * not the alternate takes in the mission speech bank. The bank
             * UID is shared by level ambience, so admit only this exact clip. */
            speech = GUEST32(group + 0x20) == 0x64AE0000u ||
                (level == 0 && sector == 9248 && sectors == 336 && rate == 33169);
        }
    }
    sub_0014EB10_gen();
    if (speech) {
        black_movie_skip_font();
        pc_speech_subtitle_start(group, level, sector, sectors, rate);
    } else pc_speech_subtitle_stop(group);
}

void sub_0014EBF0(void)
{
    const uint32_t group = g_esi;
    sub_0014EBF0_gen();
    if (black_pc_ram_span(group, 0x18) && GUEST32(group + 0x14) >= 8)
        pc_speech_subtitle_stop(group);
}

void sub_00146EA0(void)
{
    const uint32_t group = black_pc_ram_span(g_esp, 8) ? GUEST32(g_esp + 4) : 0;
    sub_00146EA0_gen();
    if (g_eax & 0xFFu) pc_speech_subtitle_stop(group);
}
