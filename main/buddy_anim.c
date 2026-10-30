#include "buddy_anim.h"

#include <stddef.h>

/* After this many clip changes in one call (the screen was off for a while),
 * start fresh at the current time instead of replaying the gap. */
#define BUDDY_ANIM_MAX_CATCH_UP 32U

static uint32_t buddy_anim_random(buddy_anim_t *anim)
{
    uint32_t x = anim->rng;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    anim->rng = x;
    return x;
}

/* One base play now, plus 0-2 more. */
static void buddy_anim_queue_bases(buddy_anim_t *anim)
{
    anim->clip_state = BUDDY_PACK_IDLE;
    anim->clip = 0;
    anim->bases_left = (uint8_t)(buddy_anim_random(anim) % 3U);
}

static void buddy_anim_restart(buddy_anim_t *anim, const buddy_pack_info_t *info,
                               buddy_pack_state_t state, uint64_t now_ms)
{
    anim->playing = true;
    anim->state = state;
    anim->pack_crc = info->payload_crc;
    anim->started_ms = now_ms;
    anim->last_special = -1;
    buddy_anim_queue_bases(anim);
}

static bool buddy_anim_mood_state(buddy_mood_t mood, buddy_pack_state_t *state)
{
    switch (mood) {
    case BUDDY_MOOD_HUNGRY: *state = BUDDY_PACK_HUNGRY; return true;
    case BUDDY_MOOD_TIRED: *state = BUDDY_PACK_TIRED; return true;
    case BUDDY_MOOD_SAD: *state = BUDDY_PACK_SAD; return true;
    case BUDDY_MOOD_STINKY: *state = BUDDY_PACK_STINKY; return true;
    default: return false;
    }
}

static void buddy_anim_pick_special(buddy_anim_t *anim, const buddy_pack_info_t *info,
                                    buddy_mood_t mood)
{
    unsigned specials = info->states[BUDDY_PACK_IDLE].clip_count - 1U;
    buddy_pack_state_t mood_state;
    buddy_pack_state_t resolved;
    unsigned pick;

    if (buddy_anim_mood_state(mood, &mood_state) &&
        buddy_pack_resolve_state(info, mood_state, &resolved) && resolved == mood_state &&
        buddy_anim_random(anim) % 2U == 0U) {
        anim->clip_state = mood_state;
        anim->clip = 0;
        return;
    }
    if (specials == 0U) {
        buddy_anim_queue_bases(anim);
        return;
    }
    if (specials >= 2U && anim->last_special > 0) {
        pick = 1U + buddy_anim_random(anim) % (specials - 1U);
        if (pick >= (unsigned)anim->last_special) {
            ++pick;
        }
    } else {
        pick = 1U + buddy_anim_random(anim) % specials;
    }
    anim->clip_state = BUDDY_PACK_IDLE;
    anim->clip = (uint16_t)pick;
    anim->last_special = (int16_t)pick;
}

static void buddy_anim_advance(buddy_anim_t *anim, const buddy_pack_info_t *info,
                               buddy_mood_t mood)
{
    if (anim->clip_state != BUDDY_PACK_IDLE || anim->clip != 0U) {
        buddy_anim_queue_bases(anim);
    } else if (anim->bases_left > 0U) {
        --anim->bases_left;
    } else {
        buddy_anim_pick_special(anim, info, mood);
    }
}

static const buddy_pack_clip_t *buddy_anim_clip(const buddy_anim_t *anim,
                                                const buddy_pack_info_t *info)
{
    return &info->clips[info->states[anim->clip_state].first_clip + anim->clip];
}

bool buddy_anim_frame(buddy_anim_t *anim, const buddy_pack_info_t *info, buddy_pack_state_t state,
                      buddy_mood_t mood, uint64_t now_ms, uint16_t *frame)
{
    const buddy_pack_clip_t *clip;
    uint64_t position;
    unsigned changes = 0;

    if (anim == NULL || info == NULL || frame == NULL ||
        info->states[BUDDY_PACK_IDLE].clip_count == 0U) {
        return false;
    }
    /* A re-uploaded pack can keep its payload CRC but have fewer clips. */
    if (!anim->playing || anim->state != state || anim->pack_crc != info->payload_crc ||
        now_ms < anim->started_ms ||
        anim->clip >= info->states[anim->clip_state].clip_count) {
        buddy_anim_restart(anim, info, state, now_ms);
    }
    if (state != BUDDY_PACK_IDLE) {
        return buddy_pack_frame_at(info, state, now_ms - anim->started_ms, frame);
    }

    clip = buddy_anim_clip(anim, info);
    while (now_ms - anim->started_ms >= (uint64_t)clip->frame_count * clip->frame_ms) {
        anim->started_ms += (uint64_t)clip->frame_count * clip->frame_ms;
        buddy_anim_advance(anim, info, mood);
        clip = buddy_anim_clip(anim, info);
        if (++changes >= BUDDY_ANIM_MAX_CATCH_UP) {
            anim->started_ms = now_ms;
        }
    }
    position = (now_ms - anim->started_ms) / clip->frame_ms;
    *frame = (uint16_t)(clip->first_frame + position);
    return true;
}

void buddy_anim_init(buddy_anim_t *anim, uint32_t seed)
{
    if (anim == NULL) {
        return;
    }
    anim->rng = seed == 0U ? 1U : seed;
    anim->playing = false;
}
