#pragma once
#include "services/bobo.h"
#include "hal/hal_kv.h"
#include <stddef.h>

#define BOBO_SAVE_SIZE 80
#define BOBO_SAVE_VERSION 3
typedef enum { BOBO_SAVE_NEW, BOBO_SAVE_LOADED, BOBO_SAVE_RECOVERED, BOBO_SAVE_RESET,
               BOBO_SAVE_UNAVAILABLE } bobo_load_result_t;
typedef struct {
    bobo_state_t state;
    hal_kv_t *kv;
    uint32_t generation, epoch;
    uint8_t slot;
    bool read_only;
} bobo_journal_t;

bool bobo_save_encode(const bobo_state_t *s, uint32_t generation, uint32_t epoch,
                      uint8_t out[BOBO_SAVE_SIZE]);
// v1 migration retains all original progress and grants the original Bobo companion.
// v2 migration retains the active companion and its stage; historical species stay hidden.
// v3 adds awake lifetime, neglect, per-life growth and health in the original 80 bytes.
bool bobo_save_decode(const uint8_t *data, size_t size, bobo_state_t *s,
                      uint32_t *generation, uint32_t *epoch);
bobo_load_result_t bobo_journal_open(bobo_journal_t *j, hal_kv_t *kv);
// Write failures are read back: exact committed bytes count as success; uncertain IO locks
// the journal until reopened. Callers must show retry when read_only is true.
int bobo_journal_commit(bobo_journal_t *j, const bobo_state_t *candidate, uint32_t epoch);
// Legacy ABI: wall-clock changes never advance an awake-only pet.
uint32_t bobo_offline_elapsed(uint32_t saved_epoch, uint32_t trusted_epoch);
