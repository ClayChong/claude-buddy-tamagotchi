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

static void test_energy_counts_short_mixed_spells(void)
{
    buddy_pet_t pet;
    unsigned i;

    buddy_pet_init(&pet);
    for (i = 0; i < 120U; ++i) {
        buddy_pet_elapse(&pet, 120, BUDDY_PET_BUSY);
        buddy_pet_elapse(&pet, 120, BUDDY_PET_AWAKE);
    }
    /* 8 h of 2-minute busy and idle spells: 120 * (120/300 + 120/1800) = 56. */
    assert(pet.energy == 24);
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

static void test_poop_pile_caps_at_four(void)
{
    buddy_pet_t pet;
    unsigned i;

    buddy_pet_init(&pet);
    assert(pet.poop == 0);
    for (i = 0; i < BUDDY_PET_POOP_MAX; ++i) {
        assert(buddy_pet_add_poop(&pet));
    }
    assert(!buddy_pet_add_poop(&pet));
    assert(pet.poop == 4);
}

static void test_clean_removes_whole_pile(void)
{
    buddy_pet_t pet;

    buddy_pet_init(&pet);
    assert(!buddy_pet_clean(&pet));
    buddy_pet_add_poop(&pet);
    buddy_pet_add_poop(&pet);
    buddy_pet_add_poop(&pet);
    assert(buddy_pet_clean(&pet));
    assert(pet.poop == 0);
    assert(pet.happiness == 80);
}

/* Each poop costs one extra happiness point per 1800 s, on top of the normal
 * 720 s decay. */
static void test_poop_penalty(void)
{
    buddy_pet_t clean_pet;
    buddy_pet_t pet;

    buddy_pet_init(&clean_pet);
    buddy_pet_init(&pet);
    buddy_pet_add_poop(&pet);
    buddy_pet_add_poop(&pet);
    buddy_pet_elapse(&clean_pet, 3600, BUDDY_PET_AWAKE);
    buddy_pet_elapse(&pet, 3600, BUDDY_PET_AWAKE);
    assert(clean_pet.happiness - pet.happiness == 4);

    buddy_pet_init(&pet);
    buddy_pet_add_poop(&pet);
    buddy_pet_elapse(&pet, 900, BUDDY_PET_AWAKE);
    assert(pet.happiness == 79);
    buddy_pet_elapse(&pet, 900, BUDDY_PET_AWAKE);
    assert(pet.happiness == 77);
}

static void test_stinky_mood(void)
{
    buddy_pet_t pet;

    buddy_pet_init(&pet);
    buddy_pet_add_poop(&pet);
    assert(buddy_pet_mood(&pet) == BUDDY_MOOD_STINKY);
    pet.energy = 10;
    pet.happiness = 10;
    assert(buddy_pet_mood(&pet) == BUDDY_MOOD_STINKY);
    pet.fullness = 10;
    assert(buddy_pet_mood(&pet) == BUDDY_MOOD_HUNGRY);
    assert(strcmp(buddy_mood_name(BUDDY_MOOD_STINKY), "STINKY") == 0);
}

static void test_pack_keeps_poop(void)
{
    buddy_pet_t pet;
    buddy_pet_t loaded;
    uint32_t epoch = 0;
    uint64_t packed;

    buddy_pet_init(&pet);
    buddy_pet_add_poop(&pet);
    buddy_pet_add_poop(&pet);
    buddy_pet_add_poop(&pet);
    packed = buddy_pet_pack(&pet, 1767225600U);
    assert(((packed >> 24) & 0xffU) == 3U);
    assert(buddy_pet_unpack(packed, &loaded, &epoch));
    assert(loaded.poop == 3);
    assert(!buddy_pet_unpack(packed | (5ULL << 24), &loaded, &epoch));
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
    test_energy_counts_short_mixed_spells();
    test_feed_refused_when_full();
    test_play_pat_and_clamp();
    test_mood_order();
    test_pack_round_trip();
    test_poop_pile_caps_at_four();
    test_clean_removes_whole_pile();
    test_poop_penalty();
    test_stinky_mood();
    test_pack_keeps_poop();
    test_huge_elapse_clamps();
    test_bar_segments();
    test_mood_names();
    return 0;
}
