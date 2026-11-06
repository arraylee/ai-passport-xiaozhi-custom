// Run the production worker and event producer against deterministic time/queue.
// UI/HW calls are boundary doubles; all navigation ownership logic and domain
// services are the real firmware sources. No FreeRTOS thread or device is used.
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../components/app/src/app_plays.c"

static bool fake_xz_running, fake_xz_fault;
static xz_phase_t fake_xz_phase = XZ_READY;
static unsigned xz_closes, voice_opened_calls, xz_key_count[4];
static bool forced_setup;
static unsigned setup_button_presses;
void xiaozhi_begin_setup(void){++setup_button_presses;}
void xiaozhi_welcome_shown(void){}
bool ui_xiaozhi_welcome_drawn(void){return true;}
bool xiaozhi_output_active(void){return false;}
static unsigned xz_stop_requests, xz_interrupts, volume_changes, mode_changes;
static ui_xz_action_t next_xz_action;
static unsigned setting_value, applied_setting;
static bool setting_saved=true, reported_saved;
static unsigned lock_depth(void);
bool platform_ble_voice_pause(void) { return true; }
bool platform_ble_voice_resume(void) { return true; }
void xiaozhi_init(const xz_config_t *c) { (void)c; }
bool xiaozhi_start(bool setup) { assert(!lock_depth());forced_setup=setup;fake_xz_running=true;return true; }
void xiaozhi_service(void) {}
void xiaozhi_stop(void) { if(fake_xz_running) ++xz_stop_requests; }
bool xiaozhi_stopped(void) { return !fake_xz_running; }
void xiaozhi_talk(void) {}
void xiaozhi_resume(void) {}
void xiaozhi_interrupt(void) { xz_interrupts++; }
bool xiaozhi_set_volume(unsigned v) { assert(!lock_depth());volume_changes++;applied_setting=v;return setting_saved; }
bool xiaozhi_set_natural_interrupt(bool v) { assert(!lock_depth());mode_changes++;applied_setting=v;return setting_saved; }
void ui_xiaozhi_setting_result(bool saved) { assert(lock_depth());reported_saved=saved; }
void xiaozhi_avatar(unsigned n) { (void)n; }
void xiaozhi_snapshot(xz_view_t *view) { memset(view,0,sizeof *view); view->restart_required=fake_xz_fault; view->phase=fake_xz_fault?XZ_ERROR:fake_xz_phase; }
const companion_portrait_t *companion_avatar(unsigned n) { (void)n; return NULL; }
void ui_xiaozhi_set_device_settings(unsigned brightness, unsigned sleep_minutes) { (void)brightness; (void)sleep_minutes; }
void ui_xiaozhi_open(const xz_view_t *v,const companion_portrait_t *p) { (void)v; (void)p; }
void ui_xiaozhi_close(void) { xz_closes++; }
void ui_xiaozhi_update(const xz_view_t *v,const companion_portrait_t *p) { (void)v; (void)p; }
ui_xz_action_t ui_xiaozhi_key(ui_play_key_t k,unsigned *n) { assert((unsigned)k<4);xz_key_count[k]++;*n=setting_value;return k == UI_PLAY_BACK ? UI_XZ_EXIT : k == UI_PLAY_OK ? next_xz_action : UI_XZ_NONE; }

struct fake_queue { unsigned capacity, item_size, count; unsigned char data[16 * sizeof(play_event_t)]; };
static uint64_t fake_ms;
static jmp_buf idle;
static void (*worker_task)(void *);
static ui_play_select_cb_t choose;
static ui_plays_exit_cb_t leave_plays;
static unsigned menu_keys, selections, home_exits;
static bool return_selected;
static unsigned empty_steps, lvgl_depth, random_calls, acquire_calls, release_calls, read_calls;
static unsigned lock_depth(void) { return lvgl_depth; }
static TickType_t last_queue_wait;
static int audio_amplitude;
static bool acquire_fails, read_fails;
static void (*during_read)(void), (*during_acquire)(void);
static bool guide_open;
static unsigned guide_opens, guide_closes, lock_failures;
static void (*during_lvgl_lock)(void);
static const char *identity_key_value = "PREVIEW_ONLY";
static bool identity_key_fails, identity_sn_fails;
static char guide_key[HAL_IDENTITY_KEY_CAP];

int64_t esp_timer_get_time(void) { return (int64_t)fake_ms * 1000; }
uint32_t esp_random(void) { random_calls++; return 17; }
QueueHandle_t xQueueCreate(unsigned capacity, unsigned item_size) {
    assert(capacity * item_size <= sizeof(((struct fake_queue *)0)->data));
    QueueHandle_t q = calloc(1, sizeof(*q)); assert(q);
    q->capacity = capacity; q->item_size = item_size; return q;
}
void vQueueDelete(QueueHandle_t q) { free(q); }
int xQueueSend(QueueHandle_t q, const void *item, TickType_t timeout) {
    (void)timeout;
    if (q->count == q->capacity) return pdFALSE;
    memcpy(q->data + q->count++ * q->item_size, item, q->item_size); return pdTRUE;
}
int xQueueReceive(QueueHandle_t q, void *item, TickType_t timeout) {
    last_queue_wait = timeout;
    if (!q->count) {
        if (!empty_steps) longjmp(idle, 1); // End this deterministic worker timeslice.
        empty_steps--;
        if (timeout) fake_ms += timeout;
        return pdFALSE;
    }
    memcpy(item, q->data, q->item_size);
    memmove(q->data, q->data + q->item_size, --q->count * q->item_size); return pdTRUE;
}
unsigned uxQueueMessagesWaiting(QueueHandle_t q) { return q->count; }
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                void *argument, unsigned priority, void *handle) {
    (void)name; (void)stack; (void)argument; (void)priority; (void)handle;
    worker_task = task; return pdPASS;
}
void vTaskDelay(TickType_t delay) { (void)delay; longjmp(idle, 1); }
bool platform_lvgl_lock(int timeout_ms) {
    (void)timeout_ms;
    if (lock_failures) { --lock_failures; return false; }
    lvgl_depth++;
    if (during_lvgl_lock) {
        void (*hook)(void) = during_lvgl_lock; during_lvgl_lock = NULL; hook();
    }
    return true;
}
void platform_lvgl_unlock(void) { assert(lvgl_depth); lvgl_depth--; }
static clicker_action_t recorded_clicker_action;
static unsigned clicker_sends, clicker_activations;
static bool clicker_transport_active;
static void (*before_clicker_disable)(void), (*after_clicker_disable)(void);
static ui_click_action_t next_clicker_action;
static ui_click_view_t recorded_clicker_view;
static platform_clicker_snapshot_t fake_clicker_snapshot;
bool platform_clicker_change_computer(void) { return true; }
void platform_clicker_set_active(bool enabled) {
    if (!enabled && before_clicker_disable) {
        void (*hook)(void) = before_clicker_disable; before_clicker_disable = NULL; hook();
    }
    clicker_transport_active = enabled;
    if (enabled) ++clicker_activations;
    if (!enabled && after_clicker_disable) {
        void (*hook)(void) = after_clicker_disable; after_clicker_disable = NULL; hook();
    }
}
bool platform_clicker_send(clicker_action_t action) { recorded_clicker_action = action; clicker_sends++; return true; }
void platform_clicker_snapshot(platform_clicker_snapshot_t *out) { *out = fake_clicker_snapshot; }
void ui_plays_open(ui_play_select_cb_t select, ui_plays_exit_cb_t exit, void *user) {
    (void)user; choose = select; leave_plays = exit;
}
void ui_plays_menu(void) { return_selected = false; }
void ui_plays_suspend(void) { assert(lvgl_depth); }
void ui_plays_close(void) { assert(lvgl_depth); leave_plays(NULL); }
void ui_button_guide_open_menu(const char *sn, const char *key) {
    assert(lvgl_depth && !guide_open);
    assert(!strcmp(sn, "PREVIEW01") && !strcmp(key, identity_key_value));
    assert(strlen(key) < sizeof guide_key); strcpy(guide_key, key);
    guide_open = true; ++guide_opens;
}
void ui_button_guide_close(void) {
    assert(lvgl_depth && guide_open); guide_open = false; ++guide_closes;
}
void ui_plays_menu_key(ui_play_key_t key) {
    menu_keys++;
    if (key == UI_PLAY_BACK || (key == UI_PLAY_OK && return_selected)) leave_plays(NULL);
    else if (key == UI_PLAY_OK) { selections++; assert(choose(UI_PLAY_ANSWER, NULL)); }
}
void ui_plays_answer(const ab_session_view_t *view, uint16_t level, uint64_t ms) {
    (void)view; (void)level; (void)ms;
}
void ui_plays_clicker(const ui_click_view_t *view) { recorded_clicker_view = *view; }
ui_click_action_t ui_plays_clicker_key(ui_play_key_t key) {
    return key == UI_PLAY_BACK ? UI_CLICK_ACTION_EXIT : next_clicker_action;
}
void ui_plays_clicker_feedback(const char *action, bool queued) { (void)action; (void)queued; }
void ui_game_set_best(int best) { (void)best; }
void ui_game_open(ui_game_exit_cb_t exit, void *user) { (void)exit; (void)user; }
void ui_game_set_result_cb(ui_game_result_cb_t cb, void *user) { (void)cb; (void)user; }
void ui_game_key(ui_game_key_t key) { (void)key; }
void ui_game_pause_for_sleep(void) {}
static void home(void *user) { (void)user; home_exits++; }
static void pump(void) { if (!setjmp(idle)) worker_task(NULL); }
static void steps(unsigned count) { empty_steps = count; pump(); }
static void key(uint64_t at_ms, int index, hal_btn_event_t event) {
    fake_ms = at_ms; app_plays_button(index, event); pump();
}
static void check(bool condition, const char *failure) {
    if (!condition) { fprintf(stderr, "FAIL at %llu ms: %s\n", (unsigned long long)fake_ms, failure); exit(1); }
}
static int audio_read(hal_audio_t *self, void *data, size_t length, size_t *got) {
    (void)self;
    assert(!lvgl_depth && length == 640);
    read_calls++; fake_ms += 20;
    if (read_fails) { *got = 0; return -1; }
    int16_t *samples = data;
    for (unsigned i = 0; i < length / sizeof(*samples); ++i)
        samples[i] = (int16_t)(i & 1 ? audio_amplitude : -audio_amplitude);
    *got = length;
    if (during_read) { void (*hook)(void) = during_read; during_read = NULL; hook(); }
    return 0;
}
static bool acquire(void *user) {
    (void)user; assert(!lvgl_depth); acquire_calls++;
    if (during_acquire) { void (*hook)(void) = during_acquire; during_acquire = NULL; hook(); }
    return !acquire_fails;
}
static void release(void *user) { (void)user; assert(!lvgl_depth); release_calls++; }
static const hal_audio_api_t audio_api = {.read = audio_read};
static hal_audio_t fake_audio = {.api = &audio_api};
static int identity_sn(hal_identity_t *self, char *out, int cap) {
    (void)self; assert(!lvgl_depth);
    if (identity_sn_fails) { out[0] = 0; return -1; }
    return snprintf(out, cap, "PREVIEW01");
}
static int identity_key(hal_identity_t *self, char *out, int cap) {
    (void)self; assert(!lvgl_depth);
    if (identity_key_fails || strlen(identity_key_value) >= (size_t)cap) { out[0] = 0; return -1; }
    return snprintf(out, cap, "%s", identity_key_value);
}
static const hal_identity_api_t identity_api = {.get_sn = identity_sn, .get_key = identity_key};
static hal_identity_t fake_identity = {.api = &identity_api};

static void reset_app(void) {
    next_xz_action=UI_XZ_NONE;xz_interrupts=volume_changes=mode_changes=0;setting_saved=true;reported_saved=false;
    fake_xz_running=fake_xz_fault=false; xz_closes=0; xz_restart=xz_leaving=false;
    fake_xz_phase=XZ_READY; resume_setup_after_sleep=false;
    xz_stop_requests=0;voice_check_deadline=0;voice_check_return_home=false;atomic_store(&voice_check_pending,false);
    if (queue) { vQueueDelete(queue); queue = NULL; }
    C = (app_plays_config_t){0};
    atomic_store(&active, PLAY_CLOSED); atomic_store(&abort_microphone, false);
    atomic_store(&sleep_gate.state, 0); atomic_store(&input_generation, 0);
    atomic_store(&config_connections, 0);
    guide_open = false; guide_opens = guide_closes = lock_failures = 0;
    identity_key_value = "PREVIEW_ONLY"; identity_key_fails = identity_sn_fails = false;
    clicker_activations = clicker_sends = 0; clicker_transport_active = false;
    before_clicker_disable = after_clicker_disable = NULL;
    during_lvgl_lock = NULL;
    memset(key_generation, 0, sizeof(key_generation));
    mic_owned = false;
    pending_scores = (play_scores_t){0};
    fake_ms = 1000; empty_steps = lvgl_depth = 0;
    random_calls = acquire_calls = release_calls = read_calls = 0;
    audio_amplitude = 0;
    acquire_fails = read_fails = false;
    during_read = during_acquire = NULL;
    app_plays_config_t config = {.audio = &fake_audio, .identity = &fake_identity,
        .acquire_audio = acquire, .release_audio = release, .exit = home};
    assert(app_plays_init(&config)); assert(app_plays_open(0));
}
static void exit_during_io(void) { child_exited(NULL); }
static void reopen_answer_during_acquire(void) {
    child_exited(NULL); assert(launch(UI_PLAY_ANSWER));
}
static void overflow_during_read(void) {
    for (unsigned i = 0; i < 16; ++i) assert(post(EVENT_KEY, 1, HAL_BTN_RELEASE));
    app_plays_button(2, HAL_BTN_PRESS);
}
static void answer_audio_and_acquire_generation(void) {
    reset_app(); child_exited(NULL); pump(); assert(launch(UI_PLAY_ANSWER));
    audio_amplitude = 8000;
    key(fake_ms, 0, HAL_BTN_PRESS); steps(100);
    assert(mic_owned && acquire_calls == 1);
    key(fake_ms, 0, HAL_BTN_RELEASE);
    assert(!mic_owned && release_calls == 1 && session.view.state == AB_STATE_PAGE_TURN);
    assert(session.view.answer_ready && random_calls == 1);
    steps(40);
    assert(session.view.state == AB_STATE_ANSWER);
    reset_app(); child_exited(NULL); pump(); assert(launch(UI_PLAY_ANSWER));
    during_acquire = reopen_answer_during_acquire;
    key(fake_ms, 0, HAL_BTN_PRESS);
    assert(!mic_owned && acquire_calls == 1 && release_calls == 1);
    assert(read_calls == 0 && random_calls == 0); // No old-page PCM on the reopened book.
}
static void input_regressions(void) {
    app_plays_config_t config = {.exit = home};
    assert(app_plays_init(&config));
    // Current physical order: DOWN at 1000, held 300 ms (>180), UP at
    // 1300 opens the homepage dock through LVGL. The iot_button raw
    // SINGLE_CLICK arrives at 1490, strictly after its 180 ms window.
    key(1000, 2, HAL_BTN_PRESS);
    fake_ms = 1300; assert(app_plays_open(0));
    key(1300, 2, HAL_BTN_RELEASE);
    key(1490, 2, HAL_BTN_CLICK);
    check(atomic_load(&active) == PLAY_MENU && selections == 0,
          "homepage release's delayed CLICK entered Answer Book instead of staying in Plays");

    // A new press shortly after opening is intentional and must be accepted;
    // this also catches replacing the bug with an arbitrary longer debounce.
    exited(NULL); fake_ms = 2000; assert(app_plays_open(0));
    unsigned before = menu_keys;
    key(2010, 1, HAL_BTN_PRESS);
    check(menu_keys == before + 1, "fresh down press within 180 ms was discarded");
    key(2020, 1, HAL_BTN_RELEASE);
    key(2210, 1, HAL_BTN_CLICK);

    // An explicit new OK gesture launches exactly once.
    key(2300, 2, HAL_BTN_PRESS); key(2600, 2, HAL_BTN_RELEASE); key(2790, 2, HAL_BTN_CLICK);
    check(atomic_load(&active) == UI_PLAY_ANSWER && selections == 1, "new OK gesture did not open Answer Book once");

    // Long OK returns to the menu while held. Its later RELEASE and CLICK
    // must retain child ownership regardless of how long the user waits.
    key(2900, 2, HAL_BTN_PRESS); key(4400, 2, HAL_BTN_LONG);
    check(atomic_load(&active) == PLAY_MENU, "long OK did not return to Plays");
    key(4900, 2, HAL_BTN_RELEASE); key(5090, 2, HAL_BTN_CLICK);
    check(atomic_load(&active) == PLAY_MENU && selections == 1, "child exit gesture leaked back into menu");

    // Up was held on the menu when another path opened the book. Its release
    // and a delayed old event may not start/finish a new microphone session.
    key(5200, 0, HAL_BTN_PRESS);
    assert(launch(UI_PLAY_ANSWER));
    key(5500, 0, HAL_BTN_RELEASE);
    check(session.view.state == AB_STATE_IDLE, "held menu key became a book recording");
    child_exited(NULL);

    // Closing Plays and opening it again invalidates an old menu gesture too.
    return_selected = true;
    key(5700, 2, HAL_BTN_PRESS); key(6000, 2, HAL_BTN_RELEASE); key(6190, 2, HAL_BTN_CLICK);
    check(!app_plays_active() && home_exits == 2, "short OK on Return Home did not close Plays");
    fake_ms = 7000; assert(app_plays_open(0)); return_selected = false;
    key(7400, 2, HAL_BTN_RELEASE); key(7590, 2, HAL_BTN_CLICK);
    check(atomic_load(&active) == PLAY_MENU && selections == 1, "old closed-menu gesture leaked across reopen");

    // Keys queued before a page transition must be rejected when the worker
    // eventually runs, even though they had a valid owner when produced.
    fake_ms = 7800; app_plays_button(2, HAL_BTN_PRESS);
    fake_ms = 8100; app_plays_button(2, HAL_BTN_RELEASE);
    fake_ms = 8290; app_plays_button(2, HAL_BTN_CLICK);
    assert(launch(UI_PLAY_ANSWER)); child_exited(NULL); pump();
    check(atomic_load(&active) == PLAY_MENU && selections == 1, "queued old-page CLICK survived a transition");
    before = menu_keys;
    key(8400, -1, HAL_BTN_PRESS); key(8410, 3, HAL_BTN_PRESS);
    check(menu_keys == before, "out-of-range key reached navigation");
    puts("app_plays input: physical entry, fast new input, child return and reopen ownership passed");
}
static void connect_menu_qr_flow(void) {
    reset_app();
    child_exited(NULL);
    // Connections before explicit entry must not immediately close the QR.
    app_plays_config_connected();
    key(1100, 2, HAL_BTN_PRESS);
    assert(choose(UI_PLAY_CONNECT, NULL)); pump();
    check(guide_open && atomic_load(&active) == UI_PLAY_CONNECT, "menu did not open the real-identity QR");
    key(1200, 2, HAL_BTN_RELEASE); key(1390, 2, HAL_BTN_CLICK);
    check(guide_open && guide_closes == 0, "QR swallowed by the same gesture that opened it");
    key(1400, 0, HAL_BTN_PRESS); key(1430, 0, HAL_BTN_RELEASE);
    check(guide_open, "up unexpectedly dismissed QR");
    key(1500, 2, HAL_BTN_PRESS); key(1550, 2, HAL_BTN_RELEASE); key(1740, 2, HAL_BTN_CLICK);
    check(!guide_open && atomic_load(&active) == PLAY_MENU, "short OK did not return to Plays");

    assert(choose(UI_PLAY_CONNECT, NULL)); pump();
    check(guide_open && guide_opens == 2, "QR could not be reopened after skipping");
    unsigned before = home_exits;
    app_plays_config_connected(); // The host callback must do no UI work.
    check(guide_open, "BLE callback modified UI directly");
    lock_failures = 1; refresh(fake_ms);
    check(guide_open, "failed LVGL lock lost the connection transition");
    refresh(fake_ms);
    check(!guide_open && !app_plays_active() && home_exits == before + 1,
          "new mini-program connection did not close the QR and return home");

    assert(app_plays_open(0)); assert(choose(UI_PLAY_CONNECT, NULL)); pump();
    key(1800, 2, HAL_BTN_PRESS); key(2600, 2, HAL_BTN_LONG);
    key(2700, 2, HAL_BTN_RELEASE); key(2890, 2, HAL_BTN_CLICK);
    check(!guide_open && atomic_load(&active) == PLAY_MENU, "held return gesture reopened another page");
    C.identity = NULL;
    check(!choose(UI_PLAY_CONNECT, NULL), "missing identity should not enqueue an invalid QR");
}
static void phone_clicker_mode_routing(void) {
    clicker_activations = 0;
    reset_app(); child_exited(NULL); pump(); assert(launch(UI_PLAY_CLICKER));
    clicker_phone = clicker_touch = clicker_scroll_reversed = false; clicker_sends = 0;
    assert(clicker_activations == 0); // Choosing a use is required before advertising.
    next_clicker_action = UI_CLICK_ACTION_COMPUTER_MODE;
    key(fake_ms + 400, 2, HAL_BTN_PRESS); key(fake_ms + 60, 2, HAL_BTN_RELEASE); key(fake_ms + 60, 2, HAL_BTN_CLICK);
    assert(clicker_activations == 1 && !clicker_phone && clicker_sends == 0);
    next_clicker_action = UI_CLICK_ACTION_NEXT;
    key(fake_ms + 400, 1, HAL_BTN_PRESS); key(fake_ms + 60, 1, HAL_BTN_RELEASE); key(fake_ms + 60, 1, HAL_BTN_CLICK);
    assert(recorded_clicker_action == CLICKER_NEXT && clicker_sends == 1);
    next_clicker_action = UI_CLICK_ACTION_PHONE_MODE;
    key(fake_ms + 400, 2, HAL_BTN_PRESS); key(fake_ms + 60, 2, HAL_BTN_RELEASE); key(fake_ms + 60, 2, HAL_BTN_CLICK);
    assert(clicker_phone && clicker_sends == 1); // Mode change sends no host key.
    next_clicker_action = UI_CLICK_ACTION_NEXT;
    key(fake_ms + 400, 1, HAL_BTN_PRESS); key(fake_ms + 60, 1, HAL_BTN_RELEASE); key(fake_ms + 60, 1, HAL_BTN_CLICK);
    assert(recorded_clicker_action == CLICKER_PHONE_NEXT && clicker_sends == 2);
    next_clicker_action = UI_CLICK_ACTION_PREV;
    key(fake_ms + 400, 0, HAL_BTN_PRESS); key(fake_ms + 60, 0, HAL_BTN_RELEASE); key(fake_ms + 60, 0, HAL_BTN_CLICK);
    assert(recorded_clicker_action == CLICKER_PHONE_PREV && clicker_sends == 3);
    fake_clicker_snapshot.status = PLATFORM_CLICKER_READY;
    fake_clicker_snapshot.keyboard_ready = true; fake_clicker_snapshot.mouse_ready = false;
    refresh(fake_ms); assert(recorded_clicker_view.status == UI_CLICK_PAIRING);
    fake_clicker_snapshot.mouse_ready = true;
    refresh(fake_ms); assert(recorded_clicker_view.status == UI_CLICK_READY && recorded_clicker_view.phone_mode);
    next_clicker_action = UI_CLICK_ACTION_TOUCH_MODE;
    key(fake_ms+400,2,HAL_BTN_PRESS); key(fake_ms+60,2,HAL_BTN_RELEASE); key(fake_ms+60,2,HAL_BTN_CLICK);
    assert(clicker_phone && clicker_touch && clicker_sends==3);
    next_clicker_action=UI_CLICK_ACTION_NEXT;
    key(fake_ms+400,1,HAL_BTN_PRESS); key(fake_ms+60,1,HAL_BTN_RELEASE); key(fake_ms+60,1,HAL_BTN_CLICK);
    assert(recorded_clicker_action==CLICKER_TOUCH_NEXT && clicker_sends==4);
    next_clicker_action=UI_CLICK_ACTION_PREV;
    key(fake_ms+400,0,HAL_BTN_PRESS); key(fake_ms+60,0,HAL_BTN_RELEASE); key(fake_ms+60,0,HAL_BTN_CLICK);
    assert(recorded_clicker_action==CLICKER_TOUCH_PREV && clicker_sends==5);
    fake_clicker_snapshot.touch_ready=false;
    refresh(fake_ms); assert(recorded_clicker_view.status==UI_CLICK_PAIRING);
    fake_clicker_snapshot.touch_ready=true;
    refresh(fake_ms); assert(recorded_clicker_view.status==UI_CLICK_READY && recorded_clicker_view.touch_mode);
    next_clicker_action=UI_CLICK_ACTION_COMPUTER_MODE;
    key(fake_ms+400,2,HAL_BTN_PRESS); key(fake_ms+60,2,HAL_BTN_RELEASE); key(fake_ms+60,2,HAL_BTN_CLICK);
    assert(!clicker_touch && !clicker_phone);
    next_clicker_action = UI_CLICK_ACTION_NONE;
}
static void connect_qr_identity_boundaries(void) {
    reset_app(); child_exited(NULL);
    char long_key[65]; memset(long_key, 'k', 64); long_key[64] = 0;
    identity_key_value = long_key;
    assert(choose(UI_PLAY_CONNECT, NULL)); pump();
    assert(guide_open && strlen(guide_key) == 64 && !strcmp(guide_key, long_key));
    assert(platform_lvgl_lock(0)); ui_button_guide_close(); child_exited(NULL); platform_lvgl_unlock();
    identity_key_fails = true;
    assert(choose(UI_PLAY_CONNECT, NULL)); pump();
    assert(!guide_open && guide_opens == 1 && atomic_load(&active) == PLAY_MENU);
    identity_key_fails = false; identity_key_value = "";
    assert(choose(UI_PLAY_CONNECT, NULL)); pump();
    assert(!guide_open && guide_opens == 1 && atomic_load(&active) == PLAY_MENU);
    identity_key_value = "PREVIEW_ONLY"; identity_sn_fails = true;
    assert(choose(UI_PLAY_CONNECT, NULL)); pump();
    assert(!guide_open && guide_opens == 1 && atomic_load(&active) == PLAY_MENU);
    identity_sn_fails = false;
    assert(choose(UI_PLAY_CONNECT, NULL)); pump();
    assert(guide_open && guide_opens == 2); // Failure leaves a navigable, retryable menu.
    identity_key_value = "PREVIEW_ONLY";
}
static void start_clicker(ui_click_action_t use) {
    reset_app(); child_exited(NULL); pump(); assert(launch(UI_PLAY_CLICKER));
    next_clicker_action = use;
    key(fake_ms + 10, 2, HAL_BTN_PRESS); key(fake_ms + 10, 2, HAL_BTN_CLICK);
    assert(clicker_transport_active && clicker_activations == 1);
}
static void clicker_sleep_cancel_recovery(void) {
    const ui_click_action_t uses[] = {UI_CLICK_ACTION_COMPUTER_MODE, UI_CLICK_ACTION_PHONE_MODE,
                                      UI_CLICK_ACTION_TOUCH_MODE};
    for (unsigned i = 0; i < sizeof uses / sizeof uses[0]; ++i) {
        start_clicker(uses[i]);
        assert(!app_plays_prepare_sleep()); steps(1);
        assert(play_sleep_frozen(&sleep_gate) && !clicker_transport_active);
        app_plays_cancel_sleep(); app_plays_cancel_sleep();
        assert(!clicker_transport_active); // The caller never touches Bluetooth.
        pump(); assert(clicker_transport_active && clicker_activations == 2);
        app_plays_cancel_sleep(); steps(2);
        assert(clicker_activations == 2); // Repeated cancellation is idempotent.
        next_clicker_action = UI_CLICK_ACTION_NEXT;
        key(fake_ms + 10, 1, HAL_BTN_PRESS); key(fake_ms + 10, 1, HAL_BTN_CLICK);
        assert(clicker_sends == 1);
    }
    // Cancelling PREPARING before the worker touches HID leaves it enabled.
    start_clicker(UI_CLICK_ACTION_COMPUTER_MODE);
    assert(!app_plays_prepare_sleep()); app_plays_cancel_sleep(); steps(1);
    assert(clicker_transport_active && clicker_activations == 1);
    // Cancellation racing either side of the transport-disable call recovers.
    for (unsigned after = 0; after < 2; ++after) {
        start_clicker(UI_CLICK_ACTION_COMPUTER_MODE);
        if (after) after_clicker_disable = app_plays_cancel_sleep;
        else before_clicker_disable = app_plays_cancel_sleep;
        assert(!app_plays_prepare_sleep()); steps(1);
        assert(!play_sleep_blocked(&sleep_gate) && clicker_transport_active);
        assert(clicker_activations == 2);
    }
    // A failed UI lock leaves PREPARING after transport was stopped.
    start_clicker(UI_CLICK_ACTION_PHONE_MODE);
    lock_failures = 1; assert(!app_plays_prepare_sleep()); steps(1);
    assert(!play_sleep_frozen(&sleep_gate) && !clicker_transport_active);
    app_plays_cancel_sleep(); pump(); assert(clicker_transport_active);
    // A blocked resume lock retains its marker for the worker's next pass.
    assert(!app_plays_prepare_sleep()); steps(1); app_plays_cancel_sleep();
    lock_failures = 1; pump(); assert(!clicker_transport_active);
    pump(); assert(clicker_transport_active);
    // Merely viewing the use selector must never start advertising on resume.
    reset_app(); child_exited(NULL); pump(); assert(launch(UI_PLAY_CLICKER));
    assert(!app_plays_prepare_sleep()); steps(1); app_plays_cancel_sleep(); pump();
    assert(!clicker_transport_active && clicker_activations == 0);
    // COMMITTED is terminal even if a late cancel callback arrives.
    start_clicker(UI_CLICK_ACTION_COMPUTER_MODE);
    assert(!app_plays_prepare_sleep()); steps(1); assert(app_plays_commit_sleep());
    app_plays_cancel_sleep(); pump();
    assert(play_sleep_frozen(&sleep_gate) && !clicker_transport_active && clicker_activations == 1);
}
static void reopen_clicker_during_resume(void) {
    child_exited(NULL); assert(launch(UI_PLAY_CLICKER));
}
static void clicker_sleep_stale_session(void) {
    for (unsigned destination = 0; destination < 3; ++destination) {
        start_clicker(UI_CLICK_ACTION_PHONE_MODE);
        assert(!app_plays_prepare_sleep()); steps(1);
        if (destination == 0) exited(NULL);
        else {
            child_exited(NULL);
            assert(launch(destination == 1 ? UI_PLAY_ANSWER : UI_PLAY_CLICKER));
        }
        app_plays_cancel_sleep(); pump();
        assert(!clicker_transport_active && clicker_activations == 1);
    }
    // Exiting an active use also clears intent for a fresh selector session.
    start_clicker(UI_CLICK_ACTION_COMPUTER_MODE);
    key(fake_ms + 10, 2, HAL_BTN_PRESS); key(fake_ms + 10, 2, HAL_BTN_LONG);
    assert(!clicker_transport_active && !clicker_enabled);
    assert(launch(UI_PLAY_CLICKER));
    assert(!app_plays_prepare_sleep()); steps(1); app_plays_cancel_sleep(); pump();
    assert(!clicker_transport_active && clicker_activations == 1);
    // The UI changes pages while resume is acquiring LVGL, after its first check.
    start_clicker(UI_CLICK_ACTION_PHONE_MODE);
    assert(!app_plays_prepare_sleep()); steps(1); app_plays_cancel_sleep();
    during_lvgl_lock = reopen_clicker_during_resume; pump();
    assert(!clicker_transport_active && clicker_activations == 1);
}
static void iphone_pointer_diagnostic(void) {
    reset_app(); child_exited(NULL); pump(); assert(launch(UI_PLAY_CLICKER));
    clicker_phone = true; clicker_touch = false; clicker_scroll_reversed = true; clicker_sends = 0;
    next_clicker_action = UI_CLICK_ACTION_POINTER_TEST;
    key(fake_ms+400,2,HAL_BTN_PRESS); key(fake_ms+60,2,HAL_BTN_RELEASE); key(fake_ms+60,2,HAL_BTN_CLICK);
    assert(recorded_clicker_action == CLICKER_POINTER_TEST && clicker_sends == 1);
    clicker_touch = true;
    key(fake_ms+400,2,HAL_BTN_PRESS); key(fake_ms+60,2,HAL_BTN_RELEASE); key(fake_ms+60,2,HAL_BTN_CLICK);
    assert(clicker_sends == 1);
    clicker_phone = clicker_touch = false;
    key(fake_ms+400,2,HAL_BTN_PRESS); key(fake_ms+60,2,HAL_BTN_RELEASE); key(fake_ms+60,2,HAL_BTN_CLICK);
    assert(clicker_sends == 1);
}
static void iphone_scroll_direction(void) {
    reset_app(); child_exited(NULL); pump(); assert(launch(UI_PLAY_CLICKER));
    clicker_phone = true; clicker_touch = false; clicker_scroll_reversed = false; clicker_sends = 0;
    next_clicker_action = UI_CLICK_ACTION_REVERSE_SCROLL;
    key(fake_ms+400,2,HAL_BTN_PRESS); key(fake_ms+60,2,HAL_BTN_RELEASE); key(fake_ms+60,2,HAL_BTN_CLICK);
    assert(clicker_scroll_reversed && clicker_sends == 0);
    next_clicker_action = UI_CLICK_ACTION_NEXT;
    key(fake_ms+400,1,HAL_BTN_PRESS); key(fake_ms+60,1,HAL_BTN_RELEASE); key(fake_ms+60,1,HAL_BTN_CLICK);
    assert(recorded_clicker_action == CLICKER_PHONE_PREV && clicker_sends == 1);
    next_clicker_action = UI_CLICK_ACTION_PREV;
    key(fake_ms+400,0,HAL_BTN_PRESS); key(fake_ms+60,0,HAL_BTN_RELEASE); key(fake_ms+60,0,HAL_BTN_CLICK);
    assert(recorded_clicker_action == CLICKER_PHONE_NEXT && clicker_sends == 2);
    fake_clicker_snapshot.status = PLATFORM_CLICKER_READY; fake_clicker_snapshot.mouse_ready = true;
    refresh(fake_ms); assert(recorded_clicker_view.scroll_reversed);
    // The iPhone direction preference must never invert Android or PPT input.
    clicker_touch = true; next_clicker_action = UI_CLICK_ACTION_NEXT;
    key(fake_ms+400,1,HAL_BTN_PRESS); key(fake_ms+60,1,HAL_BTN_RELEASE); key(fake_ms+60,1,HAL_BTN_CLICK);
    assert(recorded_clicker_action == CLICKER_TOUCH_NEXT && clicker_sends == 3);
    clicker_phone = clicker_touch = false;
    key(fake_ms+400,1,HAL_BTN_PRESS); key(fake_ms+60,1,HAL_BTN_RELEASE); key(fake_ms+60,1,HAL_BTN_CLICK);
    assert(recorded_clicker_action == CLICKER_NEXT && clicker_sends == 4);
}
static void idle_worker_wakes_on_events(void) {
    reset_app(); exited(NULL); pump();
    assert(last_queue_wait == 500); // Homepage: no 40 Hz polling.
    assert(app_plays_open(0)); pump(); assert(last_queue_wait == 500);
    uint64_t before = fake_ms;
    choose(UI_PLAY_ANSWER, NULL); pump();
    assert(fake_ms == before && atomic_load(&active) == UI_PLAY_ANSWER);
    assert(last_queue_wait <= 50); // Animation/audio phases stay responsive.
    exited(NULL); assert(!app_plays_prepare_sleep()); steps(1);
    assert(play_sleep_frozen(&sleep_gate)); // Coarse idle still reaches READY.
    app_plays_cancel_sleep(); pump(); assert(!play_sleep_frozen(&sleep_gate));
}
static void voice_exit_waits_for_radio_restore(void) {
    reset_app(); child_exited(NULL); pump(); assert(launch(UI_PLAY_XIAOZHI));
    pump(); assert(fake_xz_running && atomic_load(&active)==UI_PLAY_XIAOZHI);
    key(fake_ms+400,2,HAL_BTN_PRESS); key(fake_ms+600,2,HAL_BTN_LONG);
    assert(xz_leaving && !xz_closes && atomic_load(&active)==UI_PLAY_XIAOZHI);
    steps(10); assert(!xz_closes && !clicker_activations);
    fake_xz_running=false; pump(); // Cleanup has now restored the BLE host.
    assert(!xz_leaving && xz_closes==1 && atomic_load(&active)!=UI_PLAY_XIAOZHI);
    assert(launch(UI_PLAY_CLICKER)); pump();
    assert(atomic_load(&active)==UI_PLAY_CLICKER && !clicker_activations);
    reset_app(); child_exited(NULL); pump(); assert(launch(UI_PLAY_XIAOZHI)); pump();
    key(fake_ms+400,2,HAL_BTN_PRESS); key(fake_ms+600,2,HAL_BTN_LONG);
    fake_xz_fault=true; fake_xz_running=false; pump();
    assert(!xz_closes && !xz_leaving && xz_exit_fault_shown);
    assert(atomic_load(&active)==UI_PLAY_XIAOZHI);
    key(fake_ms+400,2,HAL_BTN_PRESS); key(fake_ms+600,2,HAL_BTN_LONG); pump();
    assert(xz_closes==1 && atomic_load(&active)!=UI_PLAY_XIAOZHI);
}
static void voice_navigation_fires_on_press_and_release(void) {
    reset_app();child_exited(NULL);pump();assert(launch(UI_PLAY_XIAOZHI));pump();
    memset(xz_key_count,0,sizeof xz_key_count);
    key(2000,1,HAL_BTN_PRESS);assert(xz_key_count[UI_PLAY_DOWN]==1);
    key(2040,1,HAL_BTN_RELEASE);key(2220,1,HAL_BTN_CLICK);assert(xz_key_count[UI_PLAY_DOWN]==1);
    key(2300,0,HAL_BTN_PRESS);assert(xz_key_count[UI_PLAY_UP]==1);
    key(2400,2,HAL_BTN_PRESS);assert(!xz_key_count[UI_PLAY_OK]);
    key(2450,2,HAL_BTN_RELEASE);assert(xz_key_count[UI_PLAY_OK]==1);
    key(2630,2,HAL_BTN_CLICK);assert(xz_key_count[UI_PLAY_OK]==1);
    key(2800,2,HAL_BTN_PRESS);key(4300,2,HAL_BTN_LONG);key(4370,2,HAL_BTN_RELEASE);
    assert(xz_key_count[UI_PLAY_BACK]==1&&xz_key_count[UI_PLAY_OK]==1);
}
static void voice_settings_and_button_interruption(void) {
    reset_app();child_exited(NULL);pump();assert(launch(UI_PLAY_XIAOZHI));pump();
    next_xz_action=UI_XZ_INTERRUPT;
    key(fake_ms+400,2,HAL_BTN_PRESS);key(fake_ms+100,2,HAL_BTN_RELEASE);key(fake_ms+180,2,HAL_BTN_CLICK);
    assert(xz_interrupts==1&&!xz_stop_requests&&!xz_restart);
    next_xz_action=UI_XZ_VOLUME;setting_value=60;
    key(fake_ms+400,2,HAL_BTN_PRESS);key(fake_ms+100,2,HAL_BTN_RELEASE);key(fake_ms+180,2,HAL_BTN_CLICK);
    assert(volume_changes==1&&applied_setting==60&&reported_saved&&!xz_stop_requests&&!xz_restart);
    next_xz_action=UI_XZ_INTERRUPT_MODE;setting_value=1;setting_saved=false;
    key(fake_ms+400,2,HAL_BTN_PRESS);key(fake_ms+100,2,HAL_BTN_RELEASE);key(fake_ms+180,2,HAL_BTN_CLICK);
    assert(mode_changes==1&&!reported_saved&&!xz_stop_requests&&!xz_restart);
    setting_saved=true;
    key(fake_ms+400,2,HAL_BTN_PRESS);key(fake_ms+100,2,HAL_BTN_RELEASE);key(fake_ms+180,2,HAL_BTN_CLICK);
    assert(mode_changes==2&&reported_saved&&xz_stop_requests&&xz_restart);
    fake_xz_running=false;pump();assert(fake_xz_running&&!xz_restart);
}

static unsigned device_brightness=100,device_sleep=5,previews,setting_writes;
static unsigned get_brightness(void*u){(void)u;return device_brightness;}
static unsigned get_sleep(void*u){(void)u;return device_sleep;}
static bool preview(void*u,unsigned n){(void)u;assert(!lock_depth());previews++;applied_setting=n;return true;}
static bool save_brightness(void*u,unsigned n){(void)u;assert(!lock_depth());setting_writes++;if(setting_saved)device_brightness=n;return setting_saved;}
static bool save_sleep(void*u,unsigned n){(void)u;assert(!lock_depth());setting_writes++;if(setting_saved)device_sleep=n;return setting_saved;}
static void setting_key(ui_xz_action_t action,unsigned value){next_xz_action=action;setting_value=value;key(fake_ms+400,2,HAL_BTN_PRESS);key(fake_ms+100,2,HAL_BTN_RELEASE);}
static void device_settings_worker(void){
 reset_app();child_exited(NULL);pump();assert(launch(UI_PLAY_XIAOZHI));pump();
 C.get_brightness=get_brightness;C.get_sleep_minutes=get_sleep;C.preview_brightness=preview;C.set_brightness=save_brightness;C.set_sleep_minutes=save_sleep;
 previews=setting_writes=0;device_brightness=100;device_sleep=5;
 setting_key(UI_XZ_BRIGHTNESS_PREVIEW,55);assert(previews==1&&applied_setting==55&&!setting_writes);
 setting_key(UI_XZ_BRIGHTNESS_CANCEL,0);assert(previews==2&&applied_setting==100&&!setting_writes);
 setting_key(UI_XZ_BRIGHTNESS,55);assert(setting_writes==1&&reported_saved&&device_brightness==55);
 setting_saved=false;setting_key(UI_XZ_SLEEP,30);assert(!reported_saved&&device_sleep==5);
 setting_saved=true;setting_key(UI_XZ_SLEEP,30);assert(reported_saved&&device_sleep==30);
 assert(!xz_stop_requests&&!xz_restart);
}
static void standby_sleep_cancel_recovers(void){
 reset_app();assert(launch(UI_PLAY_XIAOZHI));pump();fake_xz_phase=XZ_STANDBY;
 assert(!app_plays_voice_busy());
 assert(!app_plays_prepare_sleep());steps(1);assert(xz_stop_requests&&xz_restart&&!xz_setup);
 fake_xz_running=false;steps(1);assert(play_sleep_frozen(&sleep_gate));
 app_plays_cancel_sleep();pump();assert(fake_xz_running&&!xz_restart&&!forced_setup);
}

static void idle_has_no_background_audio_or_draw(void) {
    reset_app();
    steps(150); // More than the former 60-second pet checkpoint interval.
    assert(!mic_owned && !acquire_calls && !release_calls && !read_calls && !random_calls);
    assert(atomic_load(&active) == PLAY_MENU);
    exited(NULL); steps(150);
    assert(!mic_owned && !acquire_calls && !read_calls && !random_calls);
}
static void answer_audio_failure_and_timeout(void) {
    reset_app(); assert(launch(UI_PLAY_ANSWER)); acquire_fails = true;
    key(fake_ms, 0, HAL_BTN_PRESS);
    assert(!mic_owned && acquire_calls == 1 && !read_calls && !release_calls);
    assert(session.view.state == AB_STATE_MIC_ERROR && !random_calls);
    reset_app(); assert(launch(UI_PLAY_ANSWER)); read_fails = true;
    key(fake_ms, 0, HAL_BTN_PRESS);
    assert(!mic_owned && release_calls == 1 && read_calls == 1 && !random_calls);
    assert(session.view.state == AB_STATE_MIC_ERROR);
    reset_app(); assert(launch(UI_PLAY_ANSWER)); audio_amplitude = 8000;
    key(fake_ms, 0, HAL_BTN_PRESS); steps(750);
    assert(!mic_owned && release_calls == 1 && random_calls == 1);
    for (unsigned i = 0; i < 320; ++i) assert(mic_frame[i] == 0);
}
static void answer_cancellation_and_sleep(void) {
    reset_app(); assert(launch(UI_PLAY_ANSWER));
    key(fake_ms, 0, HAL_BTN_PRESS);
    during_read = exit_during_io; steps(1);
    assert(!mic_owned && release_calls == 1 && !random_calls);
    for (unsigned i = 0; i < 320; ++i) assert(mic_frame[i] == 0);
    reset_app(); assert(launch(UI_PLAY_ANSWER));
    key(fake_ms, 0, HAL_BTN_PRESS);
    during_read = overflow_during_read; steps(1);
    assert(!mic_owned && release_calls == 1 && !random_calls);
    reset_app(); assert(launch(UI_PLAY_ANSWER));
    key(fake_ms, 0, HAL_BTN_PRESS);
    unsigned reads = read_calls;
    assert(!app_plays_prepare_sleep()); steps(1);
    assert(play_sleep_frozen(&sleep_gate) && !mic_owned && read_calls == reads && release_calls == 1);
    app_plays_cancel_sleep(); pump();
    assert(!play_sleep_frozen(&sleep_gate) && !mic_owned && release_calls == 1);
}
static void bounded_serial_voice_check(void) {
    reset_app();
    assert(app_plays_voice_check());assert(!app_plays_voice_check());
    uint64_t requested=fake_ms;pump();
    assert(fake_xz_running&&atomic_load(&active)==UI_PLAY_XIAOZHI);
    assert(voice_check_deadline==requested+45000&&atomic_load(&voice_check_pending));
    fake_ms=voice_check_deadline-1;pump();assert(!xz_stop_requests);
    fake_ms++;pump();assert(xz_stop_requests&&!voice_check_deadline&&xz_leaving);
    assert(atomic_load(&active)==UI_PLAY_XIAOZHI); // Wait for actual cleanup.
    fake_xz_running=false;pump();assert(!app_plays_active()&&!atomic_load(&voice_check_pending));

    reset_app();assert(app_plays_voice_check());atomic_fetch_add(&input_generation,1);pump();
    assert(!fake_xz_running&&!atomic_load(&voice_check_pending));
    reset_app();assert(app_plays_voice_check());fake_ms+=45000;pump();
    assert(!fake_xz_running&&!atomic_load(&voice_check_pending));
    reset_app();assert(launch(UI_PLAY_ANSWER));assert(!app_plays_voice_check());
    reset_app();assert(!app_plays_prepare_sleep());assert(!app_plays_voice_check());
    reset_app();for(unsigned i=0;i<16;i++)assert(post(EVENT_KEY,0,HAL_BTN_PRESS));
    assert(!app_plays_voice_check()&&!atomic_load(&voice_check_pending));
    reset_app();assert(app_plays_voice_check());pump();fake_xz_running=false;pump();
    assert(!app_plays_active()&&!atomic_load(&voice_check_pending)); // Host stop/error.
}
static void boot_voice_opened(void *user) {
    (void)user;assert(lock_depth());assert(!fake_xz_running);voice_opened_calls++;
}
static void boot_starts_normal_voice_flow(void) {
    reset_app();platform_lvgl_lock(1);ui_plays_close();platform_lvgl_unlock();
    C.voice_opened=boot_voice_opened;voice_opened_calls=0;forced_setup=true;
    platform_lvgl_lock(1);assert(app_plays_open_xiaozhi(123));
    assert(!fake_xz_running&&!voice_opened_calls);platform_lvgl_unlock();
    lock_failures=2;steps(5); // Retain the worker stack across transient lock failures.
    assert(fake_xz_running&&!forced_setup&&voice_opened_calls==1);
    assert(atomic_load(&active)==UI_PLAY_XIAOZHI);
    assert(!app_plays_open_xiaozhi(123));
    // Saturated queue must unwind to home, not leave a blank suspended menu.
    reset_app();platform_lvgl_lock(1);ui_plays_close();
    for(unsigned i=0;i<16;i++)assert(post(EVENT_KEY,0,HAL_BTN_PRESS));
    assert(!app_plays_open_xiaozhi(123)&&!app_plays_active());platform_lvgl_unlock();pump();
    assert(!fake_xz_running);
}
static void setup_sleeps_and_reopens(void) {
    for (unsigned wake=0; wake<2; ++wake) {
        reset_app();
        assert(launch(UI_PLAY_XIAOZHI)); pump();
        assert(fake_xz_running && !forced_setup);
        fake_xz_phase=XZ_SETUP;
        assert(!app_plays_prepare_sleep()); steps(1);
        assert(resume_setup_after_sleep && xz_stop_requests);
        fake_xz_running=false; steps(1);
        assert(play_sleep_frozen(&sleep_gate));
        if (wake) {
            assert(app_plays_commit_sleep());
            // Deep sleep resets normal RAM. Only the actual RTC flag survives.
            bool saved=resume_setup_after_sleep;
            reset_app(); resume_setup_after_sleep=saved;
            platform_lvgl_lock(1); ui_plays_close();
            assert(app_plays_open_xiaozhi(0)); platform_lvgl_unlock();
        } else {
            app_plays_cancel_sleep(); // Button arrived before final sleep commit.
        }
        pump();
        assert(fake_xz_running && forced_setup && !resume_setup_after_sleep);
    }
}
static void welcome_bottom_key(void) {
    reset_app();child_exited(NULL);pump();assert(launch(UI_PLAY_XIAOZHI));pump();
    fake_xz_phase=XZ_WELCOME;next_xz_action=UI_XZ_BEGIN_SETUP;setup_button_presses=0;
    key(fake_ms+10,0,HAL_BTN_PRESS);key(fake_ms+10,1,HAL_BTN_PRESS);key(fake_ms+10,2,HAL_BTN_PRESS);
    assert(!setup_button_presses);key(fake_ms+10,2,HAL_BTN_RELEASE);key(fake_ms+10,2,HAL_BTN_CLICK);
    assert(setup_button_presses==1&&!xz_stop_requests);
    assert(!app_plays_prepare_sleep());steps(1);fake_xz_running=false;steps(1);app_plays_cancel_sleep();pump();
    assert(fake_xz_running&&!forced_setup);
}
int main(int argc, char **argv) {
    if (argc == 1) input_regressions();
    const struct { const char *name; void (*run)(void); } tests[] = {
        {"device_settings_worker",device_settings_worker},
        {"standby_sleep_cancel_recovers",standby_sleep_cancel_recovers},
        {"welcome_bottom_key", welcome_bottom_key},
        {"boot_starts_normal_voice_flow", boot_starts_normal_voice_flow},
        {"setup_sleeps_and_reopens", setup_sleeps_and_reopens},
        {"bounded_serial_voice_check", bounded_serial_voice_check},
        {"voice_settings_and_button_interruption", voice_settings_and_button_interruption},
        {"voice_navigation_fires_on_press_and_release", voice_navigation_fires_on_press_and_release},
        {"idle_has_no_background_audio_or_draw", idle_has_no_background_audio_or_draw},
        {"answer_audio_failure_and_timeout", answer_audio_failure_and_timeout},
        {"answer_cancellation_and_sleep", answer_cancellation_and_sleep},
        {"voice_exit_waits_for_radio_restore", voice_exit_waits_for_radio_restore},
        {"idle_worker_wakes_on_events", idle_worker_wakes_on_events},
        {"iphone_pointer_diagnostic", iphone_pointer_diagnostic},
        {"iphone_scroll_direction", iphone_scroll_direction},
        {"phone_clicker_mode_routing", phone_clicker_mode_routing},
        {"clicker_sleep_cancel_recovery", clicker_sleep_cancel_recovery},
        {"clicker_sleep_stale_session", clicker_sleep_stale_session},
        {"connect_qr_identity_boundaries", connect_qr_identity_boundaries},
        {"connect_menu_qr_flow", connect_menu_qr_flow},
        {"answer_audio_and_acquire_generation", answer_audio_and_acquire_generation},
    };
    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (argc > 1 && strcmp(argv[1], tests[i].name)) continue;
        tests[i].run(); printf("app plays: %s PASS\n", tests[i].name);
    }
    return 0;
}
