#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "services/clicker_reports.h"

/* Cross-task mode and input gate. The platform adapter supplies locking. */
typedef struct {
    bool desired_active;
    bool exit_latched;
    bool input_pending;
    bool input_busy;
    uint32_t generation;
    uint32_t pending_generation;
    clicker_action_t pending_action;
} clicker_gate_t;

typedef struct {
    bool begin_exit;
    bool desired_active;
    bool has_action;
    uint32_t action_generation;
    clicker_action_t action;
} clicker_gate_work_t;

typedef enum {
    CLICKER_STORE_PROCEED,
    CLICKER_STORE_REPLACE,
    CLICKER_STORE_REJECT,
} clicker_store_policy_t;

void clicker_gate_init(clicker_gate_t *gate);
void clicker_gate_set_active(clicker_gate_t *gate, bool active);
bool clicker_gate_desired_active(const clicker_gate_t *gate);
bool clicker_gate_enqueue(clicker_gate_t *gate, clicker_action_t action, bool ready);
void clicker_gate_take_work(clicker_gate_t *gate, clicker_gate_work_t *work);
bool clicker_gate_action_current(const clicker_gate_t *gate, uint32_t generation);
void clicker_gate_input_done(clicker_gate_t *gate);
void clicker_gate_reset_io(clicker_gate_t *gate);
bool clicker_gate_boundary_complete(clicker_gate_t *gate);
bool clicker_gate_bond_replacement_allowed(const clicker_gate_t *gate,
                                           bool host_active,
                                           bool exit_pending,
                                           bool shutting_down);
clicker_store_policy_t clicker_gate_store_policy(const clicker_gate_t *gate,
                                                 bool host_active,
                                                 bool exit_pending,
                                                 bool shutting_down,
                                                 bool overflow);
