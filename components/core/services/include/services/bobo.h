#pragma once
#include <stdbool.h>
#include <stdint.h>

#define BOBO_CARD_COUNT 12
#define BOBO_DESTINATION_COUNT 3
#define BOBO_NO_CARD UINT8_MAX
#define BOBO_MAX_SNACKS 9
#define BOBO_PET_COUNT 5
#define BOBO_PET_MASK ((1u << BOBO_PET_COUNT) - 1u)

// Awake time only: off/sleep time never advances travel or neglect.
#define BOBO_ILL_AFTER_S (6u * 60u * 60u)
#define BOBO_DEATH_AFTER_S (12u * 60u * 60u)

typedef enum { BOBO_WELL, BOBO_ILL, BOBO_DEAD } bobo_health_t;
typedef enum { BOBO_HOME, BOBO_TRAVELLING, BOBO_READY } bobo_phase_t;
typedef enum { BOBO_BABY, BOBO_JUNIOR, BOBO_GROWN } bobo_growth_t;
typedef enum {
    BOBO_PET_BOBO, BOBO_PET_SUNFLOWER, BOBO_PET_CACTUS, BOBO_PET_PANDA, BOBO_PET_LELE
} bobo_pet_t;
typedef enum { BOBO_NO_TOOL, BOBO_SKETCHBOOK, BOBO_LENS } bobo_tool_t;
typedef enum { BOBO_WATER, BOBO_TIDY, BOBO_CHAT, BOBO_SNACK } bobo_care_t;
typedef enum {
    BOBO_OK, BOBO_INVALID, BOBO_LOCKED, BOBO_BUSY, BOBO_AWAY,
    BOBO_COOLDOWN, BOBO_FULL, BOBO_EMPTY, BOBO_NOT_READY, BOBO_ALREADY_OWNED
} bobo_result_t;

typedef struct {
    uint8_t card_id, snacks, energy, mood;
    uint32_t sequence;
} bobo_receipt_t;
typedef struct {
    uint8_t destination, card_id, snacks;
    uint16_t duration_s, remaining_s;
} bobo_trip_t;
typedef struct {
    uint8_t energy, mood, tidy, snacks, tool, target_card, phase, intro_used;
    uint16_t collected, cooldown_s[4], recovery_s;
    uint32_t trip_sequence;
    bobo_trip_t trip;
    bobo_receipt_t receipt;
    uint8_t active_pet, owned_pets, pending_reveal;
    uint8_t newpet_journeys[BOBO_PET_COUNT - 1];
    uint32_t draw_sequence;
    uint32_t awake_s, neglect_s;
    uint8_t life_journeys, health;
} bobo_state_t;
typedef struct {
    uint8_t destination, card_id, snacks, capacity_snacks, energy_cost, tidy_cost;
    uint16_t duration_s;
} bobo_trip_plan_t;

// No allocation or clocks. Caller validates elapsed time and commits candidate states.
void bobo_init(bobo_state_t *s);
bool bobo_valid(const bobo_state_t *s);
unsigned bobo_count(const bobo_state_t *s);
const char *bobo_pet_name(unsigned pet);
const char *bobo_pet_stage_name(unsigned pet, bobo_growth_t stage);
bool bobo_pet_owned(const bobo_state_t *s, unsigned pet);
unsigned bobo_pet_count(const bobo_state_t *s);
// Draw changes a candidate state only. Commit it before showing the gift reveal.
// Pending reveals block further draws/selection; reopen and replay the same active pet.
// Only no companion or a deceased companion permits a new wish. Historical species
// bits survive migration but may never be used to replace the living companion.
bool bobo_pet_alive(const bobo_state_t *s);
bobo_result_t bobo_pet_draw(bobo_state_t *s, uint32_t random_value);
bobo_result_t bobo_pet_select(bobo_state_t *s, unsigned pet);
bobo_result_t bobo_pet_reveal_ack(bobo_state_t *s);
// Growth belongs to this life; legacy growth is imported by the save decoder.
bobo_growth_t bobo_growth_stage(const bobo_state_t *s);
unsigned bobo_growth_remaining(const bobo_state_t *s);
unsigned bobo_destination_count(const bobo_state_t *s, unsigned destination);
bool bobo_destination_unlocked(const bobo_state_t *s, unsigned destination);
void bobo_advance(bobo_state_t *s, uint32_t elapsed_s);
bobo_result_t bobo_care(bobo_state_t *s, bobo_care_t action);
bobo_result_t bobo_equip(bobo_state_t *s, bobo_tool_t tool);
bobo_result_t bobo_target(bobo_state_t *s, uint8_t card);
bobo_result_t bobo_preview(const bobo_state_t *s, unsigned destination, bobo_trip_plan_t *out);
bobo_result_t bobo_start(bobo_state_t *s, unsigned destination);
bobo_result_t bobo_cancel(bobo_state_t *s);
bobo_result_t bobo_claim(bobo_state_t *s);
