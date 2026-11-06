# AI PASSPORT natural speech interruption

User requirement, confirmed 2026-11-02: while AI is speaking, the user's natural speech must interrupt playback without a per-turn button. Confirm ends the whole conversation. Echo cancellation is explicitly requested for evaluation. Continue the existing whole-firmware repair/acceptance goal, retain fonts, provisioning, Wi-Fi persistence, non-pet features, identity and saved user content.

The user has already asked for autonomous implementation and testing without repeated design questions. Implementation choices below belong to the implementer, not to an invented user approval. Do not commit or publish without separate confirmation.

## Current measured constraints

Baseline preview.23 uses ESP32-C3,160MHz,noPSRAM,ES8311 with common-clock RX/TX I2S. Opus encoder24524B and decoder17784B use one24KiB arena and must remain mutually exclusive. The voice task has28KiB stack, observed minimum4496B free. After TLS, ordinary listening free heap is approximately29KiB with largest blocks12.5–17KiB. AEC must be allocated after network/TLS setup, bounded, and released on cancellation/retry. Retain40KiB LVGL pool and complete shipped font set.

Phone provisioning immediate password/success is user-accepted. Original standby300 was restored and verified after reboot. The user confirmed ordinary human voice replies on preview.23; only playback interruption was missing. Preview.24 received the controlled acoustic prompt and reached cloud playback, then crashed twice without a debugger: NULL I2S DMA descriptor startup. This is a rejected candidate, not a successful voice or AEC acceptance.

## Proposed implementation path and alternatives

Use a small fixed-point reference-based echo canceller only during playback, combined with local sustained near-end speech detection. On confirmed speech: mute/flush queued playback immediately, send protocol abort, discard stale reply data/control, release decoder, start encoder and automatic listening. Preserve a short cleaned audio pre-roll so the first syllable is not discarded. Keep codec directions exclusive; this achieves natural interruption without two concurrent Opus instances.

First candidate is pinned Xiph SpeexDSP1.2.1 fixed-point mono8kHz,64sample frames,256sample filter, same-clock ES8311 ADC/DAC stereo reference; matched FIR conversion from16/24kHz to8kHz. Host allocation measurement11978B before allocator overhead;512sample variant16082B;1024sample24290B. These are feasibility measurements, not MCU CPU or acoustic proof. Keep source/license and exact upstream commit1b28a0f61bc31162979e1f26f3981fc3637095c8. Use deterministic bounded allocation, and reject optional startup cleanly if the outer workspace cannot be obtained. Never permit an unchecked allocation in the imported initializer to crash on device OOM.

Evaluate C3 ESP-SR libraries as an alternative by actual symbol/resource checks; shared headers alone do not establish support or memory feasibility. The upstream XiaoZhi full AFE configuration targets S3/P4/S31+PSRAM, so importing its full pipeline is not a default fit here. Server-side AEC does not remove the simultaneous-codec constraint and requires server support; no configuration change to a paid/external backend is authorized or necessary at this stage.

Audio RX/TX must use a predictable timeline. Flush stale capture on transition, consume the paired ADC/DAC stream with an8ms write/read cadence and account for DMA latency. Process reference and capture at one sample rate;24kHz downlink must not silently be read as16kHz. Handle underrun, missing packets and saturation explicitly. Energy threshold alone is not acceptable AEC. Avoid claiming synthetic echo reduction as real-room performance.

## Acceptance gates

1. Host: bounded memory and repeated create/process/reset/destroy; silence, delayed filtered echo, echo-only speech, near-end speech with echo, near-end without echo, clipped reference/mic, initial convergence, noise, rate conversion, reference underrun, cancellation and stale post-abort control/audio.
2. MCU: measured AEC frame processing below available frame time, stack margin>=2KiB, minimum heap tracked, no runtime allocation failure/WDT/reset. No packet credentials, PCM or transcripts in logs.
3. Actual device: voice replies with no debugger; own speech alone does not self-interrupt; human/controlled external speech stops reply promptly and recognizes the new utterance, with initial syllable retained; repeated interruptions and normal end/reentry restore BLE and audio. Test representative volume/near-far conditions and distinguish human from synthetic-source evidence.
4. Entire final candidate: build/package/identity-preserving write+readback, current source regression suites, retained UI/BLE/provisioning/settings/sleep checks. Prior preview.23 evidence cannot be inherited as a new-version pass. Startup allocation assertion remains a separate unresolved investigation.

## References checked

- https://github.com/xiph/speexdsp/tree/SpeexDSP-1.2.1
- https://github.com/78/xiaozhi-esp32/blob/main/main/Kconfig.projbuild
- https://github.com/78/xiaozhi-esp32/blob/main/main/protocols/protocol.cc
- https://github.com/78/xiaozhi-esp32/blob/main/docs/mqtt-udp.md
- https://github.com/espressif/esp-sr/tree/master/lib/esp32c3

## Preview.25 measured correction

Preview.24 failed with MEPC0x420f400e, NULL descriptor in i2s_tx_channel_start; a separate run also failed a2880B PCM allocation despite7140B free (largest2688). esp_codec_dev1.5.10 overwrote TX format errors with RX success, ignored format/enable return values in open, and could start a failed peer. Project-local generated corrections are pinned to both upstream source hashes; managed dependencies and the installed SDK are unchanged. Open failure rolls back; HAL no longer starts channels before reconfiguration on retry.

Six128-frame DMA buffers per direction require6144B stereo sample storage, down from11520B; playback/reference consumption now alternates every8ms. Host fault injection and bounded-duplex-FIFO tests reproduced old failures before correction. Preview.25 local194host/69app/76tools/61flasher and5DSP tests pass; actual CPU/heap, channel mapping, acoustic acceptance and all retained final-version features still require device evidence. No claimed physical pass from these counts.

## Preview.26 capability allocation

Preview25 correctly rejected playback without panic, but a512B allocation withcaps2060 (INTERNAL|DMA|8BIT) failed despite7360B total free. ESP32-C3 RTC fast RAM has8bit access but no DMA capability (verified installed SDK memory_layout.c and Espressif heap documentation). CPU-only barge-in/VAD/preroll state is now allocated explicitly from RTCRAM; DSP workspace remains general heap. This preserves about5752B DMA-capable SRAM without changing mono audio behavior or requiring per-turn DSP allocation. Failed RTC allocation destroys the partial echo owner;20 create/process/destroy cycles and forced RTC OOM pass locally. On-device per-capability free/largest metrics are added at voice-ready and around stereo enable. Physical result remains pending.

Reference: https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32c3/api-reference/system/mem_alloc.html

## Preview.27 fixed DMA lifetime

26 proved RTC allocation worked (audio_heap stage0 DMA8096,RTC1588), but real downlink left only3828B DMA before stereo expansion and another512B allocation failed safely.27 keeps physical16bit I2S stereo from initialization. Mono clients retain the original bytes/rate semantics through bounded left-slot extraction and duplicated output; echo mode reads paired slots directly. The fixed6144B duplex sample storage no longer competes with network bursts at playback transition. Factory passthrough uses the same HAL conversion; other bit-depth formats retain their previous path. Added real HAL RED/GREEN tests for physical stereo format, mono sample order and byte counts; full194host/69app/76tools and6DSP local tests pass. Physical mic/playback/echo and whole candidate acceptance are still pending.

## Preview28: bound retained network buffers; observe without halting audio

27 passed the stereo transition on the confirmed target, then failed1700B allocations withcaps6144 and lost MQTT during a reply. The no-debug run remains rejected. The subsequent GDB diagnostic detached while leaving the CPU halted during playback, producing the abnormal sound reported by the user. Cancel was set, CPU resumed, normal voice cleanup observed, debugger removed, and standby300 acknowledged/read back. Do not run further audible or microphone tests until the user is available; do not halt playback for timing measurements.

Installed SDK `wlanif_input` retained the driver's full RX buffers under the zero-copy configuration.28 enables the SDK's L2-to-L3 copy path so a small Opus datagram retains its actual frame length and releases the driver buffer promptly. A per-voice-socket2582B receive quota admits two maximum legal packets; the existing24-entry mailbox also bounds tiny packets. Failure to apply either receive timeout or quota rejects channel startup and the owner closes the socket. This does not alter TLS verification, negotiated audio format, or saved networking data.

The actual installed SDK `wlanif_input` and `recv_udp` functions are compiled in a host fixture with a modeled9684B heap. A24-packet burst of180B payloads retains8820B and encounters19 driver allocation failures in the original zero-copy path. Copied packets with the quota retain4004B at5958B peak, accept14 packets and cleanly drop excess, with zero allocation failures. Two1291B packets, tiny-packet mailbox limits, down-interface, allocation failures, input failure and full/deleted mailbox cleanup are covered. These are ownership/space-model results, not RF, CPU, continuity or hardware acceptance.

Numeric-only observations now report processed frames, microphone/reference/residual energy, clipping, alignment, AEC peak time, decoder peak time, packet size, stack margin and capability-specific heap. Reports are limited to one per second of processed capture plus decoder high-water changes; no samples, transcripts or credentials are retained. Observation counters reset between replies and allocate no frame-time memory. FIR and DSP algorithms remain unchanged until ordinary no-debug timings identify an actual processing bottleneck.
