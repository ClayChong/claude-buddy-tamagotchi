#pragma once

/*
 * Pack clip playback. A state starts at its first frame whenever it begins.
 * In idle, the base clip (idle clip 0, usually a blink) plays 1-3 times between
 * random special clips; a hungry, sad or tired mood can swap in its own clip.
 */

#include <stdbool.h>
#include <stdint.h>

#include "buddy_pack_format.h"
#include "buddy_types.h"

typedef struct {
    uint32_t rng;
    uint32_t pack_crc;
    uint64_t started_ms;
    buddy_pack_state_t state;
    buddy_pack_state_t clip_state;
    uint16_t clip;
    int16_t last_special;
    uint8_t bases_left;
    bool playing;
} buddy_anim_t;

void buddy_anim_init(buddy_anim_t *anim, uint32_t seed);
/* False when info is NULL or the state has nothing to show. */
bool buddy_anim_frame(buddy_anim_t *anim, const buddy_pack_info_t *info, buddy_pack_state_t state,
                      buddy_mood_t mood, uint64_t now_ms, uint16_t *frame);
