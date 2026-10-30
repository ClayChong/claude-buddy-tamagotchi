#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "buddy_anim.h"

/* Every clip has 4 frames at 300 ms (1200 ms per clip), except busy and heart.
 * idle: clips 0-3 (frames 0-15); busy: clip 4 (16-19); hungry: clip 5 (20-23);
 * heart: clip 6 (24-27). */
#define CLIP_MS 1200U
#define BOUNDARIES 2000U

static void make_info(buddy_pack_info_t *info, uint16_t idle_clips, bool hungry)
{
    static const buddy_pack_clip_t clips[7] = {
        {0, 4, 300}, {4, 4, 300}, {8, 4, 300}, {12, 4, 300},
        {16, 4, 200}, {20, 4, 300}, {24, 4, 200}};

    memset(info, 0, sizeof(*info));
    info->version = 2;
    info->clip_count = 7;
    info->frame_count = 28;
    info->payload_crc = 1;
    memcpy(info->clips, clips, sizeof(clips));
    info->states[BUDDY_PACK_IDLE] = (buddy_pack_state_entry_t){0, idle_clips};
    info->states[BUDDY_PACK_BUSY] = (buddy_pack_state_entry_t){4, 1};
    info->states[BUDDY_PACK_HEART] = (buddy_pack_state_entry_t){6, 1};
    if (hungry) {
        info->states[BUDDY_PACK_HUNGRY] = (buddy_pack_state_entry_t){5, 1};
    }
}

/* Which clip a clip-start frame belongs to: 0-3 idle, 5 hungry, -1 otherwise. */
static int clip_of(uint16_t frame)
{
    if (frame < 16U && frame % 4U == 0U) return (int)(frame / 4U);
    if (frame == 20U) return 5;
    return -1;
}

/* Records the clip started at each boundary of an idle walk. */
static void walk_idle(buddy_mood_t mood, uint16_t idle_clips, bool hungry, int *starts)
{
    buddy_pack_info_t info;
    buddy_anim_t anim;
    unsigned k;

    make_info(&info, idle_clips, hungry);
    buddy_anim_init(&anim, 12345);
    for (k = 0; k < BOUNDARIES; ++k) {
        uint16_t frame = 0xffff;

        assert(buddy_anim_frame(&anim, &info, BUDDY_PACK_IDLE, mood, (uint64_t)k * CLIP_MS + 1U,
                                &frame));
        starts[k] = clip_of(frame);
        assert(starts[k] >= 0);
    }
}

static void test_state_change_starts_at_frame_zero(void)
{
    buddy_pack_info_t info;
    buddy_anim_t anim;
    uint16_t frame;

    make_info(&info, 4, false);
    buddy_anim_init(&anim, 7);
    assert(buddy_anim_frame(&anim, &info, BUDDY_PACK_IDLE, BUDDY_MOOD_NORMAL, 0, &frame));
    assert(buddy_anim_frame(&anim, &info, BUDDY_PACK_BUSY, BUDDY_MOOD_NORMAL, 12345, &frame));
    assert(frame == 16);
    assert(buddy_anim_frame(&anim, &info, BUDDY_PACK_BUSY, BUDDY_MOOD_NORMAL, 12345 + 250, &frame));
    assert(frame == 17);
    assert(buddy_anim_frame(&anim, &info, BUDDY_PACK_HEART, BUDDY_MOOD_NORMAL, 20000, &frame));
    assert(frame == 24);
    assert(buddy_anim_frame(&anim, &info, BUDDY_PACK_IDLE, BUDDY_MOOD_NORMAL, 30001, &frame));
    assert(frame == 0);
}

static void test_base_runs_between_specials(void)
{
    static int starts[BOUNDARIES];
    int last_special = -1;
    unsigned run = 0;
    unsigned specials = 0;
    unsigned k;

    walk_idle(BUDDY_MOOD_NORMAL, 4, false, starts);
    for (k = 0; k < BOUNDARIES; ++k) {
        if (starts[k] == 0) {
            ++run;
            assert(run <= 3U);
            continue;
        }
        assert(run >= 1U && run <= 3U);
        assert(starts[k] != last_special);
        last_special = starts[k];
        run = 0;
        ++specials;
    }
    assert(specials > BOUNDARIES / 4U);
}

static void test_single_idle_clip_always_base(void)
{
    static int starts[BOUNDARIES];
    unsigned k;

    walk_idle(BUDDY_MOOD_NORMAL, 1, false, starts);
    for (k = 0; k < BOUNDARIES; ++k) {
        assert(starts[k] == 0);
    }
}

static void test_mood_clip_share(void)
{
    static int starts[BOUNDARIES];
    unsigned specials = 0;
    unsigned hungry = 0;
    unsigned k;

    walk_idle(BUDDY_MOOD_HUNGRY, 4, true, starts);
    for (k = 0; k < BOUNDARIES; ++k) {
        if (starts[k] != 0) ++specials;
        if (starts[k] == 5) ++hungry;
    }
    assert(hungry * 100U >= specials * 35U);
    assert(hungry * 100U <= specials * 65U);
}

static void test_missing_mood_clip_ignored(void)
{
    static int starts[BOUNDARIES];
    unsigned k;

    walk_idle(BUDDY_MOOD_SAD, 4, true, starts);
    for (k = 0; k < BOUNDARIES; ++k) {
        assert(starts[k] >= 0 && starts[k] <= 3);
    }
}

static void test_pack_change_restarts(void)
{
    buddy_pack_info_t info;
    buddy_anim_t anim;
    uint16_t frame;
    uint64_t now;

    make_info(&info, 4, false);
    buddy_anim_init(&anim, 99);
    for (now = 1; now < 20U * CLIP_MS; now += CLIP_MS) {
        assert(buddy_anim_frame(&anim, &info, BUDDY_PACK_IDLE, BUDDY_MOOD_NORMAL, now, &frame));
    }
    info.payload_crc = 2;
    info.frame_count = 4;
    info.states[BUDDY_PACK_IDLE].clip_count = 1;
    assert(buddy_anim_frame(&anim, &info, BUDDY_PACK_IDLE, BUDDY_MOOD_NORMAL, now + 700, &frame));
    assert(frame == 0);
    assert(buddy_anim_frame(&anim, &info, BUDDY_PACK_IDLE, BUDDY_MOOD_NORMAL, now + 1000, &frame));
    assert(frame == 1 && frame < info.frame_count);
}

static void test_clip_table_change_with_same_crc(void)
{
    buddy_pack_info_t info;
    buddy_anim_t anim;
    uint16_t frame = 0;
    uint64_t now = 1;

    make_info(&info, 4, false);
    buddy_anim_init(&anim, 3);
    while (anim.clip != 3U) {
        assert(buddy_anim_frame(&anim, &info, BUDDY_PACK_IDLE, BUDDY_MOOD_NORMAL, now, &frame));
        now += CLIP_MS;
        assert(now < 1000U * CLIP_MS);
    }
    /* Clip 3 started at now - CLIP_MS; ask again halfway through it. */
    info.states[BUDDY_PACK_IDLE].clip_count = 1;
    assert(buddy_anim_frame(&anim, &info, BUDDY_PACK_IDLE, BUDDY_MOOD_NORMAL,
                            now - CLIP_MS + 600U, &frame));
    assert(frame < 4U);
}

static void test_long_gap_returns_quickly(void)
{
    buddy_pack_info_t info;
    buddy_anim_t anim;
    uint16_t frame;

    make_info(&info, 4, false);
    buddy_anim_init(&anim, 5);
    assert(buddy_anim_frame(&anim, &info, BUDDY_PACK_IDLE, BUDDY_MOOD_NORMAL, 0, &frame));
    assert(buddy_anim_frame(&anim, &info, BUDDY_PACK_IDLE, BUDDY_MOOD_NORMAL, 1000000000ULL,
                            &frame));
    assert(frame < 16U);
}

static void test_no_info_shows_nothing(void)
{
    buddy_anim_t anim;
    uint16_t frame;

    buddy_anim_init(&anim, 0);
    assert(!buddy_anim_frame(&anim, NULL, BUDDY_PACK_IDLE, BUDDY_MOOD_NORMAL, 0, &frame));
}

int main(void)
{
    test_state_change_starts_at_frame_zero();
    test_base_runs_between_specials();
    test_single_idle_clip_always_base();
    test_mood_clip_share();
    test_missing_mood_clip_ignored();
    test_pack_change_restarts();
    test_clip_table_change_with_same_crc();
    test_long_gap_returns_quickly();
    test_no_info_shows_nothing();
    return 0;
}
