/* The PC video settings model and the native Video Settings page, against the production sources. Nothing is mocked:
 * keys and the pad go through the real pc_input filter, the overlay is rendered by the real GDI code, and the settings
 * file is a real file. Only time (pc_input) and the environment are controlled. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pc_video.h"
#include "pc_video_menu.h"
#include "pc_input.h"

static unsigned checks;
#define CHECK(e) do { checks++; if (!(e)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #e); exit(1); } } while (0)

static int test_fire_control(int slot) { return slot == 9 ? 23 : slot == 11 ? 17 : -1; }
static int test_aim_control(int slot) { return slot == 16 ? 22 : -1; }

static void test_sustained_fire_survives_gameplay_gap(void)
{
    uint8_t out[20] = {0,20};
    uint64_t token;
    int i;
    _putenv_s("RECOMP_BLACK_INPUT_MODE", "auto");
    _putenv_s("RECOMP_BLACK_BIND_FIRE", "Mouse1");
    _putenv_s("RECOMP_INPUT_TRACE", "");
    _putenv_s("RECOMP_INPUT_SCRIPT", "");
    _putenv_s("RECOMP_PAD_SCRIPT", "");
    pc_input_test_reset();
    pc_input_test_set_time(100000);
    pc_input_set_control_resolver(test_fire_control);
    pc_input_gameplay_tick();
    CHECK(pc_input_gameplay_active());
    pc_input_mouse_button_event(0, 1);
    pc_input_apply_report(out, 0);
    CHECK(out[11] == 0xFF && out[4] == 0);
    pc_input_frame_complete();
    pc_input_frame_complete(); /* a rendered frame completed before the next camera update */
    memset(out + 2, 0, 18);
    pc_input_apply_report(out, 0); /* the USB poll can land in the camera-update gap */
    CHECK(out[11] == 0xFF && out[4] == 0);
    pc_input_gameplay_tick();
    memset(out + 2, 0, 18);
    pc_input_apply_report(out, 0);
    CHECK(out[11] == 0xFF && out[4] == 0);
    for (i = 0; i < 20; ++i) {
        pc_input_frame_complete();
        memset(out + 2, 0, 18);
        pc_input_apply_report(out, 0);
        CHECK(out[11] == 0xFF && out[4] == 0);
        pc_input_frame_complete();
        memset(out + 2, 0, 18);
        pc_input_apply_report(out, 0);
        CHECK(out[11] == 0xFF && out[4] == 0);
        pc_input_gameplay_tick();
        memset(out + 2, 0, 18);
        pc_input_apply_report(out, 0);
        CHECK(out[11] == 0xFF && out[4] == 0);
    }
    pc_input_frame_complete(); /* the current frame was consumed by the renderer */
    pc_input_frame_complete(); /* a later frame completed without a gameplay camera tick */
    pc_input_test_set_time(100300); /* a short update gap must not expire held Fire */
    memset(out + 2, 0, 18);
    pc_input_apply_report(out, 0);
    CHECK(out[11] == 0xFF && out[4] == 0);
    pc_input_test_set_time(100000);
    pc_input_mouse_button_event(0, 0);
    memset(out + 2, 0, 18);
    pc_input_apply_report(out, 0);
    CHECK(out[11] == 0 && out[4] == 0);

    /* Taking mouse capture during an accepted gameplay hold must not turn
     * that hold into a menu click or cut automatic fire off after one burst. */
    pc_input_test_reset();
    pc_input_test_set_time(100000);
    pc_input_set_control_resolver(test_fire_control);
    pc_input_gameplay_tick();
    token = pc_input_native_sample_begin();
    pc_input_mouse_button_event(0, 1);
    memset(out + 2, 0, 18);
    pc_input_apply_report(out, 0);
    CHECK(out[11] == 0xFF && out[4] == 0);
    pc_input_native_sample_end(token, 1u << 23);
    pc_input_frame_complete();
    pc_input_capture_changed(1);
    memset(out + 2, 0, 18);
    pc_input_apply_report(out, 0);
    CHECK(out[11] == 0xFF && out[4] == 0);
    pc_input_mouse_button_event(0, 0);
    memset(out + 2, 0, 18);
    pc_input_apply_report(out, 0);
    CHECK(out[11] == 0 && out[4] == 0);

    pc_input_test_reset();
    pc_input_test_set_time(100000);
    pc_input_set_control_resolver(test_fire_control);
    pc_input_gameplay_tick();
    pc_input_key_event(0x13, 0, 1, 0); /* R is a discrete reload action. */
    memset(out + 2, 0, 18);
    pc_input_apply_report(out, 0);
    CHECK(out[5] == 0xFF);
    pc_input_frame_complete();
    pc_input_frame_complete();
    pc_input_gameplay_tick();
    memset(out + 2, 0, 18);
    pc_input_apply_report(out, 0);
    CHECK(out[5] == 0);
    pc_input_test_reset();
}

/* A frame finished before the next camera update ends gameplay for keys and
 * buttons, but must neither release the pointer nor discard the mouse motion
 * gathered meanwhile, while the last camera update is recent. A real gap
 * (here 300 ms) still releases both. */
static void test_look_survives_camera_gap(void)
{
    double yaw, pitch;
    _putenv_s("RECOMP_BLACK_INPUT_MODE", "auto");
    _putenv_s("RECOMP_INPUT_TRACE", "");
    _putenv_s("RECOMP_INPUT_SCRIPT", "");
    _putenv_s("RECOMP_PAD_SCRIPT", "");
    pc_input_test_reset();
    pc_input_test_set_time(100000);
    pc_input_gameplay_tick();
    pc_input_frame_complete();
    CHECK(pc_input_capture_wanted());
    pc_input_capture_changed(1);
    pc_input_mouse_motion(100, 50);
    pc_input_frame_complete();             /* no camera update in this frame */
    CHECK(!pc_input_gameplay_active());    /* keys and buttons keep the original boundary */
    CHECK(pc_input_capture_wanted());      /* the pointer stays captured */
    pc_input_test_set_time(100020);
    pc_input_gameplay_tick();              /* control is back 20 ms later */
    CHECK(pc_input_consume_look(&yaw, &pitch) && yaw != 0.0 && pitch != 0.0);
    pc_input_mouse_motion(100, 50);
    pc_input_test_set_time(100300);
    pc_input_frame_complete();
    pc_input_frame_complete();             /* 300 ms without a camera update */
    CHECK(!pc_input_capture_wanted());
    pc_input_gameplay_tick();
    CHECK(!pc_input_consume_look(&yaw, &pitch)); /* stale motion is still discarded */
    pc_input_test_reset();
}

static void fire_test_reset(void)
{
    _putenv_s("RECOMP_BLACK_INPUT_MODE", "auto");
    _putenv_s("RECOMP_BLACK_BIND_FIRE", "Mouse1");
    _putenv_s("RECOMP_INPUT_TRACE", "");
    _putenv_s("RECOMP_INPUT_SCRIPT", "");
    _putenv_s("RECOMP_PAD_SCRIPT", "");
    pc_input_test_reset();
    pc_input_test_set_time(100000);
    pc_input_set_control_resolver(test_fire_control);
}

static unsigned fire_test_report(void)
{
    uint8_t out[20] = {0,20};
    pc_input_apply_report(out, 0);
    return out[11];
}

static unsigned aim_test_report(void)
{
    uint8_t out[20] = {0,20};
    pc_input_apply_report(out, 0);
    return out[10];
}

static void fire_test_enter_gap(void)
{
    pc_input_gameplay_tick();
    pc_input_frame_complete();
    pc_input_frame_complete();
}

#define FIRE_EXPECT(failure_count, expression) do { \
    ++checks; \
    if (!(expression)) { \
        ++(failure_count); \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
    } \
} while (0)

static void test_fire_ack_generation_guards(void)
{
    const uint32_t fire_bit = 1u << 23;
    unsigned failures = 0;
    unsigned check_start = checks;
    uint64_t token, older_token, newer_token, neutral_token, g2_token, stale_token, overwritten_token;
    unsigned initial, held, before_neutral, after_neutral, older_observed, newer_observed;
    unsigned gated_poll, next_press, g2_observed, stale_end_held, stale_ignored_report;
    unsigned overwritten_end_report, overwritten_neutral, after_overwritten_neutral;
    unsigned queued_reports[4], drained_neutrals[4], i;

    /* A completed Fire sample acknowledges the hold. Capturing the pointer
     * must preserve the hold without forgetting that acknowledgement. */
    fire_test_reset();
    pc_input_gameplay_tick();
    pc_input_frame_complete();
    pc_input_mouse_button_event(0, 1);
    token = pc_input_native_sample_begin(); /* the press predates this sample */
    initial = fire_test_report();
    pc_input_native_sample_end(token, fire_bit);
    pc_input_capture_changed(1);
    held = fire_test_report();
    pc_input_mouse_button_event(0, 0);
    pc_input_mouse_button_event(0, 1);
    token = pc_input_native_sample_begin();
    before_neutral = fire_test_report();
    pc_input_native_sample_end(token, before_neutral ? fire_bit : 0);
    after_neutral = fire_test_report();
    printf("Fire capture acknowledgement: initial=%u held=%u re-press=%u after-neutral=%u\n",
           initial, held, before_neutral, after_neutral);
    FIRE_EXPECT(failures, initial == 0xFF);
    FIRE_EXPECT(failures, held == 0xFF);
    FIRE_EXPECT(failures, before_neutral == 0);
    FIRE_EXPECT(failures, after_neutral == 0xFF);

    /* A Fire hold that starts during the short camera/frame gap becomes
     * acknowledged only when a sample token begins after that press. */
    fire_test_reset();
    fire_test_enter_gap();
    FIRE_EXPECT(failures, !pc_input_gameplay_active());
    pc_input_mouse_button_event(0, 1);
    token = pc_input_native_sample_begin(); /* eligible: this token includes the gap hold */
    initial = fire_test_report();
    pc_input_native_sample_end(token, fire_bit);
    pc_input_gameplay_tick();
    pc_input_mouse_button_event(0, 0);
    pc_input_mouse_button_event(0, 1);
    token = pc_input_native_sample_begin();
    before_neutral = fire_test_report();
    pc_input_native_sample_end(token, before_neutral ? fire_bit : 0);
    after_neutral = fire_test_report();
    printf("Fire gap acknowledgement: initial=%u re-press=%u after-neutral=%u\n",
           initial, before_neutral, after_neutral);
    FIRE_EXPECT(failures, initial == 0xFF);
    FIRE_EXPECT(failures, before_neutral == 0);
    FIRE_EXPECT(failures, after_neutral == 0xFF);

    /* If the gap hold is released before its eligible sample completes, the
     * old sample still observed Fire and must establish a neutral barrier. */
    fire_test_reset();
    fire_test_enter_gap();
    pc_input_mouse_button_event(0, 1);
    token = pc_input_native_sample_begin(); /* includes the first gap hold */
    initial = fire_test_report();
    pc_input_mouse_button_event(0, 0);
    pc_input_native_sample_end(token, initial ? fire_bit : 0);
    pc_input_mouse_button_event(0, 1);
    before_neutral = fire_test_report();
    token = pc_input_native_sample_begin();
    gated_poll = fire_test_report();
    pc_input_native_sample_end(token, gated_poll ? fire_bit : 0);
    after_neutral = fire_test_report();
    pc_input_mouse_button_event(0, 0);
    pc_input_mouse_button_event(0, 1);
    next_press = fire_test_report();
    printf("Fire gap release-before-end: old-report=%u gated-poll=%u after-neutral=%u next-press=%u\n",
           initial, gated_poll, after_neutral, next_press);
    FIRE_EXPECT(failures, initial == 0xFF);
    FIRE_EXPECT(failures, before_neutral == 0);
    FIRE_EXPECT(failures, gated_poll == 0);
    FIRE_EXPECT(failures, after_neutral == 0xFF);
    FIRE_EXPECT(failures, next_press == 0xFF);

    /* Replacing a gap hold while its sample is still in flight must retain
     * the old generation long enough to force neutral, without acknowledging
     * the newer Fire generation from that old sample's RT observation. */
    fire_test_reset();
    fire_test_enter_gap();
    pc_input_mouse_button_event(0, 1);
    token = pc_input_native_sample_begin(); /* token includes only the first hold */
    initial = fire_test_report();
    pc_input_mouse_button_event(0, 0);
    pc_input_mouse_button_event(0, 1);
    pc_input_native_sample_end(token, initial ? fire_bit : 0);
    before_neutral = fire_test_report();
    token = pc_input_native_sample_begin();
    gated_poll = fire_test_report();
    pc_input_native_sample_end(token, gated_poll ? fire_bit : 0);
    after_neutral = fire_test_report();
    pc_input_mouse_button_event(0, 0);
    pc_input_mouse_button_event(0, 1);
    next_press = fire_test_report();
    printf("Fire gap re-press-before-end: old-report=%u gated-poll=%u after-neutral=%u next-press=%u\n",
           initial, gated_poll, after_neutral, next_press);
    FIRE_EXPECT(failures, initial == 0xFF);
    FIRE_EXPECT(failures, before_neutral == 0);
    FIRE_EXPECT(failures, gated_poll == 0);
    FIRE_EXPECT(failures, after_neutral == 0xFF);
    FIRE_EXPECT(failures, next_press == 0xFF);

    /* A sample that began before the press cannot acknowledge that press,
     * even if the later report contains Fire. */
    fire_test_reset();
    pc_input_gameplay_tick();
    pc_input_frame_complete();
    token = pc_input_native_sample_begin();
    pc_input_mouse_button_event(0, 1);
    initial = fire_test_report();
    pc_input_native_sample_end(token, fire_bit);
    pc_input_mouse_button_event(0, 0);
    pc_input_mouse_button_event(0, 1);
    token = pc_input_native_sample_begin();
    before_neutral = fire_test_report();
    pc_input_native_sample_end(token, before_neutral ? fire_bit : 0);
    printf("Fire press-after-token: first-report=%u re-press=%u\n", initial, before_neutral);
    FIRE_EXPECT(failures, initial == 0xFF);
    FIRE_EXPECT(failures, before_neutral == 0xFF);

    /* Leave a sample for the acknowledged first press in flight. After a
     * neutral poll and a newer press, its late completion must not alias to
     * the newer generation just because both presses map to the same RT bit. */
    fire_test_reset();
    pc_input_gameplay_tick();
    pc_input_frame_complete();
    pc_input_mouse_button_event(0, 1);
    token = pc_input_native_sample_begin();
    initial = fire_test_report();
    pc_input_native_sample_end(token, fire_bit);
    older_token = pc_input_native_sample_begin();
    older_observed = fire_test_report(); /* captured for the first hold; completion is delayed */
    pc_input_mouse_button_event(0, 0);
    pc_input_mouse_button_event(0, 1);
    token = pc_input_native_sample_begin();
    before_neutral = fire_test_report();
    gated_poll = before_neutral;
    pc_input_native_sample_end(token, before_neutral ? fire_bit : 0);
    after_neutral = fire_test_report();
    pc_input_native_sample_end(older_token, older_observed ? fire_bit : 0);
    held = fire_test_report();
    pc_input_mouse_button_event(0, 0);
    pc_input_mouse_button_event(0, 1);
    next_press = fire_test_report();
    printf("Fire late older sample: first=%u older-sample=%u gated-poll=%u after-neutral=%u late-end=%u next-press=%u\n",
           initial, older_observed, gated_poll, after_neutral, held, next_press);
    FIRE_EXPECT(failures, initial == 0xFF);
    FIRE_EXPECT(failures, older_observed == 0xFF);
    FIRE_EXPECT(failures, gated_poll == 0);
    FIRE_EXPECT(failures, after_neutral == 0xFF);
    FIRE_EXPECT(failures, held == 0xFF);
    FIRE_EXPECT(failures, next_press == 0xFF);

    /* Two overlapping native samples can both observe Fire generation 1.
     * Once the newer sample acknowledges it and generation 2 is later
     * acknowledged, completing the older duplicate must be harmless. */
    fire_test_reset();
    pc_input_gameplay_tick();
    pc_input_frame_complete();
    pc_input_mouse_button_event(0, 1);
    older_token = pc_input_native_sample_begin();
    older_observed = fire_test_report();
    newer_token = pc_input_native_sample_begin();
    newer_observed = fire_test_report();
    pc_input_native_sample_end(newer_token, newer_observed ? fire_bit : 0);
    pc_input_mouse_button_event(0, 0);
    pc_input_mouse_button_event(0, 1);
    neutral_token = pc_input_native_sample_begin();
    before_neutral = fire_test_report();
    pc_input_native_sample_end(neutral_token, before_neutral ? fire_bit : 0);
    g2_token = pc_input_native_sample_begin();
    g2_observed = fire_test_report();
    pc_input_native_sample_end(g2_token, g2_observed ? fire_bit : 0);
    pc_input_native_sample_end(older_token, older_observed ? fire_bit : 0);
    stale_end_held = fire_test_report();
    printf("Fire duplicate snapshot: older=%u newer=%u neutral=%u generation2=%u after-stale-end=%u\n",
           older_observed, newer_observed, before_neutral, g2_observed, stale_end_held);
    FIRE_EXPECT(failures, older_observed == 0xFF);
    FIRE_EXPECT(failures, newer_observed == 0xFF);
    FIRE_EXPECT(failures, before_neutral == 0);
    FIRE_EXPECT(failures, g2_observed == 0xFF);
    FIRE_EXPECT(failures, stale_end_held == 0xFF);

    /* A token more than four sample starts old has been overwritten in the
     * bounded snapshot ring. Its late end must not act on the newer snapshot
     * that occupies the same ring slot. */
    fire_test_reset();
    pc_input_gameplay_tick();
    pc_input_frame_complete();
    pc_input_mouse_button_event(0, 1);
    stale_token = pc_input_native_sample_begin();
    older_observed = fire_test_report();
    for (i = 0; i < 4; ++i) {
        overwritten_token = pc_input_native_sample_begin();
        newer_observed = fire_test_report();
    }
    pc_input_mouse_button_event(0, 0);
    pc_input_mouse_button_event(0, 1);
    pc_input_native_sample_end(stale_token, older_observed ? fire_bit : 0);
    stale_ignored_report = fire_test_report();
    pc_input_native_sample_end(overwritten_token, newer_observed ? fire_bit : 0);
    overwritten_end_report = fire_test_report();
    neutral_token = pc_input_native_sample_begin();
    overwritten_neutral = fire_test_report();
    pc_input_native_sample_end(neutral_token, overwritten_neutral ? fire_bit : 0);
    after_overwritten_neutral = fire_test_report();
    printf("Fire overwritten sample ticket: old=%u newer=%u stale-end=%u valid-end=%u neutral=%u next=%u\n",
           older_observed, newer_observed, stale_ignored_report, overwritten_end_report,
           overwritten_neutral, after_overwritten_neutral);
    FIRE_EXPECT(failures, older_observed == 0xFF);
    FIRE_EXPECT(failures, newer_observed == 0xFF);
    FIRE_EXPECT(failures, stale_ignored_report == 0xFF);
    FIRE_EXPECT(failures, overwritten_end_report == 0);
    FIRE_EXPECT(failures, overwritten_neutral == 0);
    FIRE_EXPECT(failures, after_overwritten_neutral == 0xFF);

    /* Four queued Aim taps fill the per-source queue; the fifth press remains
     * physically held without a queue entry. An observed native hold must
     * still acknowledge it so release/re-press waits for a neutral sample. */
    _putenv_s("RECOMP_BLACK_BIND_AIM", "Mouse2");
    fire_test_reset();
    pc_input_set_control_resolver(test_aim_control);
    pc_input_gameplay_tick();
    pc_input_frame_complete();
    for (i = 0; i < 4; ++i) {
        pc_input_mouse_button_event(1, 1);
        pc_input_mouse_button_event(1, 0);
    }
    pc_input_mouse_button_event(1, 1); /* fifth press stays physically held */
    for (i = 0; i < 4; ++i) {
        neutral_token = pc_input_native_sample_begin();
        queued_reports[i] = aim_test_report();
        pc_input_native_sample_end(neutral_token, queued_reports[i] ? 1u << 22 : 0);
        neutral_token = pc_input_native_sample_begin();
        drained_neutrals[i] = aim_test_report();
        pc_input_native_sample_end(neutral_token, drained_neutrals[i] ? 1u << 22 : 0);
        FIRE_EXPECT(failures, queued_reports[i] == 0xFF);
        FIRE_EXPECT(failures, drained_neutrals[i] == 0);
    }
    neutral_token = pc_input_native_sample_begin();
    held = aim_test_report();
    pc_input_native_sample_end(neutral_token, held ? 1u << 22 : 0);
    pc_input_mouse_button_event(1, 0);
    pc_input_mouse_button_event(1, 1);
    neutral_token = pc_input_native_sample_begin();
    before_neutral = aim_test_report();
    pc_input_native_sample_end(neutral_token, before_neutral ? 1u << 22 : 0);
    neutral_token = pc_input_native_sample_begin();
    after_neutral = aim_test_report();
    pc_input_native_sample_end(neutral_token, after_neutral ? 1u << 22 : 0);
    printf("Aim full-queue held acknowledgement: queued=%u,%u,%u,%u neutral=%u,%u,%u,%u fifth-held=%u re-press=%u after-neutral=%u\n",
           queued_reports[0], queued_reports[1], queued_reports[2], queued_reports[3],
           drained_neutrals[0], drained_neutrals[1], drained_neutrals[2], drained_neutrals[3],
           held, before_neutral, after_neutral);
    FIRE_EXPECT(failures, held == 0xFF);
    FIRE_EXPECT(failures, before_neutral == 0);
    FIRE_EXPECT(failures, after_neutral == 0xFF);
    _putenv_s("RECOMP_BLACK_BIND_AIM", "");

    printf("Fire acknowledgement generation: %u checks, %u failures\n",
           checks - check_start, failures);
    CHECK(failures == 0);
}

#undef FIRE_EXPECT

static char ini_path[MAX_PATH + 32];
static unsigned hook_calls, hook_w, hook_h, hook_ow, hook_oh;
static void hook(unsigned w, unsigned h, unsigned ow, unsigned oh) { hook_calls++; hook_w = w; hook_h = h; hook_ow = ow; hook_oh = oh; }

/* Compare values rather than padding bytes, which an optimized structure assignment may leave unspecified. */
static int same_settings(const PcVideoSettings *a, const PcVideoSettings *b)
{
    return a->internal_height == b->internal_height && a->window_width == b->window_width && a->window_height == b->window_height &&
           a->borderless == b->borderless && a->scale == b->scale && a->nearest == b->nearest && a->vsync == b->vsync &&
           a->fps_limit == b->fps_limit && a->aspect == b->aspect && a->fov == b->fov && a->motion_blur_off == b->motion_blur_off &&
           a->aa == b->aa && a->af == b->af && a->dof == b->dof && a->sharpen == b->sharpen &&
           a->ao_method == b->ao_method && a->ao_quality == b->ao_quality &&
           a->render_override_w == b->render_override_w && a->render_override_h == b->render_override_h;
}

static void clear_env(void)
{
    static const char *const names[] = {
        "RECOMP_WINDOW_WIDTH", "RECOMP_WINDOW_HEIGHT", "RECOMP_WINDOW_MODE", "RECOMP_RENDER_WIDTH", "RECOMP_RENDER_HEIGHT",
        "RECOMP_SCALE_MODE", "RECOMP_PRESENT_FILTER", "RECOMP_VSYNC", "RECOMP_FPS_LIMIT", "RECOMP_BLACK_ASPECT",
        "RECOMP_BLACK_FOV", "RECOMP_BLACK_MOTION_BLUR", "RECOMP_INTERNAL_HEIGHT", "RECOMP_VIDEO_INI",
        "RECOMP_AA", "RECOMP_AF", "RECOMP_DOF", "RECOMP_SHARPEN", "RECOMP_SSAO", "RECOMP_AO_METHOD", "RECOMP_AO_QUALITY"
    };
    unsigned i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) _putenv_s(names[i], "");
}

static void fresh(void)
{
    DeleteFileA(ini_path);
    clear_env();
    pc_video_test_reset(ini_path);
}

static unsigned size_of(unsigned height, unsigned aspect, unsigned *h_out)
{
    PcVideoSettings s;
    unsigned w, h;
    pc_video_defaults(&s);
    s.internal_height = height; s.aspect = aspect;
    pc_video_render_size_for(&s, &w, &h);
    *h_out = h;
    return w;
}

static void test_model(void)
{
    PcVideoSettings s, t;
    unsigned h, w;
    char text[1024];

    /* The defaults: 720p internal resolution, which for the original 4:3 camera is 960x720. */
    fresh();
    pc_video_get(&s);
    CHECK(s.internal_height == 720 && s.window_width == 1280 && s.window_height == 960);
    CHECK(!s.borderless && s.scale == PCV_SCALE_FIT && !s.nearest && s.vsync && !s.fps_limit);
    CHECK(s.aspect == PCV_ASPECT_ORIGINAL && s.fov == 0.0 && !s.motion_blur_off);
    CHECK(s.aa == PCV_AA_FXAA && s.af == 16 && s.dof == PCV_STEP_OFF && s.sharpen == PCV_STEP_OFF);
    pc_video_render_size(&w, &h);
    CHECK(w == 960 && h == 720);
    pc_video_raster_size(&w, &h);
    CHECK(w == 960 && h == 720);                                        /* only SSAA rasterises larger */
    CHECK(pc_video_locked() == 0 && pc_video_generation() == 0 && !pc_video_live_resize());

    /* The raster follows the camera's shape. */
    CHECK(size_of(480, PCV_ASPECT_ORIGINAL, &h) == 640 && h == 480);
    CHECK(size_of(720, PCV_ASPECT_4_3, &h) == 960 && h == 720);
    CHECK(size_of(720, PCV_ASPECT_16_9, &h) == 1280 && h == 720);
    CHECK(size_of(720, PCV_ASPECT_21_9, &h) == 1680 && h == 720);
    CHECK(size_of(720, PCV_ASPECT_32_9, &h) == 2560 && h == 720);
    CHECK(size_of(1080, PCV_ASPECT_16_9, &h) == 1920 && h == 1080);
    CHECK(size_of(1440, PCV_ASPECT_21_9, &h) == 3360 && h == 1440);
    CHECK(size_of(2160, PCV_ASPECT_32_9, &h) == 7680 && h == 2160);
    CHECK(size_of(480, PCV_ASPECT_16_9, &h) == 854 && h == 480);          /* rounded up to an even width */

    /* Super-sampling rasterises at twice the width and height and presents at the internal resolution, when that fits. */
    pc_video_defaults(&s);
    s.aa = PCV_AA_SSAA;
    CHECK(pc_video_ssaa_factor_for(&s) == 2);
    pc_video_raster_size_for(&s, &w, &h); CHECK(w == 1920 && h == 1440);
    pc_video_render_size_for(&s, &w, &h); CHECK(w == 960 && h == 720);
    s.internal_height = 1080; s.aspect = PCV_ASPECT_16_9;
    pc_video_raster_size_for(&s, &w, &h); CHECK(w == 3840 && h == 2160);
    s.internal_height = 2160;                                             /* 7680x4320 is past the renderer's limits */
    CHECK(pc_video_ssaa_factor_for(&s) == 1);
    pc_video_raster_size_for(&s, &w, &h); CHECK(w == 3840 && h == 2160);
    s.internal_height = 1440; s.aspect = PCV_ASPECT_21_9;                 /* 6720x2880 is 19 million pixels */
    CHECK(pc_video_ssaa_factor_for(&s) == 1);
    s.aa = PCV_AA_FXAA; s.internal_height = 720; s.aspect = PCV_ASPECT_16_9;
    CHECK(pc_video_ssaa_factor_for(&s) == 1);
    s.aa = PCV_AA_SSAA; s.render_override_w = 3440; s.render_override_h = 1440;
    CHECK(pc_video_ssaa_factor_for(&s) == 1);                             /* an explicit raster size is taken as it is */

    /* Text round trip; bad keys and values are skipped, good ones around them still apply. */
    pc_video_defaults(&s);
    s.internal_height = 1080; s.window_width = 3440; s.window_height = 1440; s.borderless = 1; s.scale = PCV_SCALE_INTEGER;
    s.nearest = 1; s.vsync = 0; s.fps_limit = 144; s.aspect = PCV_ASPECT_21_9; s.fov = 90; s.motion_blur_off = 1;
    s.aa = PCV_AA_SSAA; s.af = 4; s.dof = PCV_STEP_MEDIUM; s.sharpen = PCV_STEP_HIGH;
    CHECK(pc_video_format(&s, text, sizeof(text)) > 0);
    pc_video_defaults(&t);
    CHECK(strstr(text, "aa=ssaa\n") && strstr(text, "af=4\n") && strstr(text, "dof=medium\n") && strstr(text, "sharpen=high\n"));
    CHECK(pc_video_parse(text, &t) == 17);
    CHECK(same_settings(&s, &t));
    pc_video_defaults(&t);
    CHECK(pc_video_parse("[video]\r\n; comment\r\ninternal_height=999\r\nwindow_width=1600\r\nbogus=1\r\nfov=500\r\nfov=72.5\r\n"
                         "fps_limit=77\r\naspect=5:4\r\nvsync=maybe\r\nscale = stretch \r\naf=3\r\naa=taa\r\ndof=extreme\r\nsharpen=1\r\n", &t) == 3);
    CHECK(t.internal_height == 720 && t.window_width == 1600 && t.fov == 72.5 && t.fps_limit == 0 &&
          t.aspect == PCV_ASPECT_ORIGINAL && t.vsync == 1 && t.scale == PCV_SCALE_STRETCH);
}

static void test_ambient_occlusion(void)
{
    static const char *const methods[] = { "off", "ssao", "hbao", "hbao_plus", "gtao" };
    static const char *const qualities[] = { "low", "medium", "high", "ultra" };
    PcVideoSettings s, t;
    char text[1024], setting[96], value[96];
    unsigned method, quality, generation;
    FILE *f;
    fresh();
    pc_video_get(&s);
    CHECK(s.ao_method == PCV_AO_OFF && s.ao_quality == PCV_AOQ_HIGH);
    for (method = 0; method < PCV_AO_COUNT; method++) {
        for (quality = 0; quality < PCV_AOQ_COUNT; quality++) {
            snprintf(setting, sizeof(setting), "ao_method=%s\nao_quality=%s\n", methods[method], qualities[quality]);
            CHECK(pc_video_parse(setting, &s) == 2);
            CHECK(s.ao_method == method && s.ao_quality == quality);
            CHECK(pc_video_format(&s, text, sizeof(text)) < sizeof(text));
            pc_video_defaults(&t);
            CHECK(pc_video_parse(text, &t) == 17 && same_settings(&s, &t));
        }
    }
    CHECK(pc_video_parse("ao_method=GTAO\nao_quality=ULTRA\n", &s) == 2);
    CHECK(s.ao_method == PCV_AO_GTAO && s.ao_quality == PCV_AOQ_ULTRA);
    CHECK(pc_video_parse("ao_method=invalid\nao_quality=off\nssao=invalid\n", &s) == 0);
    CHECK(s.ao_method == PCV_AO_GTAO && s.ao_quality == PCV_AOQ_ULTRA);
    for (quality = 0; quality < PCV_AOQ_COUNT; quality++) {
        snprintf(setting, sizeof(setting), "ssao=%s\n", qualities[quality]);
        CHECK(pc_video_parse(setting, &s) == 1 && s.ao_method == PCV_AO_SSAO && s.ao_quality == quality);
    }
    CHECK(pc_video_parse("ssao=off\n", &s) == 1 && s.ao_method == PCV_AO_OFF && s.ao_quality == PCV_AOQ_ULTRA);
    CHECK(pc_video_parse("ssao=low\nao_method=hbao_plus\nao_quality=high\n", &s) == 3);
    CHECK(s.ao_method == PCV_AO_HBAO_PLUS && s.ao_quality == PCV_AOQ_HIGH);
    CHECK(pc_video_set(&s));
    pc_video_test_reset(ini_path); pc_video_get(&t);
    CHECK(t.ao_method == s.ao_method && t.ao_quality == s.ao_quality);
    generation = pc_video_generation();
    t.ao_method = PCV_AO_COUNT;
    CHECK(!pc_video_set(&t) && pc_video_generation() == generation);
    t = s; t.ao_quality = PCV_AOQ_COUNT;
    CHECK(!pc_video_set(&t) && pc_video_generation() == generation);

    /* Per-field environment locks preserve the saved choice while pinning this process. */
    _putenv_s("RECOMP_AO_METHOD", "gtao");
    pc_video_test_reset(ini_path); pc_video_get(&t);
    CHECK(t.ao_method == PCV_AO_GTAO && t.ao_quality == PCV_AOQ_HIGH);
    CHECK((pc_video_locked() & (PCV_LOCK_AO_METHOD | PCV_LOCK_AO_QUALITY)) == PCV_LOCK_AO_METHOD);
    t.ao_method = PCV_AO_OFF; t.ao_quality = PCV_AOQ_LOW; CHECK(pc_video_set(&t));
    pc_video_get(&t); CHECK(t.ao_method == PCV_AO_GTAO && t.ao_quality == PCV_AOQ_LOW);
    clear_env(); pc_video_test_reset(ini_path); pc_video_get(&t);
    CHECK(t.ao_method == PCV_AO_HBAO_PLUS && t.ao_quality == PCV_AOQ_LOW);
    _putenv_s("RECOMP_AO_QUALITY", "ultra");
    pc_video_test_reset(ini_path); pc_video_get(&t);
    CHECK((pc_video_locked() & (PCV_LOCK_AO_METHOD | PCV_LOCK_AO_QUALITY)) == PCV_LOCK_AO_QUALITY);
    t.ao_method = PCV_AO_SSAO; t.ao_quality = PCV_AOQ_HIGH; CHECK(pc_video_set(&t));
    pc_video_get(&t); CHECK(t.ao_method == PCV_AO_SSAO && t.ao_quality == PCV_AOQ_ULTRA);
    clear_env(); pc_video_test_reset(ini_path); pc_video_get(&t);
    CHECK(t.ao_method == PCV_AO_SSAO && t.ao_quality == PCV_AOQ_LOW);
    _putenv_s("RECOMP_SSAO", "medium"); _putenv_s("RECOMP_AO_METHOD", "hbao"); _putenv_s("RECOMP_AO_QUALITY", "ultra");
    pc_video_test_reset(ini_path); pc_video_get(&t);
    CHECK(t.ao_method == PCV_AO_HBAO && t.ao_quality == PCV_AOQ_ULTRA);
    CHECK((pc_video_locked() & (PCV_LOCK_AO_METHOD | PCV_LOCK_AO_QUALITY)) == (PCV_LOCK_AO_METHOD | PCV_LOCK_AO_QUALITY));
    _putenv_s("RECOMP_AO_METHOD", "invalid"); _putenv_s("RECOMP_AO_QUALITY", "invalid");
    pc_video_test_reset(ini_path); pc_video_get(&t);
    CHECK(t.ao_method == PCV_AO_OFF && t.ao_quality == PCV_AOQ_HIGH);
    clear_env();

    /* The English menu choices cycle through every method and quality without changing raster size. */
    fresh(); pc_video_set_resize_hook(hook); hook_calls = 0;
    CHECK(!strcmp(pc_video_menu_row_label(PCV_ROW_AO_METHOD), "AO METHOD"));
    CHECK(!strcmp(pc_video_menu_row_label(PCV_ROW_AO_QUALITY), "AO QUALITY"));
    CHECK(strstr(pc_video_menu_row_help(PCV_ROW_AO_QUALITY), "sampling density and radius"));
    for (method = 1; method <= PCV_AO_COUNT; method++) {
        CHECK(pc_video_menu_row_change(PCV_ROW_AO_METHOD, 1)); pc_video_get(&s);
        CHECK(s.ao_method == method % PCV_AO_COUNT);
        CHECK(pc_video_menu_row_value(PCV_ROW_AO_METHOD, value, sizeof(value)) > 0);
    }
    for (quality = 1; quality <= PCV_AOQ_COUNT; quality++) {
        CHECK(pc_video_menu_row_change(PCV_ROW_AO_QUALITY, 1)); pc_video_get(&s);
        CHECK(s.ao_quality == (PCV_AOQ_HIGH + quality) % PCV_AOQ_COUNT);
    }
    CHECK(hook_calls == 0);
    s.ao_method = PCV_AO_GTAO; CHECK(pc_video_set(&s));
    CHECK(strstr(pc_video_menu_row_help(PCV_ROW_AO_QUALITY), "scene resolution"));
    CHECK(!strstr(pc_video_menu_row_help(PCV_ROW_AO_QUALITY), "stronger"));
    f = fopen(ini_path, "rb"); CHECK(f != NULL);
    text[fread(text, 1, sizeof(text) - 1, f)] = 0; fclose(f);
    CHECK(strstr(text, "ao_method=gtao\n") && strstr(text, "ao_quality=high\n"));
    fresh(); hook_calls = 0;
}

static void test_environment_and_file(void)
{
    PcVideoSettings s, w;
    unsigned rw, rh;
    FILE *f;
    char text[1024];

    /* A settings file from a previous run is read at start. */
    DeleteFileA(ini_path);
    clear_env();
    f = fopen(ini_path, "wb");
    CHECK(f != NULL);
    fputs("[video]\ninternal_height=1080\nwindow_width=1920\nwindow_height=1080\nwindow_mode=borderless\naspect=16:9\nfov=85\n", f);
    fclose(f);
    pc_video_test_reset(ini_path);
    pc_video_get(&s);
    CHECK(s.internal_height == 1080 && s.window_width == 1920 && s.borderless && s.aspect == PCV_ASPECT_16_9 && s.fov == 85.0);
    CHECK(pc_video_aspect() == PCV_ASPECT_16_9 && pc_video_fov() == 85.0 && !pc_video_motion_blur_off());
    pc_video_render_size(&rw, &rh);
    CHECK(rw == 1920 && rh == 1080);

    /* A change is validated, published, announced, saved, and tells a renderer that can resize. */
    pc_video_set_resize_hook(hook);
    CHECK(pc_video_live_resize());
    w = s; w.vsync = 0;
    CHECK(pc_video_set(&w) && pc_video_generation() == 1 && hook_calls == 0);   /* not a raster change */
    w.internal_height = 720;
    CHECK(pc_video_set(&w) && hook_calls == 1 && hook_w == 1280 && hook_h == 720 && hook_ow == 1280 && hook_oh == 720);
    w.aspect = PCV_ASPECT_21_9;
    CHECK(pc_video_set(&w) && hook_calls == 2 && hook_w == 1680 && hook_h == 720);
    w.aa = PCV_AA_SSAA;                                                   /* super-sampling: a bigger raster, the same output */
    CHECK(pc_video_set(&w) && hook_calls == 3 && hook_w == 3360 && hook_h == 1440 && hook_ow == 1680 && hook_oh == 720);
    w.aa = PCV_AA_FXAA; w.af = 8; w.dof = PCV_STEP_LOW; w.sharpen = PCV_STEP_MEDIUM;
    CHECK(pc_video_set(&w) && hook_calls == 4 && hook_w == 1680 && hook_h == 720);   /* back to the plain raster; AF/DoF/sharpen are not sizes */
    w.af = 16; w.dof = PCV_STEP_OFF; w.sharpen = PCV_STEP_OFF;
    CHECK(pc_video_set(&w) && hook_calls == 4);
    w.internal_height = 123;
    CHECK(!pc_video_set(&w) && hook_calls == 4 && pc_video_generation() == 6);  /* refused: nothing changed */
    w.internal_height = 720; w.af = 3;
    CHECK(!pc_video_set(&w));
    w.af = 16; w.dof = 9;
    CHECK(!pc_video_set(&w));
    w.dof = PCV_STEP_OFF; w.aa = 7;
    CHECK(!pc_video_set(&w));
    w.aa = PCV_AA_FXAA;
    w.internal_height = 720; w.fov = 20.0;
    CHECK(!pc_video_set(&w));
    w.fov = 85.0;
    pc_video_get(&s);
    CHECK(s.internal_height == 720 && s.aspect == PCV_ASPECT_21_9 && !s.vsync && s.fov == 85.0);
    f = fopen(ini_path, "rb");
    CHECK(f != NULL);
    text[fread(text, 1, sizeof(text) - 1, f)] = 0;
    fclose(f);
    CHECK(strstr(text, "internal_height=720") && strstr(text, "aspect=21:9") && strstr(text, "vsync=0"));
    pc_video_test_reset(ini_path);                                        /* and it survives a restart */
    pc_video_get(&w);
    CHECK(w.internal_height == 720 && w.aspect == PCV_ASPECT_21_9 && !w.vsync && w.borderless);

    /* F11 is remembered without being announced as a change. */
    pc_video_note_borderless(0);
    CHECK(pc_video_generation() == 0);
    pc_video_test_reset(ini_path);
    pc_video_get(&w);
    CHECK(!w.borderless);

    /* The environment pins a field for this run: the file keeps the player's choice, and so does a change made meanwhile. */
    _putenv_s("RECOMP_RENDER_WIDTH", "3440");
    _putenv_s("RECOMP_RENDER_HEIGHT", "1440");
    _putenv_s("RECOMP_VSYNC", "0");
    _putenv_s("RECOMP_BLACK_FOV", "100");
    _putenv_s("RECOMP_WINDOW_MODE", "borderless");
    pc_video_test_reset(ini_path);
    pc_video_get(&s);
    CHECK((pc_video_locked() & (PCV_LOCK_INTERNAL | PCV_LOCK_VSYNC | PCV_LOCK_FOV | PCV_LOCK_WINDOW_MODE)) ==
          (PCV_LOCK_INTERNAL | PCV_LOCK_VSYNC | PCV_LOCK_FOV | PCV_LOCK_WINDOW_MODE));
    CHECK(!(pc_video_locked() & (PCV_LOCK_ASPECT | PCV_LOCK_SCALE | PCV_LOCK_FPS)));
    CHECK(s.render_override_w == 3440 && s.render_override_h == 1440 && !s.vsync && s.fov == 100.0 && s.borderless);
    pc_video_render_size(&rw, &rh);
    CHECK(rw == 3440 && rh == 1440);
    w = s; w.vsync = 1; w.fov = 60; w.internal_height = 480; w.aspect = PCV_ASPECT_16_9; w.borderless = 0;
    CHECK(pc_video_set(&w));
    pc_video_get(&s);
    CHECK(!s.vsync && s.fov == 100.0 && s.borderless && s.aspect == PCV_ASPECT_16_9);   /* pinned fields kept, the rest took */
    pc_video_render_size(&rw, &rh);
    CHECK(rw == 3440 && rh == 1440);
    f = fopen(ini_path, "rb");
    CHECK(f != NULL);
    text[fread(text, 1, sizeof(text) - 1, f)] = 0;
    fclose(f);
    CHECK(strstr(text, "aspect=16:9") && strstr(text, "fov=85") && strstr(text, "fov=100") == NULL &&
          strstr(text, "vsync=0") && strstr(text, "vsync=1") == NULL);   /* the file kept the earlier choice for the pinned fields */
    clear_env();

    /* A malformed pair of raster variables is ignored, as the renderers always did. */
    _putenv_s("RECOMP_RENDER_WIDTH", "99999");
    _putenv_s("RECOMP_RENDER_HEIGHT", "1440");
    pc_video_test_reset(ini_path);
    pc_video_get(&s);
    CHECK(!s.render_override_w && !(pc_video_locked() & PCV_LOCK_INTERNAL));
    clear_env();
    _putenv_s("RECOMP_INTERNAL_HEIGHT", "1080");
    pc_video_test_reset(ini_path);
    pc_video_get(&s);
    CHECK(s.internal_height == 1080 && (pc_video_locked() & PCV_LOCK_INTERNAL));
    clear_env();

    /* Reset goes back to the defaults. */
    pc_video_test_reset(ini_path);
    pc_video_get(&s);
    s.internal_height = 2160; s.vsync = 0;
    CHECK(pc_video_set(&s));
    pc_video_reset();
    pc_video_get(&s);
    CHECK(s.internal_height == 720 && s.vsync && s.window_width == 1280);
}

static uint8_t report[20];
static int pad(unsigned digital, unsigned a, unsigned b, unsigned white)
{
    memset(report, 0, sizeof(report));
    report[1] = 20; report[2] = (uint8_t)digital; report[4] = (uint8_t)a; report[5] = (uint8_t)b; report[9] = (uint8_t)white;
    pc_input_apply_report(report, 1);
    return report[2] || report[4] || report[5] || report[9];     /* what the game would see */
}

static int enter_reaches_game(void)
{
    uint8_t out[20] = { 0, 20 };
    pc_input_apply_report(out, 0);
    return out[4] == 0xFF;
}

static unsigned alpha_at(const uint32_t *px, unsigned w, unsigned x, unsigned y) { return px[(size_t)y * w + x] >> 24; }

static void test_menu(void)
{
    PcVideoSettings s;
    const uint32_t *px;
    int changed, i;
    unsigned x, y, painted, minx, maxx, miny, maxy;

    fresh();
    pc_input_test_reset();
    pc_input_test_set_time(1000);
    pc_video_menu_install();

    /* Nothing to offer until the game shows an Options page: V is the game's. */
    CHECK(!pc_video_menu_overlay(1280, 960, &px, &changed));
    pc_input_key_event(0x2F, 0, 1, 0);
    pc_input_key_event(0x2F, 0, 0, 0);
    CHECK(!pc_video_menu_is_open());

    /* On an Options page the hint is drawn, and V opens the page. */
    pc_video_menu_page_seen();
    CHECK(pc_video_menu_overlay(1280, 960, &px, &changed) && changed);
    painted = 0; minx = 1280; maxx = 0; miny = 960; maxy = 0;
    for (y = 0; y < 960; y++) for (x = 0; x < 1280; x++) if (alpha_at(px, 1280, x, y)) {
        painted++; if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y;
    }
    CHECK(painted > 500 && miny > 800 && maxy < 960 && minx > 400 && maxx < 880);   /* a small banner, bottom centre */
    CHECK(pc_video_menu_overlay(1280, 960, &px, &changed) && !changed);              /* unchanged: no new pixels */
    pc_input_key_event(0x2F, 0, 1, 0);
    CHECK(pc_video_menu_is_open());
    pc_input_key_event(0x2F, 0, 0, 0);

    /* Open: the page is drawn centred and opaque over a dimmed screen. */
    CHECK(pc_video_menu_overlay(1280, 960, &px, &changed) && changed);
    CHECK(alpha_at(px, 1280, 5, 5) == 120 && alpha_at(px, 1280, 640, 480) > 200);
    CHECK(pc_video_menu_overlay(1280, 960, &px, &changed) && !changed);

    /* Keys are the page's while it is open: Enter changes a row and never reaches the game. */
    pc_video_get(&s);
    CHECK(!s.borderless && pc_video_menu_test_selected() == 0);
    pc_input_test_set_time(1100);
    pc_input_key_event(0x1C, 0, 1, 0);
    CHECK(!enter_reaches_game());
    pc_input_key_event(0x1C, 0, 0, 0);
    pc_video_get(&s);
    CHECK(s.borderless);
    pc_input_key_event(0x50, 1, 1, 0); pc_input_key_event(0x50, 1, 0, 0);      /* down */
    pc_input_key_event(0x50, 1, 1, 0); pc_input_key_event(0x50, 1, 0, 0);
    CHECK(pc_video_menu_test_selected() == 2);
    pc_input_key_event(0x4D, 1, 1, 0); pc_input_key_event(0x4D, 1, 0, 0);      /* right: 720p -> 1080p */
    pc_input_key_event(0x4D, 1, 1, 1);                                          /* held: auto-repeat steps again */
    pc_input_key_event(0x4D, 1, 0, 0);
    pc_video_get(&s);
    CHECK(s.internal_height == 1440);
    pc_input_key_event(0x4B, 1, 1, 0); pc_input_key_event(0x4B, 1, 0, 0);      /* left */
    pc_video_get(&s);
    CHECK(s.internal_height == 1080);
    CHECK(pc_video_menu_overlay(1280, 960, &px, &changed) && changed);           /* redrawn after the changes */

    /* A wheel notch and the mouse buttons belong to it too. */
    pc_input_wheel_event(-120);
    CHECK(pc_video_menu_test_selected() == 3);
    pc_input_wheel_event(120);
    CHECK(pc_video_menu_test_selected() == 2);

    /* Reset and Done are actions; Escape closes, and its release does not reach the game either. */
    for (i = pc_video_menu_test_selected(); i < PCV_ROW_RESET; i++) { pc_input_key_event(0x50, 1, 1, 0); pc_input_key_event(0x50, 1, 0, 0); }
    CHECK(pc_video_menu_test_selected() == PCV_ROW_RESET);
    pc_input_key_event(0x1C, 0, 1, 0); pc_input_key_event(0x1C, 0, 0, 0);
    pc_video_get(&s);
    CHECK(s.internal_height == 720 && !s.borderless);
    pc_input_key_event(0x50, 1, 1, 0); pc_input_key_event(0x50, 1, 0, 0);
    CHECK(pc_video_menu_test_selected() == PCV_ROW_DONE);
    pc_input_key_event(0x1C, 0, 1, 0); pc_input_key_event(0x1C, 0, 0, 0);        /* Done */
    CHECK(!pc_video_menu_is_open());
    pc_input_test_set_time(5000);
    pc_input_key_event(0x1C, 0, 1, 0);
    CHECK(enter_reaches_game());                                                 /* closed: Enter is the game's again */
    pc_input_key_event(0x1C, 0, 0, 0);

    /* Escape closes without anything leaking as the game's Back. */
    pc_input_test_set_time(9000);
    pc_video_menu_page_seen();
    pc_input_key_event(0x2F, 0, 1, 0); pc_input_key_event(0x2F, 0, 0, 0);
    CHECK(pc_video_menu_is_open());
    pc_input_key_event(0x01, 0, 1, 0);
    CHECK(!pc_video_menu_is_open());
    pc_input_key_event(0x01, 0, 0, 0);

    /* The pad: the right shoulder button opens, the d-pad and A change, B closes; each press is the page's alone. */
    pc_video_menu_page_seen();
    pc_input_test_set_time(12000);
    CHECK(!pad(0, 0, 0, 255) && pc_video_menu_is_open());
    CHECK(!pad(0, 0, 0, 255));                                                   /* still held */
    CHECK(!pad(0, 0, 0, 0));
    CHECK(!pad(2, 0, 0, 0) && pc_video_menu_test_selected() == 1);               /* down */
    CHECK(!pad(0, 0, 0, 0));
    CHECK(!pad(2, 0, 0, 0) && pc_video_menu_test_selected() == 2);
    CHECK(!pad(0, 0, 0, 0));
    CHECK(!pad(0, 255, 0, 0));                                                   /* A: 720p -> 1080p */
    pc_video_get(&s);
    CHECK(s.internal_height == 1080);
    CHECK(!pad(0, 0, 0, 0));
    CHECK(!pad(0, 0, 255, 0) && !pc_video_menu_is_open());                       /* B closes ... */
    CHECK(!pad(0, 0, 255, 0));                                                   /* ... and is not the game's while held */
    CHECK(pad(0, 0, 0, 0) == 0);
    CHECK(pad(0, 255, 0, 0));                                                    /* released: the game gets its pad back */

    /* The page goes away with the Options page that carried it. */
    pc_video_menu_page_seen();
    pc_input_test_set_time(20000);
    pc_input_key_event(0x2F, 0, 1, 0); pc_input_key_event(0x2F, 0, 0, 0);
    CHECK(pc_video_menu_is_open());
    Sleep(1700);
    CHECK(!pc_video_menu_overlay(1280, 960, &px, &changed) && !pc_video_menu_is_open());
}

static void test_environment_locks_in_menu(void)
{
    PcVideoSettings s;
    int i;
    fresh();
    _putenv_s("RECOMP_VSYNC", "0");
    _putenv_s("RECOMP_BLACK_FOV", "90");
    pc_video_test_reset(ini_path);
    pc_input_test_reset();
    pc_video_menu_install();
    pc_video_menu_test_force_available(1);
    pc_video_menu_test_action(6);
    CHECK(pc_video_menu_is_open());
    for (i = 0; i < PCV_ROW_VSYNC; i++) pc_video_menu_test_action(1);            /* to VSYNC */
    CHECK(pc_video_menu_test_selected() == PCV_ROW_VSYNC);
    pc_video_menu_test_action(4);
    pc_video_get(&s);
    CHECK(!s.vsync);                                                             /* pinned: unchanged */
    for (i = PCV_ROW_VSYNC; i > PCV_ROW_FOV; i--) pc_video_menu_test_action(0);                 /* to FOV */
    pc_video_menu_test_action(3);
    pc_video_get(&s);
    CHECK(s.fov == 90.0);
    for (i = PCV_ROW_FOV; i < PCV_ROW_FPS; i++) pc_video_menu_test_action(1);                   /* to the FPS limit row */
    CHECK(pc_video_menu_test_selected() == PCV_ROW_FPS);
    pc_video_menu_test_action(3);
    pc_video_get(&s);
    CHECK(s.fps_limit == 30);                                                    /* not pinned: changes */
    pc_video_menu_test_action(5);
    CHECK(!pc_video_menu_is_open());
    pc_video_menu_test_force_available(0);
    clear_env();
}

/* The picture-effect rows: anti-aliasing, anisotropic filtering, depth of field and sharpening. */
static void test_effect_rows(void)
{
    PcVideoSettings s;
    unsigned i;

    /* Environment pins: a bad value pins the effect off, a good one pins that value, the others stay free. */
    fresh();
    _putenv_s("RECOMP_AF", "4");
    _putenv_s("RECOMP_DOF", "medium");
    _putenv_s("RECOMP_AA", "bogus");
    pc_video_test_reset(ini_path);
    pc_video_get(&s);
    CHECK(s.af == 4 && s.dof == PCV_STEP_MEDIUM && s.aa == PCV_AA_OFF && s.sharpen == PCV_STEP_OFF);
    CHECK((pc_video_locked() & (PCV_LOCK_AF | PCV_LOCK_DOF | PCV_LOCK_AA)) == (PCV_LOCK_AF | PCV_LOCK_DOF | PCV_LOCK_AA) &&
          !(pc_video_locked() & PCV_LOCK_SHARPEN));
    clear_env();
    _putenv_s("RECOMP_AF", "3");
    _putenv_s("RECOMP_AA", "SSAA");
    _putenv_s("RECOMP_SHARPEN", "2");
    pc_video_test_reset(ini_path);
    pc_video_get(&s);
    CHECK(s.af == 1 && s.aa == PCV_AA_SSAA && s.sharpen == PCV_STEP_OFF);   /* 3 is no level; "2" is no step name */

    /* The rows: pinned ones do not move, the rest cycle and wrap. */
    fresh();
    _putenv_s("RECOMP_AF", "4");
    pc_video_test_reset(ini_path);
    pc_input_test_reset();
    pc_video_menu_install();
    pc_video_menu_test_force_available(1);
    pc_video_menu_test_action(6);
    for (i = 0; i < PCV_ROW_AA; i++) pc_video_menu_test_action(1);
    CHECK(pc_video_menu_test_selected() == PCV_ROW_AA);
    pc_video_menu_test_action(3); pc_video_get(&s); CHECK(s.aa == PCV_AA_SSAA);
    pc_video_menu_test_action(3); pc_video_get(&s); CHECK(s.aa == PCV_AA_OFF);
    pc_video_menu_test_action(3); pc_video_get(&s); CHECK(s.aa == PCV_AA_FXAA);
    pc_video_menu_test_action(2); pc_video_get(&s); CHECK(s.aa == PCV_AA_OFF);
    pc_video_menu_test_action(1);
    CHECK(pc_video_menu_test_selected() == PCV_ROW_AF);
    pc_video_menu_test_action(3); pc_video_menu_test_action(2); pc_video_get(&s);
    CHECK(s.af == 4);                                                       /* pinned */
    pc_video_menu_test_action(1);
    pc_video_menu_test_action(3); pc_video_get(&s); CHECK(s.dof == PCV_STEP_LOW);
    pc_video_menu_test_action(3); pc_video_menu_test_action(3); pc_video_get(&s); CHECK(s.dof == PCV_STEP_HIGH);
    pc_video_menu_test_action(3); pc_video_get(&s); CHECK(s.dof == PCV_STEP_OFF);
    pc_video_menu_test_action(1);
    CHECK(pc_video_menu_test_selected() == PCV_ROW_SHARPEN);
    pc_video_menu_test_action(2); pc_video_get(&s); CHECK(s.sharpen == PCV_STEP_HIGH);
    pc_video_menu_test_action(5);
    clear_env();

    /* Anisotropic filtering walks 1 (off), 2, 4, 8, 16 and wraps. */
    fresh();
    pc_input_test_reset();
    pc_video_menu_install();
    pc_video_menu_test_action(6);
    for (i = 0; i < PCV_ROW_AF; i++) pc_video_menu_test_action(1);
    pc_video_get(&s);
    CHECK(s.af == 16);
    pc_video_menu_test_action(3); pc_video_get(&s); CHECK(s.af == 1);
    pc_video_menu_test_action(3); pc_video_get(&s); CHECK(s.af == 2);
    pc_video_menu_test_action(3); pc_video_get(&s); CHECK(s.af == 4);
    pc_video_menu_test_action(3); pc_video_get(&s); CHECK(s.af == 8);
    pc_video_menu_test_action(3); pc_video_get(&s); CHECK(s.af == 16);
    pc_video_menu_test_action(5);
    pc_video_menu_test_force_available(0);
    /* They are saved with the rest and come back. */
    pc_video_test_reset(ini_path);
    pc_video_get(&s);
    CHECK(s.af == 16 && s.aa == PCV_AA_FXAA && s.dof == PCV_STEP_OFF && s.sharpen == PCV_STEP_OFF);
}

static void test_sizes_and_cycling(void)
{
    PcVideoSettings s;
    const uint32_t *px;
    int changed, i;
    unsigned seen_heights = 0, h;

    fresh();
    pc_input_test_reset();
    pc_video_menu_install();
    pc_video_menu_test_force_available(1);
    pc_video_menu_test_action(6);
    /* The internal resolution walks 480p .. 2160p and wraps. */
    pc_video_menu_test_action(1); pc_video_menu_test_action(1);
    CHECK(pc_video_menu_test_selected() == 2);
    for (i = 0; i < 5; i++) {
        pc_video_get(&s);
        for (h = 0; h < pc_video_internal_height_count; h++) if (pc_video_internal_heights[h] == s.internal_height) seen_heights |= 1u << h;
        pc_video_menu_test_action(3);
    }
    CHECK(seen_heights == 0x1F);
    pc_video_get(&s);
    CHECK(s.internal_height == 720);
    /* Every row, left and right, leaves the settings valid (pc_video_set refuses anything else). */
    for (i = 0; i < PCV_ROW_COUNT; i++) {
        unsigned k;
        for (k = 0; k < 9; k++) pc_video_menu_test_action(3);
        for (k = 0; k < 9; k++) pc_video_menu_test_action(2);
        pc_video_menu_test_action(1);
    }
    /* The page draws at every window size, tiny to enormous, inside the buffer. */
    pc_video_menu_test_action(6);
    for (i = 0; i < 5; i++) {
        static const unsigned dims[5][2] = { { 320, 240 }, { 640, 480 }, { 1920, 1080 }, { 3440, 1440 }, { 3840, 2160 } };
        unsigned x, y, painted = 0;
        CHECK(pc_video_menu_overlay(dims[i][0], dims[i][1], &px, &changed) && changed);
        for (y = 0; y < dims[i][1]; y += 7) for (x = 0; x < dims[i][0]; x += 7) if (alpha_at(px, dims[i][0], x, y) > 200) painted++;
        CHECK(painted > 50);
    }
    pc_video_menu_test_force_available(0);
}

static unsigned native_open_requests;
static void request_native_video(void)
{
    /* A native request queues work; its ability to query here also proves no popup lock is held. */
    CHECK(!pc_video_menu_is_open());
    native_open_requests++;
}

static void test_native_row_services(void)
{
    PcVideoSettings s, before;
    char value[128], tiny[5];
    unsigned generation;
    int row;
    fresh();
    pc_video_menu_set_native_opener(NULL);
    pc_video_menu_test_action(5);
    for (row = 0; row < PCV_ROW_COUNT; row++) {
        size_t value_len;
        CHECK(*pc_video_menu_row_label(row) && *pc_video_menu_row_help(row));
        CHECK(!pc_video_menu_row_locked(row));
        value_len = pc_video_menu_row_value(row, value, sizeof(value));
        CHECK(value_len == strlen(value));
        if (row < PCV_ROW_RESET) {
            CHECK(*value && !strstr(value, "LOCKED") && !strstr(value, "GPU ONLY") && !strstr(value, "RESTART"));
            CHECK(pc_video_menu_row_change(row, 1));
            CHECK(pc_video_menu_row_change(row, -1));
        } else CHECK(!*value && !pc_video_menu_row_change(row, 1));
    }
    CHECK(!strcmp(pc_video_menu_row_label(PCV_ROW_MODE), "WINDOW MODE"));
    CHECK(!*pc_video_menu_row_label(-1) && !*pc_video_menu_row_label(PCV_ROW_COUNT));
    CHECK(!*pc_video_menu_row_help(-1));
    generation = pc_video_generation();
    CHECK(!pc_video_menu_row_change(-1, 1) && !pc_video_menu_row_change(PCV_ROW_COUNT, 1));
    CHECK(!pc_video_menu_row_change(PCV_ROW_MODE, 0) && pc_video_generation() == generation);
    CHECK(!pc_video_menu_row_value(PCV_ROW_FOV, NULL, 0));
    CHECK(!pc_video_menu_row_value(PCV_ROW_FOV, tiny, 0));
    pc_video_get(&s); s.fov = 35; CHECK(pc_video_set(&s));
    /* A degree symbol is two UTF-8 bytes. Truncation cannot expose its first byte alone. */
    CHECK(pc_video_menu_row_value(PCV_ROW_FOV, tiny, 4) == 2 && !strcmp(tiny, "35"));
    CHECK(pc_video_menu_row_value(PCV_ROW_FOV, tiny, 5) == 4 && !strcmp(tiny, "35\xC2\xB0"));
    CHECK(pc_video_menu_row_value(PCV_ROW_FOV, tiny, 1) == 0 && !*tiny);
    CHECK(strstr(pc_video_menu_row_help(PCV_ROW_INTERNAL), "restart"));
    CHECK(strstr(pc_video_menu_row_help(PCV_ROW_AA), "GPU renderer"));
    pc_video_set_resize_hook(hook);
    CHECK(strstr(pc_video_menu_row_help(PCV_ROW_INTERNAL), "immediately"));
    s.aa = PCV_AA_SSAA; s.internal_height = 2160; s.aspect = PCV_ASPECT_16_9; CHECK(pc_video_set(&s));
    CHECK(strstr(pc_video_menu_row_help(PCV_ROW_AA), "unavailable at this resolution"));
    s.internal_height = 720; CHECK(pc_video_set(&s));
    CHECK(!strstr(pc_video_menu_row_help(PCV_ROW_AA), "unavailable"));
    pc_video_menu_reset_settings(); pc_video_get(&s); pc_video_defaults(&before);
    CHECK(same_settings(&s, &before));

    /* Every exposed row obeys its environment pin, and Reset cannot overwrite a pinned field. */
    _putenv_s("RECOMP_WINDOW_WIDTH", "1600"); _putenv_s("RECOMP_WINDOW_HEIGHT", "900");
    _putenv_s("RECOMP_WINDOW_MODE", "borderless"); _putenv_s("RECOMP_INTERNAL_HEIGHT", "1080");
    _putenv_s("RECOMP_AA", "ssaa"); _putenv_s("RECOMP_AF", "8");
    _putenv_s("RECOMP_DOF", "high"); _putenv_s("RECOMP_SHARPEN", "medium");
    _putenv_s("RECOMP_AO_METHOD", "gtao"); _putenv_s("RECOMP_AO_QUALITY", "ultra");
    _putenv_s("RECOMP_BLACK_ASPECT", "16:9"); _putenv_s("RECOMP_BLACK_FOV", "90");
    _putenv_s("RECOMP_SCALE_MODE", "stretch"); _putenv_s("RECOMP_PRESENT_FILTER", "nearest");
    _putenv_s("RECOMP_VSYNC", "0"); _putenv_s("RECOMP_FPS_LIMIT", "60");
    _putenv_s("RECOMP_BLACK_MOTION_BLUR", "off");
    pc_video_test_reset(ini_path); pc_video_get(&before);
    generation = pc_video_generation();
    for (row = 0; row < PCV_ROW_RESET; row++) {
        CHECK(pc_video_menu_row_locked(row));
        CHECK(strstr(pc_video_menu_row_help(row), "fixed by an environment variable"));
        CHECK(!pc_video_menu_row_change(row, 1));
    }
    CHECK(pc_video_generation() == generation);
    pc_video_menu_reset_settings(); pc_video_get(&s);
    CHECK(same_settings(&s, &before));

    fresh();
    _putenv_s("RECOMP_RENDER_WIDTH", "1110"); _putenv_s("RECOMP_RENDER_HEIGHT", "740");
    pc_video_test_reset(ini_path);
    CHECK(pc_video_menu_row_locked(PCV_ROW_INTERNAL));
    CHECK(pc_video_menu_row_value(PCV_ROW_INTERNAL, value, sizeof(value)) && !strcmp(value, "1110 x 740"));
    pc_video_menu_reset_settings(); pc_video_get(&s);
    CHECK(s.render_override_w == 1110 && s.render_override_h == 740);
    fresh();
}

static void test_native_opener(void)
{
    const uint32_t *px;
    int changed;
    fresh();
    pc_video_menu_install();
    pc_video_menu_test_force_available(1);
    pc_video_menu_open(); CHECK(pc_video_menu_is_open());
    pc_video_menu_set_native_opener(request_native_video);
    CHECK(!pc_video_menu_is_open());
    pc_video_menu_hide_hint(1);
    CHECK(!pc_video_menu_overlay(1280, 960, &px, &changed));
    native_open_requests = 0;
    pc_video_menu_open(); CHECK(native_open_requests == 1 && !pc_video_menu_is_open());
    pc_video_menu_test_action(6); CHECK(native_open_requests == 2 && !pc_video_menu_is_open());
    pc_input_key_event(0x2F, 0, 1, 0); pc_input_key_event(0x2F, 0, 0, 0);
    CHECK(native_open_requests == 3 && !pc_video_menu_is_open());
    pad(0, 0, 0, 0);
    CHECK(!pad(0, 0, 0, 255) && native_open_requests == 4 && !pc_video_menu_is_open());
    CHECK(!pad(0, 0, 0, 255) && native_open_requests == 4);
    pad(0, 0, 0, 0);
    pc_video_menu_set_native_opener(NULL);
    pc_video_menu_open(); CHECK(pc_video_menu_is_open());
    pc_video_menu_test_action(5);
    pc_video_menu_test_force_available(0);
    pc_video_menu_hide_hint(0);
}

int main(void)
{
    DWORD n = GetTempPathA(MAX_PATH, ini_path);
    CHECK(n > 0 && n < MAX_PATH);
    snprintf(ini_path + n, 32, "pc_video_test_%lu.ini", (unsigned long)GetCurrentProcessId());
    test_model();
    test_sustained_fire_survives_gameplay_gap();
    test_look_survives_camera_gap();
    test_fire_ack_generation_guards();
    test_ambient_occlusion();
    test_environment_and_file();
    test_menu();
    test_environment_locks_in_menu();
    test_effect_rows();
    test_sizes_and_cycling();
    test_native_row_services();
    test_native_opener();
    DeleteFileA(ini_path);
    printf("pc_video: %u checks passed\n", checks);
    return 0;
}
