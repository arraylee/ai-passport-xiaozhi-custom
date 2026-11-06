# Plays input lifetime and sound-wish regressions

This target compiles the real `app_plays.c` producer/worker and domain services.
Deterministic ESP/FreeRTOS and presentation boundary doubles require no device,
Bluetooth, Flash, audio, LVGL render stack, or firmware build.

```sh
cmake -S tests/host/app_plays_input -B /tmp/passport-app-plays-input
cmake --build /tmp/passport-app-plays-input
ctest --test-dir /tmp/passport-app-plays-input --output-on-failure
```

The reported failure is replayed as PRESS at 1000 ms, a 300 ms hold, RELEASE
opening the homepage dock at 1300 ms, and raw CLICK at 1490 ms. The last event
matches `iot_button` waiting strictly longer than the configured 180 ms repeat
window after release. Before the fix this test fails at 1490 ms because the
worker launches Answer Book; afterward it stays on the Plays menu.

Further checks cover fresh input 10 ms after entering, explicit next-gesture
launch, a held child return and late completion, a held menu key released in
the book, short return home, reopen, queued input across transitions, and
invalid button indexes. The production fix associates subsequent callbacks
with the page generation at PRESS; it does not rely on a longer debounce.

Actual ADC voltage/debounce, LVGL encoder restoration, and physical feel remain
separate device/UI integration acceptance checks.

The same target also drives the real worker through the five-companion wish
flow with synthetic 20 ms PCM frames and an in-memory implementation of the
actual two-slot journal. It checks:

- one audio lease, duration-based completion, and persisted journal bytes before
  a successful UI reveal;
- known and uncertain write failures, unchanged visible ownership on failure,
  and reloading an already-written pending companion without another draw;
- replay of the active companion when all five are owned, and pending-reveal
  reopen/acknowledgement without a new recording or acquisition;
- acquire/read errors, 15-second silence timeout, back/exit, stale queued page
  commands, queue overflow, and invalidation during the final captured frame;
- completion of already-accepted pet commands before the sleep handshake,
  microphone release before READY, and RTC layout/generation matching;
- the original Answer Book audio/reveal path and a page-generation change while
  its audio acquisition is blocked;
- clearing the shared PCM frame after processing rather than retaining audio.

The UI doubles check the publication boundary and journal contents; they do not
render LVGL or prove physical microphone sensitivity, animation feel, or speaker
behavior. Those require separate native-render/device acceptance.

Sleep-cancellation coverage also verifies computer, phone and touch clicker
uses recover only in their original page session. Tests cancel before/after
transport disable and while waiting for the UI lock, repeat cancellation, and
check that committed sleep, leaving/re-entering the page, and an unselected use
never start advertising. QR tests retain a complete 64-byte identity key and
keep the menu usable without opening a QR when key/SN retrieval fails.
