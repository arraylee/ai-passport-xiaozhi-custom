#include "services/bobo.h"
#include <string.h>

static const uint16_t durations[] = {600, 1200, 1800};
static const uint16_t cooldowns[] = {600, 300, 300, 10};
static uint8_t add(uint8_t n, unsigned amount) {
    return n + amount > 100 ? 100 : (uint8_t)(n + amount);
}
static uint8_t spend(uint8_t n, uint8_t amount) {
    return n < 20 + amount ? 20 : n - amount;
}
static bool owns(const bobo_state_t *s, unsigned card) {
    return card < BOBO_CARD_COUNT && (s->collected & (1u << card));
}
const char *bobo_pet_name(unsigned pet) {
    static const char *const names[] = {"博博", "向日葵", "仙人掌", "熊猫", "小熊乐乐"};
    return pet < BOBO_PET_COUNT ? names[pet] : NULL;
}
const char *bobo_pet_stage_name(unsigned pet, bobo_growth_t stage) {
    static const char *const names[BOBO_PET_COUNT][3] = {
        {"宝宝熊", "少年熊", "大熊"},
        {"小花芽", "小向日葵", "大向日葵"},
        {"小嫩芽", "小仙人掌", "大仙人掌"},
        {"宝宝熊猫", "少年熊猫", "大熊猫"},
        {"宝宝乐乐", "小熊乐乐", "大熊乐乐"},
    };
    return pet < BOBO_PET_COUNT && (unsigned)stage <= BOBO_GROWN ? names[pet][stage] : NULL;
}
bool bobo_pet_owned(const bobo_state_t *s, unsigned pet) {
    return s && pet < BOBO_PET_COUNT && (s->owned_pets & (1u << pet));
}
unsigned bobo_pet_count(const bobo_state_t *s) {
    unsigned count = 0;
    for (unsigned pet = 0; pet < BOBO_PET_COUNT; ++pet) count += bobo_pet_owned(s, pet);
    return count;
}
static uint32_t choice_uniform_bounded(uint32_t random_value, uint32_t bound) {
    // Use the whole random word; buckets differ by at most one input.
    return (uint32_t)(((uint64_t)random_value * bound) >> 32);
}
bool bobo_pet_alive(const bobo_state_t *s) {
    return s && s->owned_pets && s->health != BOBO_DEAD;
}
bobo_result_t bobo_pet_draw(bobo_state_t *s, uint32_t random_value) {
    if (!bobo_valid(s)) return BOBO_INVALID;
    if (s->phase != BOBO_HOME || s->pending_reveal || bobo_pet_alive(s)) return BOBO_BUSY;
    unsigned pet = choice_uniform_bounded(random_value, BOBO_PET_COUNT);
    s->owned_pets |= 1u << pet;
    s->active_pet = pet;
    s->pending_reveal = 1;
    ++s->draw_sequence;
    s->energy = s->mood = 70; s->tidy = 80;
    memset(s->cooldown_s, 0, sizeof(s->cooldown_s));
    s->recovery_s = 0;
    s->awake_s = s->neglect_s = 0;
    s->life_journeys = 0; s->health = BOBO_WELL;
    memset(&s->trip, 0, sizeof(s->trip)); s->trip.card_id = BOBO_NO_CARD;
    return BOBO_OK;
}
bobo_result_t bobo_pet_select(bobo_state_t *s, unsigned pet) {
    if (!bobo_valid(s) || pet >= BOBO_PET_COUNT) return BOBO_INVALID;
    if (s->phase != BOBO_HOME || s->pending_reveal) return BOBO_BUSY;
    if (!bobo_pet_alive(s) || pet != s->active_pet) return BOBO_LOCKED;
    return BOBO_OK;
}
bobo_result_t bobo_pet_reveal_ack(bobo_state_t *s) {
    if (!bobo_valid(s)) return BOBO_INVALID;
    if (!s->pending_reveal) return BOBO_NOT_READY;
    s->pending_reveal = 0;
    return BOBO_OK;
}
void bobo_init(bobo_state_t *s) {
    memset(s, 0, sizeof(*s));
    s->energy = s->mood = 70;
    s->tidy = 80;
    s->snacks = 2;
    s->target_card = s->trip.card_id = s->receipt.card_id = BOBO_NO_CARD;
}
bool bobo_valid(const bobo_state_t *s) {
    if (!s || s->energy > 100 || s->mood > 100 ||
        s->tidy < 20 || s->tidy > 100 || s->snacks > 9 || s->tool > BOBO_LENS ||
        s->phase > BOBO_READY || s->intro_used > 1 || s->collected >= (1u << 12) ||
        s->recovery_s >= 600 || (s->target_card != BOBO_NO_CARD &&
        (s->target_card >= BOBO_CARD_COUNT || owns(s, s->target_card)))) return false;
    if (s->active_pet >= BOBO_PET_COUNT || (s->owned_pets & ~BOBO_PET_MASK) ||
        (s->owned_pets ? !bobo_pet_owned(s, s->active_pet) : s->active_pet != BOBO_PET_BOBO) ||
        s->pending_reveal > 1 || (s->pending_reveal && (!s->owned_pets || s->phase != BOBO_HOME))) return false;
    if (s->life_journeys > 8 || s->health > BOBO_DEAD || s->neglect_s > BOBO_DEATH_AFTER_S ||
        s->awake_s < s->neglect_s ||
        (!s->owned_pets && (s->health || s->awake_s || s->neglect_s || s->life_journeys)) ||
        (s->health == BOBO_WELL && s->neglect_s >= BOBO_ILL_AFTER_S) ||
        (s->health == BOBO_ILL && (s->neglect_s < BOBO_ILL_AFTER_S || s->neglect_s >= BOBO_DEATH_AFTER_S)) ||
        (s->health == BOBO_DEAD && (s->neglect_s != BOBO_DEATH_AFTER_S || s->pending_reveal ||
         s->phase != BOBO_HOME || s->energy || s->mood)) ||
        (s->health != BOBO_DEAD && (s->energy < 20 || s->mood < 40))) return false;
    for (unsigned i = 0; i < BOBO_PET_COUNT - 1; ++i)
        if (s->newpet_journeys[i] > 8 || (s->newpet_journeys[i] && !bobo_pet_owned(s, i + 1))) return false;
    for (unsigned i = 0; i < 4; ++i) if (s->cooldown_s[i] > cooldowns[i]) return false;
    if (s->trip.destination >= 3 || s->trip.snacks > 2 ||
        s->trip.duration_s > 1800 || s->trip.remaining_s > s->trip.duration_s ||
        (s->trip.card_id != BOBO_NO_CARD && (s->trip.card_id >= 12 ||
        s->trip.card_id / 4 != s->trip.destination))) return false;
    if (s->phase != BOBO_HOME && (!s->intro_used || !s->trip.duration_s ||
        !s->trip.snacks || owns(s, s->trip.card_id))) return false;
    if ((s->phase == BOBO_TRAVELLING) != (s->trip.remaining_s > 0)) return false;
    if (s->receipt.card_id != BOBO_NO_CARD && !owns(s, s->receipt.card_id)) return false;
    return s->receipt.snacks <= 2 && s->receipt.energy <= 10 && s->receipt.mood <= 8;
}
unsigned bobo_count(const bobo_state_t *s) {
    unsigned count = 0;
    for (unsigned i = 0; i < BOBO_CARD_COUNT; ++i) count += owns(s, i);
    return count;
}
static unsigned growth_count(const bobo_state_t *s) {
    return s->life_journeys;
}
bobo_growth_t bobo_growth_stage(const bobo_state_t *s) {
    unsigned count = growth_count(s);
    return count >= 8 ? BOBO_GROWN : count >= 3 ? BOBO_JUNIOR : BOBO_BABY;
}
unsigned bobo_growth_remaining(const bobo_state_t *s) {
    unsigned count = growth_count(s);
    return count >= 8 ? 0 : (count >= 3 ? 8 : 3) - count;
}
unsigned bobo_destination_count(const bobo_state_t *s, unsigned destination) {
    if (destination >= 3) return 0;
    unsigned count = 0;
    for (unsigned i = destination * 4; i < destination * 4 + 4; ++i) count += owns(s, i);
    return count;
}
bool bobo_destination_unlocked(const bobo_state_t *s, unsigned destination) {
    static const uint8_t required[] = {0, 1, 3};
    return destination < 3 && bobo_count(s) >= required[destination];
}
void bobo_advance(bobo_state_t *s, uint32_t elapsed_s) {
    if (!bobo_valid(s) || !bobo_pet_alive(s) || s->pending_reveal || !elapsed_s) return;
    uint32_t remaining = BOBO_DEATH_AFTER_S - s->neglect_s;
    if (elapsed_s > remaining) elapsed_s = remaining;
    uint64_t awake = (uint64_t)s->awake_s + elapsed_s;
    s->awake_s = awake > UINT32_MAX ? UINT32_MAX : (uint32_t)awake;
    s->neglect_s += elapsed_s;
    if (s->neglect_s == BOBO_DEATH_AFTER_S) {
        s->health = BOBO_DEAD; s->phase = BOBO_HOME;
        s->trip.remaining_s = 0; s->energy = s->mood = 0;
        return;
    }
    if (s->neglect_s >= BOBO_ILL_AFTER_S) s->health = BOBO_ILL;
    for (unsigned i = 0; i < 4; ++i)
        s->cooldown_s[i] = elapsed_s >= s->cooldown_s[i] ? 0 : s->cooldown_s[i] - elapsed_s;
    uint32_t home_s = elapsed_s;
    if (s->phase == BOBO_TRAVELLING) {
        if (elapsed_s < s->trip.remaining_s) {
            s->trip.remaining_s -= elapsed_s;
            home_s = 0;
        } else {
            home_s -= s->trip.remaining_s;
            s->trip.remaining_s = 0;
            s->phase = BOBO_READY;
        }
    }
    // Caller supplies monotonic awake seconds, never a wall-clock difference.
    uint64_t recovery = (uint64_t)home_s + s->recovery_s;
    if (s->energy < 80) {
        uint64_t value = s->energy + (recovery / 600) * 2;
        s->energy = value > 80 ? 80 : (uint8_t)value;
    }
    s->recovery_s = recovery % 600;
}
bobo_result_t bobo_care(bobo_state_t *s, bobo_care_t action) {
    if (!bobo_valid(s) || (unsigned)action > BOBO_SNACK) return BOBO_INVALID;
    if (!bobo_pet_alive(s) || s->pending_reveal) return BOBO_LOCKED;
    if (s->phase == BOBO_TRAVELLING) return BOBO_AWAY;
    if (s->cooldown_s[action]) return BOBO_COOLDOWN;
    if (action == BOBO_SNACK && !s->snacks) return BOBO_EMPTY;
    uint8_t energy = s->energy, mood = s->mood, tidy = s->tidy;
    switch (action) {
        case BOBO_WATER: energy = add(energy, 15); break;
        case BOBO_TIDY: tidy = add(tidy, 30); break;
        case BOBO_CHAT: mood = add(mood, 10); break;
        case BOBO_SNACK: energy = add(energy, 25); mood = add(mood, 5); break;
    }
    if (energy == s->energy && mood == s->mood && tidy == s->tidy && !s->neglect_s) return BOBO_FULL;
    s->energy = energy; s->mood = mood; s->tidy = tidy;
    s->neglect_s = 0; s->health = BOBO_WELL;
    s->cooldown_s[action] = cooldowns[action];
    if (action == BOBO_SNACK) --s->snacks;
    return BOBO_OK;
}
bobo_result_t bobo_equip(bobo_state_t *s, bobo_tool_t tool) {
    if ((unsigned)tool > BOBO_LENS) return BOBO_INVALID;
    s->tool = tool;
    return BOBO_OK;
}
bobo_result_t bobo_target(bobo_state_t *s, uint8_t card) {
    if (card != BOBO_NO_CARD && card >= 12) return BOBO_INVALID;
    if (owns(s, card)) return BOBO_ALREADY_OWNED;
    s->target_card = card;
    return BOBO_OK;
}
bobo_result_t bobo_preview(const bobo_state_t *s, unsigned destination, bobo_trip_plan_t *out) {
    if (!bobo_valid(s) || destination >= 3 || !out) return BOBO_INVALID;
    if (!bobo_pet_alive(s)) return BOBO_LOCKED;
    if (s->phase != BOBO_HOME || s->pending_reveal) return BOBO_BUSY;
    if (!bobo_destination_unlocked(s, destination)) return BOBO_LOCKED;
    memset(out, 0, sizeof(*out));
    out->destination = destination;
    out->duration_s = s->intro_used ? durations[destination] : 180;
    out->snacks = s->energy >= 60 && s->tidy >= 60 ? 2 : 1;
    unsigned capacity = BOBO_MAX_SNACKS - s->snacks;
    out->capacity_snacks = capacity < out->snacks ? capacity : out->snacks;
    out->energy_cost = out->tidy_cost = 10 + destination * 5;
    out->card_id = BOBO_NO_CARD;
    unsigned base = destination * 4;
    if (s->target_card < 12 && s->target_card / 4 == destination && !owns(s, s->target_card)) {
        out->card_id = s->target_card;
        return BOBO_OK;
    }
    if (s->tool != BOBO_NO_TOOL) {
        unsigned start = base + (s->tool == BOBO_LENS ? 2 : 0);
        for (unsigned i = start; i < start + 2; ++i) {
            if (!owns(s, i)) { out->card_id = i; return BOBO_OK; }
        }
    }
    for (unsigned i = base; i < base + 4; ++i) {
        if (!owns(s, i)) { out->card_id = i; break; }
    }
    return BOBO_OK;
}
bobo_result_t bobo_start(bobo_state_t *s, unsigned destination) {
    bobo_trip_plan_t p;
    bobo_result_t result = bobo_preview(s, destination, &p);
    if (result != BOBO_OK) return result;
    s->trip = (bobo_trip_t){.destination = p.destination, .card_id = p.card_id,
        .snacks = p.snacks, .duration_s = p.duration_s, .remaining_s = p.duration_s};
    s->phase = BOBO_TRAVELLING;
    s->intro_used = 1;
    ++s->trip_sequence;
    s->energy = spend(s->energy, p.energy_cost);
    s->tidy = spend(s->tidy, p.tidy_cost);
    return BOBO_OK;
}
bobo_result_t bobo_cancel(bobo_state_t *s) {
    if (s->phase != BOBO_TRAVELLING) return BOBO_NOT_READY;
    s->phase = BOBO_HOME;
    s->trip.remaining_s = 0;
    return BOBO_OK;
}
bobo_result_t bobo_claim(bobo_state_t *s) {
    if (s->phase != BOBO_READY) return BOBO_NOT_READY;
    bobo_receipt_t receipt = {.card_id = s->trip.card_id, .sequence = s->trip_sequence};
    unsigned capacity = BOBO_MAX_SNACKS - s->snacks;
    receipt.snacks = capacity < s->trip.snacks ? capacity : s->trip.snacks;
    receipt.energy = add(s->energy, 10) - s->energy;
    receipt.mood = add(s->mood, 8) - s->mood;
    if (receipt.card_id < 12) s->collected |= 1u << receipt.card_id;
    if (owns(s, s->target_card)) s->target_card = BOBO_NO_CARD;
    s->snacks += receipt.snacks;
    s->energy += receipt.energy;
    s->mood += receipt.mood;
    s->receipt = receipt;
    if (s->life_journeys < 8) ++s->life_journeys;
    s->phase = BOBO_HOME;
    return BOBO_OK;
}
