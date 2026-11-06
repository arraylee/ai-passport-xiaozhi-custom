#include "services/bobo_save.h"
#include <string.h>

static const char *const keys[] = {"bobo_a", "bobo_b"};
static void put16(uint8_t **p, uint16_t n) { *(*p)++ = n; *(*p)++ = n >> 8; }
static void put32(uint8_t **p, uint32_t n) { put16(p, n); put16(p, n >> 16); }
static uint16_t get16(const uint8_t **p) {
    uint16_t n = (*p)[0] | ((uint16_t)(*p)[1] << 8); *p += 2; return n;
}
static uint32_t get32(const uint8_t **p) {
    uint32_t lo = get16(p), hi = get16(p); return lo | (hi << 16);
}
static uint32_t crc32(const uint8_t *p, size_t size) {
    uint32_t crc = UINT32_MAX;
    while (size--) {
        crc ^= *p++;
        for (unsigned j = 0; j < 8; ++j) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
bool bobo_save_encode(const bobo_state_t *s, uint32_t generation, uint32_t epoch,
                      uint8_t out[BOBO_SAVE_SIZE]) {
    if (!out || !bobo_valid(s)) return false;
    memset(out, 0, BOBO_SAVE_SIZE);
    uint8_t *p = out;
    memcpy(p, "BOBO", 4); p += 4;
    put16(&p, BOBO_SAVE_VERSION); put16(&p, BOBO_SAVE_SIZE); put32(&p, generation); put32(&p, epoch);
    *p++ = s->energy; *p++ = s->mood; *p++ = s->tidy; *p++ = s->snacks;
    *p++ = s->tool; *p++ = s->target_card; *p++ = s->phase; *p++ = s->intro_used;
    put16(&p, s->collected);
    for (unsigned i = 0; i < 4; ++i) put16(&p, s->cooldown_s[i]);
    put16(&p, s->recovery_s); put32(&p, s->trip_sequence);
    *p++ = s->trip.destination; *p++ = s->trip.card_id; *p++ = s->trip.snacks;
    put16(&p, s->trip.duration_s); put16(&p, s->trip.remaining_s);
    *p++ = s->receipt.card_id; *p++ = s->receipt.snacks;
    *p++ = s->receipt.energy; *p++ = s->receipt.mood; put32(&p, s->receipt.sequence);
    *p++ = s->active_pet; *p++ = s->owned_pets; *p++ = s->pending_reveal;
    for (unsigned i = 0; i < BOBO_PET_COUNT - 1; ++i) *p++ = s->newpet_journeys[i];
    put32(&p, s->draw_sequence);
    put32(&p, s->awake_s); put32(&p, s->neglect_s);
    *p++ = s->life_journeys | (s->health << 4); // byte 75 remains reserved zero.
    p = out + BOBO_SAVE_SIZE - 4;
    put32(&p, crc32(out, BOBO_SAVE_SIZE - 4));
    return true;
}
bool bobo_save_decode(const uint8_t *data, size_t size, bobo_state_t *s,
                      uint32_t *generation, uint32_t *epoch) {
    if (!data || size != BOBO_SAVE_SIZE || !s || !generation || !epoch || memcmp(data, "BOBO", 4)) return false;
    const uint8_t *p = data + BOBO_SAVE_SIZE - 4;
    if (get32(&p) != crc32(data, BOBO_SAVE_SIZE - 4)) return false;
    p = data + 4;
    uint16_t version = get16(&p);
    if ((version < 1 || version > BOBO_SAVE_VERSION) || get16(&p) != BOBO_SAVE_SIZE) return false;
    uint32_t gen = get32(&p), stamp = get32(&p);
    bobo_state_t decoded; memset(&decoded, 0, sizeof(decoded));
    decoded.energy = *p++; decoded.mood = *p++; decoded.tidy = *p++; decoded.snacks = *p++;
    decoded.tool = *p++; decoded.target_card = *p++; decoded.phase = *p++; decoded.intro_used = *p++;
    decoded.collected = get16(&p);
    for (unsigned i = 0; i < 4; ++i) decoded.cooldown_s[i] = get16(&p);
    decoded.recovery_s = get16(&p); decoded.trip_sequence = get32(&p);
    decoded.trip.destination = *p++; decoded.trip.card_id = *p++; decoded.trip.snacks = *p++;
    decoded.trip.duration_s = get16(&p); decoded.trip.remaining_s = get16(&p);
    decoded.receipt.card_id = *p++; decoded.receipt.snacks = *p++;
    decoded.receipt.energy = *p++; decoded.receipt.mood = *p++; decoded.receipt.sequence = get32(&p);
    if (version == 1) {
        decoded.owned_pets = 1u << BOBO_PET_BOBO;
    } else {
        decoded.active_pet = *p++; decoded.owned_pets = *p++; decoded.pending_reveal = *p++;
        for (unsigned i = 0; i < BOBO_PET_COUNT - 1; ++i) decoded.newpet_journeys[i] = *p++;
        decoded.draw_sequence = get32(&p);
    }
    if (version == BOBO_SAVE_VERSION) {
        decoded.awake_s = get32(&p); decoded.neglect_s = get32(&p);
        uint8_t life = *p++;
        if (life & 0xc0) return false;
        decoded.life_journeys = life & 0xf; decoded.health = life >> 4;
    } else if (decoded.owned_pets) {
        unsigned count = decoded.active_pet == BOBO_PET_BOBO ? bobo_count(&decoded) :
            decoded.active_pet < BOBO_PET_COUNT ? decoded.newpet_journeys[decoded.active_pet - 1] : 0;
        decoded.life_journeys = count > 8 ? 8 : count;
    }
    while (p < data + BOBO_SAVE_SIZE - 4) if (*p++ != 0) return false;
    if (!bobo_valid(&decoded)) return false;
    *s = decoded; *generation = gen; *epoch = stamp;
    return true;
}
bobo_load_result_t bobo_journal_open(bobo_journal_t *j, hal_kv_t *kv) {
    memset(j, 0, sizeof(*j)); j->kv = kv; j->slot = 1; bobo_init(&j->state);
    if (!kv) { j->read_only = true; return BOBO_SAVE_UNAVAILABLE; }
    bool found = false, corrupt = false, io_error = false;
    for (unsigned i = 0; i < 2; ++i) {
        uint8_t bytes[BOBO_SAVE_SIZE]; int length = 0;
        int result = hal_kv_get(kv, keys[i], bytes, sizeof(bytes), &length);
        if (result == HAL_KV_NOT_FOUND) continue;
        if (result == HAL_KV_TOO_LARGE) { corrupt = true; continue; }
        if (result != 0) { io_error = true; continue; }
        bobo_state_t state; uint32_t generation, epoch;
        if (!bobo_save_decode(bytes, (size_t)length, &state, &generation, &epoch)) { corrupt = true; continue; }
        // Unsigned serial arithmetic handles generation wrap without implementation-defined casts.
        uint32_t delta = generation - j->generation;
        if (!found || (delta != 0 && delta < 0x80000000u)) {
            j->state = state; j->generation = generation; j->epoch = epoch; j->slot = i;
        }
        found = true;
    }
    if (io_error) { j->read_only = true; return BOBO_SAVE_UNAVAILABLE; }
    if (!found) return corrupt ? BOBO_SAVE_RESET : BOBO_SAVE_NEW;
    return corrupt ? BOBO_SAVE_RECOVERED : BOBO_SAVE_LOADED;
}
int bobo_journal_commit(bobo_journal_t *j, const bobo_state_t *candidate, uint32_t epoch) {
    uint8_t bytes[BOBO_SAVE_SIZE];
    if (!j || !j->kv || j->read_only || !bobo_save_encode(candidate, j->generation + 1, epoch, bytes)) return -1;
    unsigned next_slot = 1 - j->slot;
    if (hal_kv_set(j->kv, keys[next_slot], bytes, sizeof(bytes)) != 0) {
        uint8_t actual[BOBO_SAVE_SIZE]; int length = 0;
        int result = hal_kv_get(j->kv, keys[next_slot], actual, sizeof(actual), &length);
        if (result == HAL_KV_ERROR || (result < 0 && result != HAL_KV_NOT_FOUND && result != HAL_KV_TOO_LARGE)) {
            j->read_only = true;
            return -1;
        }
        if (result != 0 || length != BOBO_SAVE_SIZE || memcmp(actual, bytes, sizeof(bytes))) return -1;
    }
    j->state = *candidate; j->epoch = epoch; ++j->generation; j->slot = next_slot;
    return 0;
}
uint32_t bobo_offline_elapsed(uint32_t saved_epoch, uint32_t trusted_epoch) {
    (void)saved_epoch; (void)trusted_epoch;
    return 0;
}
