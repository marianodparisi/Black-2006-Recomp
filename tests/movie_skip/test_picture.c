#include <assert.h>
#ifdef NDEBUG
#error Movie picture assertions must remain enabled in Release builds.
#endif
#include "fb_present_policy.h"

int main(void)
{
    unsigned mode, nearest;
    FbPresentRect r;
    /* The same source picture is fitted at native and supersampled raster
     * sizes. Screen-height fitting must override integer and stretch modes. */
    for (mode = FB_SCALE_FIT; mode <= FB_SCALE_INTEGER; ++mode)
        for (nearest = 0; nearest <= 1; ++nearest) {
            r = fb_present_rect(3440, 1440, 640, 480, mode, nearest, 20, 11);
            assert(r.x == 411 && r.y == 0 && r.width == 2618 && r.height == 1440);
            assert(r.nearest == nearest);
            r = fb_present_rect(3440, 1440, 5040, 2160, mode, nearest, 20, 11);
            assert(r.x == 411 && r.y == 0 && r.width == 2618 && r.height == 1440);
            r = fb_present_rect(1720, 720, 640, 480, mode, nearest, 20, 11);
            assert(r.x == 205 && r.y == 0 && r.width == 1309 && r.height == 720);
        }
    /* A narrower window fits the full width without cropping content. */
    r = fb_present_rect(1280, 720, 640, 480, FB_SCALE_STRETCH, 0, 20, 11);
    assert(r.x == 0 && r.y == 8 && r.width == 1280 && r.height == 704);
    /* Ordinary movie and gameplay metadata keep their previous behavior. */
    r = fb_present_rect(3440, 1440, 640, 480, FB_SCALE_INTEGER, 1, 4, 3);
    assert(r.x == 760 && r.y == 0 && r.width == 1920 && r.height == 1440);
    r = fb_present_rect(3440, 1440, 5040, 2160, FB_SCALE_INTEGER, 1, 21, 9);
    assert(r.x == 40 && r.y == 0 && r.width == 3360 && r.height == 1440);
    /* The CPU renderer can emit this diagnostic raster size as ordinary
     * aspect metadata. It must keep every row and retain integer scaling. */
    assert(!fb_cinematic_crop(640, 352));
    r = fb_present_rect(3440, 1440, 640, 352, FB_SCALE_INTEGER, 1, 640, 352);
    assert(r.x == 440 && r.y == 16 && r.width == 2560 && r.height == 1408);
    r = fb_present_rect(3440, 1440, 640, 352, FB_SCALE_STRETCH, 1, 640, 352);
    assert(r.x == 0 && r.y == 0 && r.width == 3440 && r.height == 1440);
    return 0;
}
