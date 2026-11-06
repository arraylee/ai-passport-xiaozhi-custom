#include "services/bobo_save.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t bytes[2][BOBO_SAVE_SIZE]; int sizes[2]; bool fail;
    unsigned read_errors; bool write_then_fail, partial_write, fail_read_after_write;
} fake_store_t;
static int slot(const char *key) { return strcmp(key, "bobo_a") == 0 ? 0 : 1; }
static int get(hal_kv_t *kv, const char *key, void *out, int cap, int *size) {
    fake_store_t *f = kv->impl; int i = slot(key);
    if (f->read_errors & (1u << i)) return HAL_KV_ERROR;
    if (!f->sizes[i]) return HAL_KV_NOT_FOUND;
    if (f->sizes[i] > cap) return HAL_KV_TOO_LARGE;
    memcpy(out, f->bytes[i], f->sizes[i]); *size = f->sizes[i]; return 0;
}
static int set(hal_kv_t *kv, const char *key, const void *in, int size) {
    fake_store_t *f = kv->impl; int i = slot(key);
    if (f->fail) return -1;
    assert(size == BOBO_SAVE_SIZE);
    memcpy(f->bytes[i], in, f->partial_write ? 30 : size); f->sizes[i] = size;
    if (f->fail_read_after_write) f->read_errors |= 1u << i;
    return f->write_then_fail || f->partial_write || f->fail_read_after_write ? -1 : 0;
}
static const hal_kv_api_t api = {.get = get, .set = set};

// Frozen v1 wire fixture from the original 80-byte format, independent of the v2 encoder.
static const uint8_t legacy_ready[BOBO_SAVE_SIZE] = {
    0x42, 0x4f, 0x42, 0x4f, 0x01, 0x00, 0x50, 0x00, 0x2a, 0x00, 0x00, 0x00, 0x00, 0xf1, 0x53, 0x65,
    0x2d, 0x40, 0x21, 0x08, 0x02, 0x08, 0x02, 0x01, 0xff, 0x00, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x7b, 0x00, 0x18, 0x00, 0x00, 0x00, 0x02, 0x08, 0x01, 0x08, 0x07, 0x00, 0x00, 0x07,
    0x01, 0x0a, 0x08, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x77, 0x6d, 0x64, 0x30,
};
// Frozen preview8 v2 wire data: current Panda, three journeys, four hidden species.
static const uint8_t legacy_v2_ready[BOBO_SAVE_SIZE] = {
    0x42, 0x4f, 0x42, 0x4f, 0x02, 0x00, 0x50, 0x00, 0x2a, 0x00, 0x00, 0x00, 0x00, 0xf1, 0x53, 0x65,
    0x2d, 0x40, 0x21, 0x08, 0x02, 0x08, 0x02, 0x01, 0xff, 0x00, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x7b, 0x00, 0x18, 0x00, 0x00, 0x00, 0x02, 0x08, 0x01, 0x08, 0x07, 0x00, 0x00, 0x07,
    0x01, 0x0a, 0x08, 0x17, 0x00, 0x00, 0x00, 0x03, 0x1f, 0x00, 0x03, 0x08, 0x03, 0x08, 0x2a, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa7, 0x8e, 0xcb, 0x0b,
};
static void repair_crc(uint8_t bytes[BOBO_SAVE_SIZE]) {
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < BOBO_SAVE_SIZE - 4; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    crc = ~crc;
    for (unsigned i = 0; i < 4; ++i) bytes[BOBO_SAVE_SIZE - 4 + i] = (uint8_t)(crc >> (8 * i));
}
static void test_legacy_migration(void) {
    bobo_state_t s, expected;
    uint32_t generation, epoch;
    assert(bobo_save_decode(legacy_ready, sizeof(legacy_ready), &s, &generation, &epoch));
    bobo_init(&expected);
    expected.energy = 45; expected.mood = 64; expected.tidy = 33; expected.snacks = 8;
    expected.tool = BOBO_LENS; expected.target_card = 8; expected.phase = BOBO_READY; expected.intro_used = 1;
    expected.collected = 255; expected.cooldown_s[0] = 21; expected.recovery_s = 123; expected.trip_sequence = 24;
    expected.trip = (bobo_trip_t){.destination = 2, .card_id = 8, .snacks = 1, .duration_s = 1800};
    expected.receipt = (bobo_receipt_t){.card_id = 7, .snacks = 1, .energy = 10, .mood = 8, .sequence = 23};
    expected.owned_pets = 1; expected.life_journeys = 8;
    assert(memcmp(&s, &expected, sizeof(s)) == 0);
    assert(generation == 42 && epoch == 1700000000 && bobo_growth_stage(&s) == BOBO_GROWN);
    assert(!s.pending_reveal && !s.draw_sequence && bobo_pet_count(&s) == 1);
    assert(bobo_pet_select(&s, BOBO_PET_PANDA) == BOBO_BUSY);
    assert(bobo_claim(&s) == BOBO_OK && s.collected == 511 && s.snacks == 9);
    assert(bobo_growth_stage(&s) == BOBO_GROWN && s.active_pet == BOBO_PET_BOBO);

    fake_store_t f = {0}; hal_kv_t kv = {.api = &api, .impl = &f};
    memcpy(f.bytes[0], legacy_ready, sizeof(legacy_ready)); f.sizes[0] = BOBO_SAVE_SIZE;
    bobo_journal_t j, boot;
    assert(bobo_journal_open(&j, &kv) == BOBO_SAVE_LOADED && j.state.owned_pets == 1);
    assert(bobo_journal_commit(&j, &s, epoch) == 0 && f.bytes[j.slot][4] == 3);
    assert(bobo_journal_open(&boot, &kv) == BOBO_SAVE_LOADED);
    assert(boot.generation == 43 && boot.state.collected == 511 && boot.state.owned_pets == 1);
    uint8_t bytes[BOBO_SAVE_SIZE];
    memcpy(bytes, legacy_ready, sizeof(bytes)); bytes[55] = 1; repair_crc(bytes);
    assert(!bobo_save_decode(bytes, sizeof(bytes), &s, &generation, &epoch)); // v1 reserved bytes stay strict.
}

static void test_v2_migration(void) {
    bobo_state_t state, roundtrip; uint32_t gen, epoch;
    assert(bobo_save_decode(legacy_v2_ready, BOBO_SAVE_SIZE, &state, &gen, &epoch));
    assert(state.active_pet == BOBO_PET_PANDA && state.owned_pets == BOBO_PET_MASK);
    assert(state.life_journeys == 3 && bobo_growth_stage(&state) == BOBO_JUNIOR);
    assert(state.phase == BOBO_READY && state.trip.card_id == 8 && state.collected == 255);
    assert(state.snacks == 8 && state.newpet_journeys[1] == 8 && !state.awake_s && !state.neglect_s);
    uint8_t bytes[BOBO_SAVE_SIZE];
    assert(bobo_save_encode(&state, gen, epoch, bytes));
    assert(!memcmp(bytes + 16, legacy_v2_ready + 16, 50)); // Every old field survives.
    assert(bobo_save_decode(bytes, BOBO_SAVE_SIZE, &roundtrip, &gen, &epoch));
    assert(!memcmp(&state, &roundtrip, sizeof(state)));
    assert(bobo_claim(&state) == BOBO_OK);
    assert(bobo_pet_select(&state, BOBO_PET_CACTUS) == BOBO_LOCKED);
    assert(bobo_pet_draw(&state, 0) == BOBO_BUSY);
    // A v2 pending gift still replays its committed species and prior stage.
    memcpy(bytes, legacy_v2_ready, sizeof(bytes));
    bytes[22] = BOBO_HOME; bytes[55] = BOBO_PET_LELE; bytes[57] = 1; repair_crc(bytes);
    assert(bobo_save_decode(bytes, BOBO_SAVE_SIZE, &state, &gen, &epoch));
    assert(state.pending_reveal && state.active_pet == BOBO_PET_LELE && state.life_journeys == 8);
    bytes[66] = 1; repair_crc(bytes);
    assert(!bobo_save_decode(bytes, BOBO_SAVE_SIZE, &state, &gen, &epoch));
}
static void test_life_save_and_generation_wrap(void) {
    fake_store_t f = {0}; hal_kv_t kv = {.api = &api, .impl = &f};
    bobo_state_t state; bobo_init(&state); state.owned_pets = 1;
    bobo_advance(&state, BOBO_ILL_AFTER_S + 12); state.life_journeys = 3;
    assert(bobo_save_encode(&state, UINT32_MAX, 1700000000, f.bytes[0])); f.sizes[0] = BOBO_SAVE_SIZE;
    bobo_journal_t j, boot;
    assert(bobo_journal_open(&j, &kv) == BOBO_SAVE_LOADED);
    assert(j.state.health == BOBO_ILL && j.state.neglect_s == BOBO_ILL_AFTER_S + 12);
    bobo_advance(&state, UINT32_MAX);
    f.fail = true; assert(bobo_journal_commit(&j, &state, 0) < 0 && j.state.health == BOBO_ILL);
    f.fail = false; assert(bobo_journal_commit(&j, &state, 0) == 0 && j.generation == 0);
    assert(bobo_journal_open(&boot, &kv) == BOBO_SAVE_LOADED);
    assert(boot.generation == 0 && boot.state.health == BOBO_DEAD && !boot.epoch);
    assert(!memcmp(&state, &boot.state, sizeof(state)));
}

static void test_pet_format_validation(void) {
    bobo_state_t s, decoded;
    bobo_init(&s); s.owned_pets = 15; s.active_pet = BOBO_PET_PANDA; s.pending_reveal = 1;
    s.newpet_journeys[0] = 3; s.newpet_journeys[1] = 8; s.draw_sequence = 4;
    uint8_t bytes[BOBO_SAVE_SIZE], damaged[BOBO_SAVE_SIZE]; uint32_t generation, epoch;
    assert(bobo_save_encode(&s, 100, 1700000000, bytes) && bytes[4] == 3 && bytes[5] == 0);
    assert(bobo_save_decode(bytes, sizeof(bytes), &decoded, &generation, &epoch));
    assert(memcmp(&s, &decoded, sizeof(s)) == 0 && generation == 100 && epoch == 1700000000);
    static const struct { unsigned offset; uint8_t value; } invalid[] = {
        {4, 4}, {55, BOBO_PET_COUNT}, {56, 63}, {56, 7}, {57, 2}, {58, 9}, {59, 9}, {60, 9}, {61, 9}, {74, 0x80}, {75, 1}
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        memcpy(damaged, bytes, sizeof(damaged)); damaged[invalid[i].offset] = invalid[i].value; repair_crc(damaged);
        decoded = s; generation = 321; epoch = 654;
        assert(!bobo_save_decode(damaged, sizeof(damaged), &decoded, &generation, &epoch));
        assert(memcmp(&decoded, &s, sizeof(s)) == 0 && generation == 321 && epoch == 654);
    }
    bobo_init(&s); s.owned_pets = 8; s.active_pet = BOBO_PET_PANDA;
    assert(bobo_save_encode(&s, 100, 0, bytes));
    bytes[58] = 1; repair_crc(bytes);
    assert(!bobo_save_decode(bytes, sizeof(bytes), &decoded, &generation, &epoch)); // No journeys for unowned pets.
    bobo_init(&s); assert(bobo_save_encode(&s, 100, 0, bytes));
    bytes[57] = 1; repair_crc(bytes);
    assert(!bobo_save_decode(bytes, sizeof(bytes), &decoded, &generation, &epoch)); // No ownerless pending gift.
}

static void test_pending_gift_journal(void) {
    fake_store_t f = {0}; hal_kv_t kv = {.api = &api, .impl = &f};
    bobo_journal_t j, boot;
    assert(bobo_journal_open(&j, &kv) == BOBO_SAVE_NEW);
    bobo_state_t candidate = j.state;
    assert(bobo_pet_draw(&candidate, UINT32_MAX) == BOBO_OK);
    f.fail = true;
    assert(bobo_journal_commit(&j, &candidate, 0) < 0);
    assert(!j.state.owned_pets && !j.state.pending_reveal && !j.state.draw_sequence);
    assert(bobo_journal_open(&boot, &kv) == BOBO_SAVE_NEW);
    f.fail = false;
    assert(bobo_journal_commit(&j, &candidate, 0) == 0);
    assert(bobo_journal_open(&boot, &kv) == BOBO_SAVE_LOADED);
    assert(boot.state.active_pet == BOBO_PET_LELE && boot.state.owned_pets == 16);
    assert(boot.state.pending_reveal && boot.state.draw_sequence == 1);
    candidate = boot.state;
    assert(bobo_pet_draw(&candidate, 0) == BOBO_BUSY);
    assert(memcmp(&candidate, &boot.state, sizeof(candidate)) == 0);
    assert(bobo_pet_reveal_ack(&candidate) == BOBO_OK);
    f.fail = true;
    assert(bobo_journal_commit(&j, &candidate, 0) < 0 && j.state.pending_reveal);
    assert(bobo_journal_open(&boot, &kv) == BOBO_SAVE_LOADED && boot.state.pending_reveal);
    f.fail = false;
    assert(bobo_journal_commit(&j, &candidate, 0) == 0);
    assert(bobo_journal_open(&boot, &kv) == BOBO_SAVE_LOADED && !boot.state.pending_reveal);

    candidate = j.state;
    bobo_advance(&candidate, BOBO_DEATH_AFTER_S);
    assert(bobo_pet_draw(&candidate, 0) == BOBO_OK && candidate.active_pet != BOBO_PET_LELE);
    unsigned selected = candidate.active_pet;
    f.fail_read_after_write = true;
    assert(bobo_journal_commit(&j, &candidate, 0) < 0 && j.read_only);
    assert(j.state.active_pet == BOBO_PET_LELE && j.state.draw_sequence == 1);
    assert(bobo_journal_commit(&j, &candidate, 0) < 0);
    f.read_errors = 0; f.fail_read_after_write = false;
    assert(bobo_journal_open(&boot, &kv) == BOBO_SAVE_LOADED);
    assert(boot.state.active_pet == selected && boot.state.pending_reveal && boot.state.draw_sequence == 2);
    candidate = boot.state; assert(bobo_pet_reveal_ack(&candidate) == BOBO_OK);
    f.partial_write = true;
    assert(bobo_journal_commit(&boot, &candidate, 0) < 0);
    assert(bobo_journal_open(&j, &kv) == BOBO_SAVE_RECOVERED);
    assert(j.state.pending_reveal && j.state.active_pet == selected && j.state.draw_sequence == 2);
    f.partial_write = false; f.write_then_fail = true;
    assert(bobo_journal_commit(&j, &candidate, 0) == 0 && !j.state.pending_reveal);
}

static void test_claim_growth_transaction(void) {
    fake_store_t f = {0}; hal_kv_t kv = {.api = &api, .impl = &f};
    bobo_journal_t j, boot;
    assert(bobo_journal_open(&j, &kv) == BOBO_SAVE_NEW);
    bobo_state_t candidate = j.state;
    assert(bobo_pet_draw(&candidate, UINT32_MAX) == BOBO_OK);
    assert(bobo_pet_reveal_ack(&candidate) == BOBO_OK);
    assert(bobo_start(&candidate, 0) == BOBO_OK);
    bobo_advance(&candidate, candidate.trip.remaining_s);
    assert(bobo_journal_commit(&j, &candidate, 0) == 0);
    assert(bobo_claim(&candidate) == BOBO_OK && candidate.life_journeys == 1);
    f.fail = true;
    assert(bobo_journal_commit(&j, &candidate, 0) < 0 && j.state.life_journeys == 0);
    assert(bobo_journal_open(&boot, &kv) == BOBO_SAVE_LOADED && boot.state.phase == BOBO_READY);
    candidate = boot.state; assert(bobo_claim(&candidate) == BOBO_OK);
    f.fail = false;
    assert(bobo_journal_commit(&boot, &candidate, 0) == 0);
    assert(bobo_journal_open(&j, &kv) == BOBO_SAVE_LOADED && j.state.life_journeys == 1);
    candidate = j.state;
    assert(bobo_claim(&candidate) == BOBO_NOT_READY);
    assert(memcmp(&candidate, &j.state, sizeof(candidate)) == 0);
}

int main(void) {
    test_legacy_migration(); test_v2_migration(); test_life_save_and_generation_wrap();
    test_pet_format_validation();
    test_pending_gift_journal();
    test_claim_growth_transaction();
    fake_store_t f = {0}; hal_kv_t kv = {.api = &api, .impl = &f};
    bobo_journal_t j, boot;
    assert(bobo_journal_open(&j, &kv) == BOBO_SAVE_NEW);
    bobo_state_t candidate = j.state; candidate.owned_pets = 1;
    assert(bobo_start(&candidate, 0) == BOBO_OK);
    assert(bobo_journal_commit(&j, &candidate, 1700000000) == 0);
    assert(j.generation == 1 && j.state.phase == BOBO_TRAVELLING);
    assert(bobo_journal_open(&boot, &kv) == BOBO_SAVE_LOADED);
    assert(boot.state.trip.remaining_s == 180 && boot.epoch == 1700000000);
    candidate = j.state; bobo_advance(&candidate, 180);
    assert(bobo_journal_commit(&j, &candidate, 1700000180) == 0);
    assert(bobo_journal_open(&boot, &kv) == BOBO_SAVE_LOADED && boot.state.phase == BOBO_READY);

    candidate = j.state; assert(bobo_claim(&candidate) == BOBO_OK);
    f.fail = true;
    assert(bobo_journal_commit(&j, &candidate, 1700000180) < 0);
    assert(j.state.phase == BOBO_READY && j.state.collected == 0 && j.state.snacks == 2);
    assert(bobo_journal_open(&boot, &kv) == BOBO_SAVE_LOADED && boot.state.phase == BOBO_READY);
    f.fail = false;
    assert(bobo_journal_commit(&j, &candidate, 1700000180) == 0);
    assert(bobo_journal_open(&boot, &kv) == BOBO_SAVE_LOADED && boot.state.collected == 1);
    assert(bobo_claim(&boot.state) == BOBO_NOT_READY && boot.state.snacks == 4);
    f.read_errors = 3;
    assert(bobo_journal_open(&boot, &kv) == BOBO_SAVE_UNAVAILABLE);
    assert(bobo_journal_commit(&boot, &candidate, 1700000180) < 0 && boot.read_only);
    f.read_errors = 1u << j.slot;
    assert(bobo_journal_open(&boot, &kv) == BOBO_SAVE_UNAVAILABLE);
    f.read_errors = 0;
    assert(bobo_journal_open(&boot, &kv) == BOBO_SAVE_LOADED && boot.state.collected == 1);
    f.sizes[j.slot] = BOBO_SAVE_SIZE + 1;
    assert(bobo_journal_open(&boot, &kv) == BOBO_SAVE_RECOVERED && boot.state.phase == BOBO_READY);
    f.sizes[j.slot] = BOBO_SAVE_SIZE;
    // Damage newest slot: previous complete generation is still usable.
    f.bytes[j.slot][30] ^= 1;
    assert(bobo_journal_open(&boot, &kv) == BOBO_SAVE_RECOVERED && boot.state.phase == BOBO_READY);
    // Every payload/header byte is checked, including reserved bytes and CRC.
    uint8_t encoded[BOBO_SAVE_SIZE]; bobo_state_t decoded; uint32_t gen, epoch;
    assert(bobo_save_encode(&j.state, UINT32_MAX, 1700000000, encoded));
    for (unsigned i = 0; i < BOBO_SAVE_SIZE; ++i) {
        encoded[i] ^= 0x80;
        assert(!bobo_save_decode(encoded, sizeof(encoded), &decoded, &gen, &epoch));
        encoded[i] ^= 0x80;
    }
    assert(bobo_save_decode(encoded, sizeof(encoded), &decoded, &gen, &epoch));
    assert(gen == UINT32_MAX && decoded.collected == 1);
    assert(!bobo_save_decode(encoded, sizeof(encoded)-1, &decoded, &gen, &epoch));
    assert(!bobo_offline_elapsed(0, 1700000000));
    assert(!bobo_offline_elapsed(1700000000, 0));
    assert(!bobo_offline_elapsed(1700000000, 1699999999));
    assert(bobo_offline_elapsed(1700000000, 1700000300) == 0);
    assert(!bobo_offline_elapsed(1700000000, 1800000000));
    memset(&f, 0, sizeof(f));
    assert(bobo_journal_open(&j, &kv) == BOBO_SAVE_NEW);
    candidate = j.state; candidate.owned_pets = 1; assert(bobo_start(&candidate, 0) == BOBO_OK);
    f.write_then_fail = true;
    assert(bobo_journal_commit(&j, &candidate, 1700000000) == 0);
    assert(j.state.phase == BOBO_TRAVELLING && !j.read_only);
    f.write_then_fail = false; f.partial_write = true;
    candidate = j.state; bobo_advance(&candidate, 180);
    assert(bobo_journal_commit(&j, &candidate, 1700000180) < 0);
    assert(j.state.phase == BOBO_TRAVELLING && !j.read_only);
    assert(bobo_journal_open(&boot, &kv) == BOBO_SAVE_RECOVERED && boot.state.phase == BOBO_TRAVELLING);
    f.partial_write = false; f.fail_read_after_write = true;
    assert(bobo_journal_commit(&j, &candidate, 1700000180) < 0 && j.read_only);
    assert(bobo_journal_commit(&j, &candidate, 1700000180) < 0);
    f.read_errors = 0; f.fail_read_after_write = false;
    assert(bobo_journal_open(&boot, &kv) == BOBO_SAVE_LOADED && boot.state.phase == BOBO_READY);
    for (unsigned cards = 0; cards <= BOBO_CARD_COUNT; ++cards) {
        bobo_init(&candidate); candidate.collected = (1u << cards) - 1;
        assert(bobo_save_encode(&candidate, 1, 1700000000, encoded));
        assert(bobo_save_decode(encoded, sizeof(encoded), &decoded, &gen, &epoch));
        assert(bobo_growth_stage(&candidate) == bobo_growth_stage(&decoded));
        assert(bobo_growth_remaining(&candidate) == bobo_growth_remaining(&decoded));
    }
    puts("bobo save: failed write, restart, corruption, atomic rewards and clock boundaries PASS");
    return 0;
}
