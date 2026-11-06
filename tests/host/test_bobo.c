#include "services/bobo.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static void adopt(bobo_state_t *s, uint32_t random) {
    bobo_init(s);
    assert(bobo_pet_draw(s, random) == BOBO_OK);
    assert(bobo_pet_reveal_ack(s) == BOBO_OK);
}
static void collect(bobo_state_t *s, unsigned destination) {
    assert(bobo_start(s, destination) == BOBO_OK);
    bobo_advance(s, s->trip.remaining_s);
    assert(s->phase == BOBO_READY);
    assert(bobo_claim(s) == BOBO_OK);
}
static void test_one_companion(void) {
    bobo_state_t s, before;
    adopt(&s, UINT32_MAX);
    assert(s.active_pet == BOBO_PET_LELE);
    before = s;
    assert(bobo_pet_draw(&s, 0) == BOBO_BUSY);
    assert(bobo_pet_select(&s, BOBO_PET_BOBO) == BOBO_LOCKED);
    assert(memcmp(&s, &before, sizeof(s)) == 0);
    s.owned_pets = BOBO_PET_MASK; // Migrated collection still cannot select hidden companions.
    before = s;
    assert(bobo_pet_select(&s, BOBO_PET_BOBO) == BOBO_LOCKED);
    assert(bobo_pet_draw(&s, 0) == BOBO_BUSY);
    assert(memcmp(&s, &before, sizeof(s)) == 0);
    assert(bobo_pet_select(&s, s.active_pet) == BOBO_OK);
    assert(bobo_pet_select(&s, BOBO_PET_COUNT) == BOBO_INVALID);
    assert(bobo_pet_draw(NULL, 0) == BOBO_INVALID);
    assert(bobo_pet_reveal_ack(&s) == BOBO_NOT_READY);
}
static void test_collection_and_care(void) {
    bobo_state_t s, before; bobo_trip_plan_t p;
    adopt(&s, 0);
    assert(bobo_preview(&s, 0, &p) == BOBO_OK && p.duration_s == 180 && p.snacks == 2);
    assert(bobo_preview(&s, 1, &p) == BOBO_LOCKED);
    assert(bobo_equip(&s, BOBO_LENS) == BOBO_OK);
    assert(bobo_start(&s, 0) == BOBO_OK && s.trip.card_id == 2);
    assert(bobo_care(&s, BOBO_WATER) == BOBO_AWAY);
    bobo_advance(&s, 179); assert(s.trip.remaining_s == 1);
    bobo_advance(&s, 1); assert(bobo_claim(&s) == BOBO_OK);
    before = s; assert(bobo_claim(&s) == BOBO_NOT_READY); assert(!memcmp(&s, &before, sizeof(s)));
    assert(bobo_target(&s, 1) == BOBO_OK);
    collect(&s, 0); assert(s.target_card == BOBO_NO_CARD);
    assert(bobo_target(&s, 1) == BOBO_ALREADY_OWNED);
    collect(&s, 0);
    for (unsigned d = 0; d < 3; ++d) {
        while (bobo_destination_count(&s, d) < 4) {
            unsigned old = bobo_count(&s); collect(&s, d);
            assert(bobo_count(&s) == old + 1 && bobo_valid(&s));
        }
    }
    assert(bobo_count(&s) == 12 && s.snacks == 9);
    collect(&s, 2); assert(s.receipt.card_id == BOBO_NO_CARD && !s.receipt.snacks);
    adopt(&s, 0);
    assert(bobo_care(&s, BOBO_WATER) == BOBO_OK && s.energy == 85);
    before = s; assert(bobo_care(&s, BOBO_WATER) == BOBO_COOLDOWN); assert(!memcmp(&s, &before, sizeof(s)));
    bobo_advance(&s, 600); assert(bobo_care(&s, BOBO_WATER) == BOBO_OK && s.energy == 100);
    bobo_advance(&s, 600); assert(bobo_care(&s, BOBO_WATER) == BOBO_OK);
    assert(s.neglect_s == 0);
    s.snacks = 0; assert(bobo_care(&s, BOBO_SNACK) == BOBO_EMPTY);
    assert(bobo_valid(&s));
}
static void test_lifetime_and_new_life(void) {
    bobo_state_t s, before;
    adopt(&s, 0); s.collected = 0xfff;
    for (unsigned trips = 0; trips < 8; ++trips) collect(&s, 0);
    assert(bobo_growth_stage(&s) == BOBO_GROWN);
    assert(s.neglect_s > 0); // Trips and rewards do not provide healthcare.
    assert(bobo_care(&s, BOBO_TIDY) == BOBO_OK && !s.neglect_s);
    bobo_advance(&s, BOBO_ILL_AFTER_S - 1);
    assert(s.health == BOBO_WELL);
    bobo_advance(&s, 1); assert(s.health == BOBO_ILL);
    before = s; assert(bobo_pet_draw(&s, 1) == BOBO_BUSY); assert(!memcmp(&s, &before, sizeof(s)));
    s.energy = s.mood = s.tidy = 100;
    assert(bobo_care(&s, BOBO_WATER) == BOBO_OK);
    assert(s.health == BOBO_WELL && !s.neglect_s); // Real care works even at full attributes.
    bobo_advance(&s, 1); before = s;
    assert(bobo_care(&s, BOBO_WATER) == BOBO_COOLDOWN); assert(!memcmp(&s, &before, sizeof(s)));
    bobo_advance(&s, BOBO_DEATH_AFTER_S - 2); assert(s.health == BOBO_ILL);
    bobo_advance(&s, 1); assert(s.health == BOBO_DEAD && bobo_valid(&s));
    before = s; bobo_advance(&s, UINT32_MAX); assert(!memcmp(&s, &before, sizeof(s)));
    assert(bobo_care(&s, BOBO_WATER) == BOBO_LOCKED && bobo_start(&s, 0) == BOBO_LOCKED);
    assert(bobo_pet_draw(&s, 0) == BOBO_OK); // Same species is a new baby, even with every card.
    assert(s.active_pet == BOBO_PET_BOBO && s.collected == 0xfff && s.pending_reveal);
    assert(bobo_growth_stage(&s) == BOBO_BABY && !s.awake_s && !s.neglect_s);
    before = s; bobo_advance(&s, UINT32_MAX); assert(!memcmp(&s, &before, sizeof(s)));
    assert(bobo_pet_reveal_ack(&s) == BOBO_OK);
    for (unsigned n = 1; n <= 8; ++n) {
        collect(&s, 0);
        assert(s.life_journeys == n);
        assert(bobo_growth_stage(&s) == (n >= 8 ? BOBO_GROWN : n >= 3 ? BOBO_JUNIOR : BOBO_BABY));
    }
    s.awake_s = UINT32_MAX - 2;
    bobo_advance(&s, UINT32_MAX);
    assert(s.awake_s == UINT32_MAX && s.health == BOBO_DEAD && bobo_valid(&s));
    before = s; bobo_advance(&s, UINT32_MAX); assert(!memcmp(&s, &before, sizeof(s)));
}
int main(void) {
    test_one_companion(); test_collection_and_care(); test_lifetime_and_new_life();
    puts("bobo single companion, collection and meaningful care PASS");
}
