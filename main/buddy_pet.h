#pragma once

/*
 * Pet stats. Fullness, happiness and energy run from 0 to 100 and change with
 * real time; whole-second remainders carry over so frequent small updates add
 * up exactly.
 */

#include <stdbool.h>
#include <stdint.h>

#include "buddy_types.h"

#define BUDDY_PET_START 80U
#define BUDDY_PET_POOP_MAX 4U

typedef enum {
    BUDDY_PET_AWAKE,
    BUDDY_PET_BUSY,
    BUDDY_PET_SLEEPING,
} buddy_pet_activity_t;

typedef enum {
    BUDDY_PET_FULLNESS,
    BUDDY_PET_HAPPINESS,
    BUDDY_PET_ENERGY,
} buddy_pet_stat_t;

typedef struct {
    uint8_t fullness;
    uint8_t happiness;
    uint8_t energy;
    /* Uncleaned poops, 0..BUDDY_PET_POOP_MAX. */
    uint8_t poop;
    uint32_t fullness_carry_s;
    uint32_t happiness_carry_s;
    /* Energy drain in units of 1/1800 point: busy +6/s, awake +1/s, sleeping
     * -15/s, so short spells of each activity still add up. */
    int32_t energy_units;
    /* Poop-seconds towards the next happiness point lost to the pile. */
    uint32_t poop_carry_s;
} buddy_pet_t;

void buddy_pet_init(buddy_pet_t *pet);
void buddy_pet_elapse(buddy_pet_t *pet, uint32_t seconds, buddy_pet_activity_t activity);
/* Adds delta and clamps the stat to 0..100. */
void buddy_pet_add(buddy_pet_t *pet, buddy_pet_stat_t stat, int delta);
/* Fullness +30; false, with no change, when fullness is already 90 or more. */
bool buddy_pet_feed(buddy_pet_t *pet);
void buddy_pet_play(buddy_pet_t *pet);
void buddy_pet_pat(buddy_pet_t *pet);
/* One more poop; false, with no change, when the pile is already full. */
bool buddy_pet_add_poop(buddy_pet_t *pet);
/* Removes the whole pile; false when there was nothing to clean. */
bool buddy_pet_clean(buddy_pet_t *pet);
buddy_mood_t buddy_pet_mood(const buddy_pet_t *pet);
/* Lit segments of an 8-segment bar for a 0..100 stat. */
unsigned buddy_pet_segments(uint8_t value);
/* Upper-case word shown on the pet page. */
const char *buddy_mood_name(buddy_mood_t mood);
/* NVS value: epoch_seconds << 32 | poop << 24 | energy << 16 | happiness << 8 |
 * fullness. */
uint64_t buddy_pet_pack(const buddy_pet_t *pet, uint32_t epoch_seconds);
/* False, leaving pet untouched, when a stat is above 100 or the pile above
 * BUDDY_PET_POOP_MAX. */
bool buddy_pet_unpack(uint64_t packed, buddy_pet_t *pet, uint32_t *epoch_seconds);
