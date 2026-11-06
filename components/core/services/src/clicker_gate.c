#include "services/clicker_gate.h"

#include <string.h>

void clicker_gate_init(clicker_gate_t *gate)
{
    memset(gate, 0, sizeof *gate);
}

void clicker_gate_set_active(clicker_gate_t *gate, bool active)
{
    if (!active && gate->desired_active) {
        gate->desired_active = false;
        gate->exit_latched = true;
        gate->generation++;
        if (gate->input_pending) {
            gate->input_pending = false;
            gate->input_busy = false;
        }
    } else if (active) {
        gate->desired_active = true;
    }
}

bool clicker_gate_desired_active(const clicker_gate_t *gate)
{
    return gate->desired_active;
}

bool clicker_gate_enqueue(clicker_gate_t *gate, clicker_action_t action, bool ready)
{
    if (!ready || !gate->desired_active || gate->exit_latched || gate->input_busy ||
        action < CLICKER_PREV || action > CLICKER_POINTER_TEST) {
        return false;
    }
    gate->pending_action = action;
    gate->pending_generation = gate->generation;
    gate->input_pending = true;
    gate->input_busy = true;
    return true;
}

void clicker_gate_take_work(clicker_gate_t *gate, clicker_gate_work_t *work)
{
    memset(work, 0, sizeof *work);
    work->desired_active = gate->desired_active;
    if (gate->exit_latched) {
        work->begin_exit = true;
        gate->exit_latched = false;
        if (gate->input_pending) {
            gate->input_pending = false;
            gate->input_busy = false;
        }
        return;
    }
    if (!gate->input_pending) return;
    work->has_action = true;
    work->action = gate->pending_action;
    work->action_generation = gate->pending_generation;
    gate->input_pending = false;
}

bool clicker_gate_action_current(const clicker_gate_t *gate, uint32_t generation)
{
    return gate->desired_active && !gate->exit_latched &&
           generation == gate->generation;
}

void clicker_gate_input_done(clicker_gate_t *gate)
{
    gate->input_pending = false;
    gate->input_busy = false;
}

void clicker_gate_reset_io(clicker_gate_t *gate)
{
    gate->input_pending = false;
    gate->input_busy = false;
}

bool clicker_gate_boundary_complete(clicker_gate_t *gate)
{
    gate->exit_latched = false;
    clicker_gate_reset_io(gate);
    return gate->desired_active;
}

bool clicker_gate_bond_replacement_allowed(const clicker_gate_t *gate,
                                           bool host_active,
                                           bool exit_pending,
                                           bool shutting_down)
{
    return gate->desired_active && !gate->exit_latched && host_active &&
           !exit_pending && !shutting_down;
}

clicker_store_policy_t clicker_gate_store_policy(const clicker_gate_t *gate,
                                                 bool host_active,
                                                 bool exit_pending,
                                                 bool shutting_down,
                                                 bool overflow)
{
    if (clicker_gate_bond_replacement_allowed(
            gate, host_active, exit_pending, shutting_down)) {
        return overflow ? CLICKER_STORE_REPLACE : CLICKER_STORE_PROCEED;
    }
    return overflow ? CLICKER_STORE_REJECT : CLICKER_STORE_PROCEED;
}
