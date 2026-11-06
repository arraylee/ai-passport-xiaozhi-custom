#include "xiaozhi.h"
#include "xz_capture.h"
#include "esp_private/esp_clk.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>

typedef struct {
    xz_dsp_stats_t *out;
    SemaphoreHandle_t done;
    bool ok;
} dsp_job_t;

static bool dsp_measure(xz_capture_t *capture, unsigned rate,
                        unsigned *frames, unsigned *avg_us, unsigned *peak_us) {
    if (!xz_capture_begin(capture, rate)) return false;
    int16_t history[128] = {0}, stereo[192 * 2];
    unsigned cursor = 0, factor = rate / 8000;
    uint32_t random = 24;
    uint64_t total_us = 0;
    *frames = *avg_us = *peak_us = 0;
    for (unsigned block = 0; block < 64; ++block) {
        for (unsigned i = 0; i < 64; ++i) {
            random = random * 1664525u + 1013904223u;
            int16_t ref = (int16_t)(((int32_t)(random >> 16) - 32768) / 4);
            history[cursor & 127] = ref;
            int16_t mic = history[(cursor - 40) & 127] / 2 +
                          history[(cursor - 90) & 127] / 4;
            ++cursor;
            for (unsigned j = 0; j < factor; ++j) {
                stereo[(i * factor + j) * 2] = mic;
                stereo[(i * factor + j) * 2 + 1] = ref;
            }
        }
        int64_t start = esp_timer_get_time();
        bool ok = xz_capture_feed(capture, stereo, 64 * factor);
        unsigned elapsed = (unsigned)(esp_timer_get_time() - start);
        if (!ok || xz_capture_triggered(capture)) return false;
        total_us += elapsed;
        if (elapsed > *peak_us) *peak_us = elapsed;
        *avg_us = (unsigned)(total_us / ++*frames);
        vTaskDelay(1); // Let idle/UI run; delay is excluded from processing time.
    }
    memset(stereo, 0, sizeof stereo);
    return true;
}

static void dsp_task(void *arg) {
    dsp_job_t *job = arg;
    xz_capture_t *capture = xz_capture_create();
    if (capture) {
        job->out->cpu_mhz = esp_clk_cpu_freq() / 1000000;
        job->ok = dsp_measure(capture, 16000, &job->out->frames16,
                             &job->out->avg16_us, &job->out->peak16_us) &&
                  dsp_measure(capture, 24000, &job->out->frames24,
                             &job->out->avg24_us, &job->out->peak24_us);
        xz_capture_destroy(capture);
    }
    job->out->stack_free = (unsigned)uxTaskGetStackHighWaterMark(NULL);
    xSemaphoreGive(job->done);
    // Caller reclaims this stack immediately, before releasing UI exclusion.
    for (;;) vTaskSuspend(NULL);
}

bool xiaozhi_dsp_check(xz_dsp_stats_t *out) {
    if (!out || !xiaozhi_stopped()) return false;
    memset(out, 0, sizeof *out);
    dsp_job_t job = {.out = out, .done = xSemaphoreCreateBinary()};
    if (!job.done) return false;
    TaskHandle_t task = NULL;
    if (xTaskCreate(dsp_task, "dsp_check", 6144, &job, 6, &task) == pdPASS) {
        xSemaphoreTake(job.done, portMAX_DELAY);
        vTaskDelete(task);
    }
    vSemaphoreDelete(job.done);
    return job.ok;
}
