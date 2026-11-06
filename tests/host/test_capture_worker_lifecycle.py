"""Run the real capture worker against a deterministic queue/task scheduler.

Exercise the enqueue-versus-exit handoff and failure/retry without BLE hardware.
The screenshot protocol and renderer keep their independent existing tests.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_ble_advertising import function

ROOT = Path(__file__).resolve().parents[2]
BOUNDARY = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
#include "services/screen_capture.h"
#define CAPTURE_TASK_STACK_BYTES 6144u
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define CFG_MSG_SCREEN_CAPTURE 9
#define CFG_ST_DONE 0
#define CFG_ST_ERR_SEQ 1
typedef struct { uint8_t op; uint16_t capture_id; uint32_t session; } capture_cmd_t;
static void *s_capture_cmd_q=(void*)1, *s_capture_worker_lock=(void*)2;
static bool s_capture_worker_running;
static atomic_uint_fast32_t s_capture_session=1, s_capture_result_deadline_ms;
static atomic_uint_fast16_t s_capture_result_id;
static capture_cmd_t queue[4];
static unsigned count, created, deleted, starts, cleared_pending, done, errors, now_ms;
static bool locked, fail_create, in_worker, enqueue_at_exit;
static void (*task_entry)(void*);
static bool capture_queue_command(uint8_t op, uint16_t capture_id);
static uint32_t capture_now_ms(void) { return now_ms; }
static int xSemaphoreTake(void *lock, unsigned wait) {
    assert(lock==s_capture_worker_lock);
    assert(wait==0 || wait==portMAX_DELAY);
    if(locked) { assert(wait==0); return 0; }
    locked=true; return pdTRUE;
}
static void xSemaphoreGive(void *lock) {
    assert(lock==s_capture_worker_lock && locked); locked=false;
    if(in_worker && !s_capture_worker_running && enqueue_at_exit) {
        enqueue_at_exit=false;
        assert(capture_queue_command(SC_OP_START,0));
    }
}
static int xQueueSend(void *q,const capture_cmd_t *cmd,int wait) {
    assert(q==s_capture_cmd_q && locked && wait==0);
    if(count==4) return 0;
    queue[count++]=*cmd; return pdTRUE;
}
static int xQueueReceive(void *q,capture_cmd_t *cmd,int wait) {
    assert(q==s_capture_cmd_q && locked && wait==0);
    if(!count) return 0;
    *cmd=queue[0]; --count;
    for(unsigned i=0;i<count;i++) queue[i]=queue[i+1];
    return pdTRUE;
}
static void xQueueReset(void *q) { assert(q==s_capture_cmd_q && locked); count=0; }
static int xTaskCreate(void (*fn)(void*),const char *name,unsigned stack,void *arg,int priority,void *handle) {
    (void)name; assert(locked && s_capture_worker_running && stack==6144 && !arg && priority==5 && !handle);
    if(fail_create) return 0;
    task_entry=fn; ++created; return pdPASS;
}
static void vTaskDelete(void *handle) { assert(!handle && !locked); ++deleted; }
static void capture_clear_start_pending(void) { ++cleared_pending; }
static void capture_run_start(uint32_t session) {
    assert(session==s_capture_session && !locked); ++starts;
}
static void notify_status(int type,int status) {
    assert(type==CFG_MSG_SCREEN_CAPTURE && !locked);
    if(status==CFG_ST_DONE) ++done; else { assert(status==CFG_ST_ERR_SEQ); ++errors; }
}
static void run_worker(void) { assert(task_entry); in_worker=true; task_entry(NULL); in_worker=false; }
'''
CASES = r'''
int main(void) {
    // First request allocates one worker; a burst reuses it. Full queue is busy.
    assert(capture_queue_command(SC_OP_START,0) && created==1);
    for(int i=0;i<3;i++) assert(capture_queue_command(SC_OP_START,0));
    assert(!capture_queue_command(SC_OP_START,0) && created==1);
    run_worker(); assert(starts==4 && deleted==1 && !count && !s_capture_worker_running);

    // Host callback must never block behind the worker's short handoff lock.
    locked=true; assert(!capture_queue_command(SC_OP_START,0) && !count); locked=false;

    // Allocation failure leaves neither a queued request nor a running flag.
    fail_create=true; assert(!capture_queue_command(SC_OP_START,0));
    assert(!count && !s_capture_worker_running && created==deleted);
    fail_create=false; assert(capture_queue_command(SC_OP_START,0));
    run_worker(); assert(starts==5 && created==deleted);

    // Disconnect invalidates commands already queued by the previous client.
    assert(capture_queue_command(SC_OP_START,0));
    assert(capture_queue_command(SC_OP_FINISH,22)); ++s_capture_session;
    run_worker(); assert(starts==5 && cleared_pending==1 && !done && !errors);

    // A receipt survives worker deletion and is consumed by a fresh worker.
    s_capture_result_id=22; s_capture_result_deadline_ms=300000; now_ms=1000;
    assert(capture_queue_command(SC_OP_FINISH,22)); run_worker();
    assert(done==1 && !s_capture_result_id && created==deleted);
    s_capture_result_id=23; s_capture_result_deadline_ms=1001; now_ms=1001;
    assert(capture_queue_command(SC_OP_CANCEL,23)); run_worker();
    assert(errors==1 && !s_capture_result_id && created==deleted);
    // Deadline wraps around the 32-bit millisecond clock.
    s_capture_result_id=24; s_capture_result_deadline_ms=20; now_ms=0xfffffff0u;
    assert(capture_result_matches(24)); now_ms=20; assert(!capture_result_matches(24));

    // Enqueue immediately after an empty worker unlocks but before it deletes:
    // the accepted request must acquire a successor, never a stranded queue.
    unsigned before=created;
    assert(capture_queue_command(SC_OP_START,0)); enqueue_at_exit=true;
    run_worker(); assert(created==before+2 && deleted==created-1);
    assert(count==1 && s_capture_worker_running);
    run_worker(); assert(starts==7 && !count && !s_capture_worker_running && created==deleted);
    return 0;
}
'''


class CaptureWorkerLifecycle(unittest.TestCase):
    def test_worker_releases_stack_and_handles_exit_races_and_failures(self):
        app = (ROOT / "components/app/src/app.c").read_text()
        names = ("capture_deadline_reached", "capture_clear_result", "capture_result_matches",
                 "capture_transfer_task", "capture_queue_command")
        with tempfile.TemporaryDirectory() as directory:
            source, executable = Path(directory) / "test.c", Path(directory) / "test"
            source.write_text(BOUNDARY + "\n".join(function(app, name) for name in names) + CASES)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined", "-fno-sanitize-recover=all",
                            "-I" + str(ROOT / "components/core/services/include"),
                            str(source), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_boot_does_not_allocate_an_idle_capture_stack(self):
        app = (ROOT / "components/app/src/app.c").read_text()
        self.assertNotIn("xTaskCreate", function(app, "capture_orchestration_init"))


if __name__ == "__main__":
    unittest.main()
