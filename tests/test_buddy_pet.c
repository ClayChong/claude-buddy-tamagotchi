#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "buddy_pet.h"

static void test_init_is_eighty(void)
{
    buddy_pet_t pet;

    buddy_pet_init(&pet);
    assert(pet.fullness == 80 && pet.happiness == 80 && pet.energy == 80);
}

static void test_fullness_decay(void)
{
    buddy_pet_t pet;

    buddy_pet_init(&pet);
    buddy_pet_elapse(&pet, 864U * 10U, BUDDY_PET_AWAKE);
    assert(pet.fullness == 70);
    buddy_pet_elapse(&pet, 432, BUDDY_PET_AWAKE);
    assert(pet.fullness == 70);
    buddy_pet_elapse(&pet, 432, BUDDY_PET_AWAKE);
    assert(pet.fullness == 69);
}

static void test_happiness_decay(void)
{
    buddy_pet_t pet;

    buddy_pet_init(&pet);
    buddy_pet_elapse(&pet, 7200, BUDDY_PET_AWAKE);
    assert(pet.happiness == 70);
}

static void test_energy_by_activity(void)
{
    buddy_pet_t pet;

    buddy_pet_init(&pet);
    buddy_pet_elapse(&pet, 3000, BUDDY_PET_BUSY);
    assert(pet.energy == 70);
    buddy_pet_elapse(&pet, 1200, BUDDY_PET_SLEEPING);
    assert(pet.energy == 80);
    buddy_pet_elapse(&pet, 18000, BUDDY_PET_AWAKE);
    assert(pet.energy == 70);
}

static void test_feed_refused_when_full(void)
{
    buddy_pet_t pet;

    buddy_pet_init(&pet);
    pet.fullness = 89;
    assert(buddy_pet_feed(&pet));
    assert(pet.fullness == 100);
    assert(!buddy_pet_feed(&pet));
    assert(pet.fullness == 100);
}

static void test_play_pat_and_clamp(void)
{
    buddy_pet_t pet;

    buddy_pet_init(&pet);
    buddy_pet_play(&pet);
    assert(pet.happiness == 100);
    pet.happiness = 50;
    buddy_pet_pat(&pet);
    assert(pet.happiness == 55);
    buddy_pet_add(&pet, BUDDY_PET_HAPPINESS, -200);
    assert(pet.happiness == 0);
    buddy_pet_add(&pet, BUDDY_PET_ENERGY, 500);
    assert(pet.energy == 100);
}

static buddy_mood_t mood_of(uint8_t fullness, uint8_t happiness, uint8_t energy)
{
    buddy_pet_t pet;

    buddy_pet_init(&pet);
    pet.fullness = fullness;
    pet.happiness = happiness;
    pet.energy = energy;
    return buddy_pet_mood(&pet);
}

static void test_mood_order(void)
{
    assert(mood_of(24, 80, 10) == BUDDY_MOOD_HUNGRY);
    assert(mood_of(50, 80, 19) == BUDDY_MOOD_TIRED);
    assert(mood_of(50, 24, 50) == BUDDY_MOOD_SAD);
    assert(mood_of(70, 70, 70) == BUDDY_MOOD_HAPPY);
    assert(mood_of(69, 70, 70) == BUDDY_MOOD_NORMAL);
}

static void test_pack_round_trip(void)
{
    buddy_pet_t pet;
    buddy_pet_t loaded;
    uint32_t epoch = 0;

    buddy_pet_init(&pet);
    pet.fullness = 12;
    pet.happiness = 34;
    pet.energy = 56;
    assert(buddy_pet_unpack(buddy_pet_pack(&pet, 1767225600U), &loaded, &epoch));
    assert(loaded.fullness == 12 && loaded.happiness == 34 && loaded.energy == 56);
    assert(epoch == 1767225600U);
    assert(!buddy_pet_unpack(((uint64_t)1767225600U << 32) | 101U, &loaded, &epoch));
}

static void test_huge_elapse_clamps(void)
{
    buddy_pet_t pet;

    buddy_pet_init(&pet);
    buddy_pet_elapse(&pet, 100, BUDDY_PET_BUSY);
    buddy_pet_elapse(&pet, UINT32_MAX, BUDDY_PET_BUSY);
    assert(pet.fullness == 0 && pet.happiness == 0 && pet.energy == 0);
}

static void test_bar_segments(void)
{
    assert(buddy_pet_segments(0) == 0);
    assert(buddy_pet_segments(6) == 0);
    assert(buddy_pet_segments(7) == 1);
    assert(buddy_pet_segments(50) == 4);
    assert(buddy_pet_segments(100) == 8);
}

static void test_mood_names(void)
{
    assert(strcmp(buddy_mood_name(BUDDY_MOOD_HAPPY), "HAPPY") == 0);
    assert(strcmp(buddy_mood_name(BUDDY_MOOD_NORMAL), "OK") == 0);
    assert(strcmp(buddy_mood_name(BUDDY_MOOD_HUNGRY), "HUNGRY") == 0);
    assert(strcmp(buddy_mood_name(BUDDY_MOOD_TIRED), "TIRED") == 0);
    assert(strcmp(buddy_mood_name(BUDDY_MOOD_SAD), "SAD") == 0);
}

int main(void)
{
    test_init_is_eighty();
    test_fullness_decay();
    test_happiness_decay();
    test_energy_by_activity();
    test_feed_refused_when_full();
    test_play_pat_and_clamp();
    test_mood_order();
    test_pack_round_trip();
    test_huge_elapse_clamps();
    test_bar_segments();
    test_mood_names();
    return 0;
}
