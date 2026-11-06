#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "services/clicker_gate.h"

static uint8_t sent_reports[4][8];
static unsigned sent_count;

static bool record_report(const uint8_t report[8], void *user)
{
    (void)user;
    assert(sent_count < 4);
    memcpy(sent_reports[sent_count++], report, 8);
    return true;
}

static void fast_exit_reenter_drops_old_action(void)
{
    clicker_gate_t gate;
    clicker_gate_work_t work;
    clicker_gate_init(&gate);
    clicker_gate_set_active(&gate, true);
    assert(clicker_gate_boundary_complete(&gate));
    assert(clicker_gate_enqueue(&gate, CLICKER_NEXT, true));

    clicker_gate_set_active(&gate, false);
    clicker_gate_set_active(&gate, true);
    clicker_gate_take_work(&gate, &work);

    assert(work.begin_exit);
    assert(!work.has_action);
    assert(work.desired_active);
    assert(clicker_gate_boundary_complete(&gate));
    clicker_gate_take_work(&gate, &work);
    assert(!work.begin_exit && !work.has_action);
}

static void taken_action_is_invalid_after_session_changes(void)
{
    clicker_gate_t gate;
    clicker_gate_work_t work;
    clicker_gate_init(&gate);
    clicker_gate_set_active(&gate, true);
    assert(clicker_gate_boundary_complete(&gate));
    assert(clicker_gate_enqueue(&gate, CLICKER_PREV, true));
    clicker_gate_take_work(&gate, &work);
    assert(work.has_action && clicker_gate_action_current(&gate, work.action_generation));

    clicker_gate_set_active(&gate, false);
    assert(!clicker_gate_action_current(&gate, work.action_generation));
}

static void held_key_is_released_before_reenter(void)
{
    clicker_gate_t gate;
    clicker_gate_work_t work;
    clicker_reports_t reports;
    uint8_t zero[8] = {0};
    memset(sent_reports, 0, sizeof sent_reports);
    sent_count = 0;
    clicker_gate_init(&gate);
    clicker_reports_init(&reports);
    clicker_gate_set_active(&gate, true);
    assert(clicker_gate_boundary_complete(&gate));

    assert(clicker_gate_enqueue(&gate, CLICKER_NEXT, true));
    clicker_gate_take_work(&gate, &work);
    assert(work.has_action && clicker_gate_action_current(&gate, work.action_generation));
    assert(clicker_reports_request(&reports, work.action, true));
    clicker_reports_tick(&reports, record_report, NULL);
    assert(sent_count == 1 && sent_reports[0][2] == 0x4e);

    clicker_gate_set_active(&gate, false);
    clicker_gate_set_active(&gate, true);
    clicker_gate_take_work(&gate, &work);
    assert(work.begin_exit && !work.has_action);
    clicker_reports_cancel(&reports);
    clicker_reports_tick(&reports, record_report, NULL);
    assert(sent_count == 2 && !memcmp(sent_reports[1], zero, 8));
    assert(clicker_gate_boundary_complete(&gate));
}

static void reset_sync_converges_both_directions(void)
{
    clicker_gate_t gate;
    clicker_gate_init(&gate);
    clicker_gate_set_active(&gate, true);
    assert(clicker_gate_boundary_complete(&gate));

    /* Host reset is the old session boundary. Work is unavailable until sync. */
    clicker_gate_reset_io(&gate);
    clicker_gate_set_active(&gate, false);
    assert(!clicker_gate_boundary_complete(&gate));

    clicker_gate_set_active(&gate, true);
    clicker_gate_set_active(&gate, false);
    clicker_gate_set_active(&gate, true);
    assert(clicker_gate_boundary_complete(&gate));
}

static void standard_mode_never_runs_round_robin_replacement(void)
{
    clicker_gate_t gate;
    clicker_gate_init(&gate);
    char stored_bond = 'A';
    assert(!clicker_gate_bond_replacement_allowed(&gate, false, false, false));
    assert(clicker_gate_store_policy(&gate, false, false, false, false) ==
           CLICKER_STORE_PROCEED);
    assert(clicker_gate_store_policy(&gate, false, false, false, true) ==
           CLICKER_STORE_REJECT);
    if (clicker_gate_store_policy(&gate, false, false, false, true) ==
        CLICKER_STORE_REPLACE) {
        stored_bond = 'B';
    }
    assert(stored_bond == 'A');

    clicker_gate_set_active(&gate, true);
    assert(!clicker_gate_bond_replacement_allowed(&gate, false, false, false));
    assert(clicker_gate_boundary_complete(&gate));
    assert(clicker_gate_bond_replacement_allowed(&gate, true, false, false));
    assert(clicker_gate_store_policy(&gate, true, false, false, true) ==
           CLICKER_STORE_REPLACE);

    clicker_gate_set_active(&gate, false);
    clicker_gate_set_active(&gate, true);
    assert(!clicker_gate_bond_replacement_allowed(&gate, true, false, false));
    clicker_gate_work_t work;
    clicker_gate_take_work(&gate, &work);
    assert(work.begin_exit);
    assert(!clicker_gate_bond_replacement_allowed(&gate, false, true, false));
    assert(clicker_gate_store_policy(&gate, false, true, false, true) ==
           CLICKER_STORE_REJECT);
    assert(!clicker_gate_bond_replacement_allowed(&gate, true, false, true));
}

int main(void)
{
    fast_exit_reenter_drops_old_action();
    taken_action_is_invalid_after_session_changes();
    held_key_is_released_before_reenter();
    reset_sync_converges_both_directions();
    standard_mode_never_runs_round_robin_replacement();
    puts("clicker_gate: generations, reset convergence and bond policy PASS");
}
