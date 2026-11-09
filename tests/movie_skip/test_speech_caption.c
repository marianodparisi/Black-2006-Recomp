/* Gameplay speech captions: the real pc_speech_subtitle.c on a test clock, with a caption table written here. */
#include "pc_speech_subtitle.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static int shown(uint64_t now, int paused)
{
    uint16_t out[PC_SPEECH_SUBTITLE_CHARS];
    pc_speech_subtitle_test_time(now);
    return pc_speech_subtitle(out, PC_SPEECH_SUBTITLE_CHARS, paused);
}

int main(void)
{
    FILE *table;
    CreateDirectoryA("subtitles", NULL);
    CreateDirectoryA("subtitles\\en", NULL);
    table = fopen("subtitles/en/gameplay.tsv", "wb");
    if (!table) return 2;
    fputs("# level\tsector\tsectors\trate\tend_ms\tcaption\n"
          "0\t9248\t336\t33169\t17464\tThe opening montage, whose end the game never reports.\n"
          "0\t16\t32\t32000\t3448\tFinal sweep, no contact. Over.\n", table);
    fclose(table);
    _putenv_s("RECOMP_LANGUAGE", "en-US");

    /* Never stopped: the last page stays briefly past the clip's length, then leaves. */
    pc_speech_subtitle_test_time(1000);
    pc_speech_subtitle_start(0x100u, 0, 9248, 336, 33169);
    CHECK(shown(1000, 0));
    CHECK(shown(1000 + 17464 + 1000, 0));
    CHECK(!shown(1000 + 17464 + 1600, 0));
    CHECK(!shown(1000 + 17464 + 60000, 0));

    /* Paused time does not count towards it. */
    pc_speech_subtitle_test_time(100000);
    pc_speech_subtitle_start(0x200u, 0, 16, 32, 32000);
    CHECK(shown(100000, 0));
    CHECK(!shown(101000, 1));            /* paused at 1 s: not drawn */
    CHECK(!shown(130000, 1));            /* 29 s later, still paused */
    CHECK(shown(130000, 0));             /* resumed: 1 s of 3.4 s played */
    CHECK(shown(130000 + 2448 + 1000, 0));
    CHECK(!shown(130000 + 2448 + 1600, 0));

    /* A reported stop ends it at once. */
    pc_speech_subtitle_test_time(200000);
    pc_speech_subtitle_start(0x300u, 0, 16, 32, 32000);
    CHECK(shown(200500, 0));
    pc_speech_subtitle_stop(0x300u);
    CHECK(!shown(200600, 0));

    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    else printf("speech caption checks passed\n");
    return failures != 0;
}
