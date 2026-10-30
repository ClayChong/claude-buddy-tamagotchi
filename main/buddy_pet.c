#include "buddy_pet.h"

#include <stddef.h>

#define BUDDY_PET_MAX 100U
#define BUDDY_PET_FULLNESS_S 864U
#define BUDDY_PET_HAPPINESS_S 720U
/* One energy point is 1800 units: 300 s busy, 1800 s awake or 120 s asleep. */
#define BUDDY_PET_ENERGY_POINT 1800
#define BUDDY_PET_ENERGY_BUSY 6
#define BUDDY_PET_ENERGY_AWAKE 1
#define BUDDY_PET_ENERGY_SLEEP (-15)
#define BUDDY_PET_FEED 30
#define BUDDY_PET_FEED_REFUSE 90U
#define BUDDY_PET_PLAY 20
#define BUDDY_PET_PAT 5

/* Moves value one point per period, towards 0 (down) or 100 (up). */
static uint8_t buddy_pet_step(uint8_t value, uint32_t *carry, uint32_t seconds,
                              uint32_t period, bool up)
{
    uint64_t total = (uint64_t)*carry + seconds;
    uint64_t steps = total / period;

    *carry = (uint32_t)(total % period);
    if (up) {
        return steps >= BUDDY_PET_MAX - value ? (uint8_t)BUDDY_PET_MAX : (uint8_t)(value + steps);
    }
    return steps >= value ? 0U : (uint8_t)(value - steps);
}

static uint8_t buddy_pet_clamp(int value)
{
    if (value < 0) return 0;
    if (value > (int)BUDDY_PET_MAX) return (uint8_t)BUDDY_PET_MAX;
    return (uint8_t)value;
}

void buddy_pet_init(buddy_pet_t *pet)
{
    if (pet == NULL) {
        return;
    }
    pet->fullness = BUDDY_PET_START;
    pet->happiness = BUDDY_PET_START;
    pet->energy = BUDDY_PET_START;
    pet->fullness_carry_s = 0;
    pet->happiness_carry_s = 0;
    pet->energy_units = 0;
}

void buddy_pet_elapse(buddy_pet_t *pet, uint32_t seconds, buddy_pet_activity_t activity)
{
    int64_t rate = BUDDY_PET_ENERGY_AWAKE;
    int64_t units;
    int64_t steps;

    if (pet == NULL) {
        return;
    }
    pet->fullness = buddy_pet_step(pet->fullness, &pet->fullness_carry_s, seconds,
                                   BUDDY_PET_FULLNESS_S, false);
    pet->happiness = buddy_pet_step(pet->happiness, &pet->happiness_carry_s, seconds,
                                    BUDDY_PET_HAPPINESS_S, false);
    if (activity == BUDDY_PET_BUSY) {
        rate = BUDDY_PET_ENERGY_BUSY;
    } else if (activity == BUDDY_PET_SLEEPING) {
        rate = BUDDY_PET_ENERGY_SLEEP;
    }
    units = pet->energy_units + rate * (int64_t)seconds;
    steps = units / BUDDY_PET_ENERGY_POINT;
    pet->energy_units = (int32_t)(units - steps * BUDDY_PET_ENERGY_POINT);
    if (steps >= (int64_t)pet->energy) {
        pet->energy = 0;
    } else if (-steps >= (int64_t)(BUDDY_PET_MAX - pet->energy)) {
        pet->energy = (uint8_t)BUDDY_PET_MAX;
    } else {
        pet->energy = (uint8_t)(pet->energy - steps);
    }
}

void buddy_pet_add(buddy_pet_t *pet, buddy_pet_stat_t stat, int delta)
{
    if (pet == NULL) {
        return;
    }
    switch (stat) {
    case BUDDY_PET_FULLNESS: pet->fullness = buddy_pet_clamp(pet->fullness + delta); break;
    case BUDDY_PET_HAPPINESS: pet->happiness = buddy_pet_clamp(pet->happiness + delta); break;
    case BUDDY_PET_ENERGY: pet->energy = buddy_pet_clamp(pet->energy + delta); break;
    }
}

bool buddy_pet_feed(buddy_pet_t *pet)
{
    if (pet == NULL || pet->fullness >= BUDDY_PET_FEED_REFUSE) {
        return false;
    }
    buddy_pet_add(pet, BUDDY_PET_FULLNESS, BUDDY_PET_FEED);
    return true;
}

void buddy_pet_play(buddy_pet_t *pet)
{
    buddy_pet_add(pet, BUDDY_PET_HAPPINESS, BUDDY_PET_PLAY);
}

void buddy_pet_pat(buddy_pet_t *pet)
{
    buddy_pet_add(pet, BUDDY_PET_HAPPINESS, BUDDY_PET_PAT);
}

buddy_mood_t buddy_pet_mood(const buddy_pet_t *pet)
{
    if (pet == NULL) return BUDDY_MOOD_NORMAL;
    if (pet->fullness < 25U) return BUDDY_MOOD_HUNGRY;
    if (pet->energy < 20U) return BUDDY_MOOD_TIRED;
    if (pet->happiness < 25U) return BUDDY_MOOD_SAD;
    if (pet->fullness >= 70U && pet->happiness >= 70U && pet->energy >= 70U) {
        return BUDDY_MOOD_HAPPY;
    }
    return BUDDY_MOOD_NORMAL;
}

unsigned buddy_pet_segments(uint8_t value)
{
    if (value > BUDDY_PET_MAX) {
        value = BUDDY_PET_MAX;
    }
    return ((unsigned)value * 8U + 50U) / 100U;
}

const char *buddy_mood_name(buddy_mood_t mood)
{
    switch (mood) {
    case BUDDY_MOOD_HAPPY: return "HAPPY";
    case BUDDY_MOOD_HUNGRY: return "HUNGRY";
    case BUDDY_MOOD_TIRED: return "TIRED";
    case BUDDY_MOOD_SAD: return "SAD";
    default: return "OK";
    }
}

uint64_t buddy_pet_pack(const buddy_pet_t *pet, uint32_t epoch_seconds)
{
    if (pet == NULL) {
        return 0;
    }
    return ((uint64_t)epoch_seconds << 32) | ((uint64_t)pet->energy << 16) |
           ((uint64_t)pet->happiness << 8) | pet->fullness;
}

bool buddy_pet_unpack(uint64_t packed, buddy_pet_t *pet, uint32_t *epoch_seconds)
{
    uint8_t fullness = (uint8_t)packed;
    uint8_t happiness = (uint8_t)(packed >> 8);
    uint8_t energy = (uint8_t)(packed >> 16);

    if (pet == NULL || fullness > BUDDY_PET_MAX || happiness > BUDDY_PET_MAX ||
        energy > BUDDY_PET_MAX) {
        return false;
    }
    buddy_pet_init(pet);
    pet->fullness = fullness;
    pet->happiness = happiness;
    pet->energy = energy;
    if (epoch_seconds != NULL) {
        *epoch_seconds = (uint32_t)(packed >> 32);
    }
    return true;
}
