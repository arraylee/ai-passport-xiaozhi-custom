set(SPX "vendor/speexdsp")
set(VAD "vendor/webrtcvad")
set(ECHO_SOURCES "${SPX}/libspeexdsp/mdf.c" "${SPX}/libspeexdsp/fftwrap.c" "${SPX}/libspeexdsp/kiss_fft.c" "${SPX}/libspeexdsp/kiss_fftr.c")
file(GLOB VAD_SOURCES "${VAD}/webrtc/common_audio/vad/*.c" "${VAD}/webrtc/common_audio/signal_processing/*.c" "${VAD}/webrtc/common_audio/third_party/spl_sqrt_floor/*.c")
list(FILTER VAD_SOURCES EXCLUDE REGEX "/(webrtc_vad|spl_init|complex_fft|real_fft)\\.c$")
idf_component_register(SRCS "xz_quiet.c" "xz_prompt.c" "xz_protocol.c" "xz_portal.c" "xz_wifi_profiles.c" "xz_network.c" "xz_cloud_cache.c" "xiaozhi.c" "xz_codec_memory.c" "xz_echo.c" "xz_bargein.c" "xz_downsample.c" "xz_capture.c" "xz_dsp_check.c" ${ECHO_SOURCES} ${VAD_SOURCES}
    INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "." "${SPX}/include" "${SPX}/libspeexdsp" "${VAD}" EMBED_TXTFILES "portal.html" EMBED_FILES "farewell_zh.adpcm" "welcome_zh.pcm"
    REQUIRES ports
    PRIV_REQUIRES services esp_wifi esp_netif esp_event esp_http_client esp_http_server
        mqtt mbedtls nvs_flash esp_timer esp_app_format esp_hw_support json esp_audio_codec)
set_source_files_properties(${ECHO_SOURCES} PROPERTIES COMPILE_DEFINITIONS "FIXED_POINT;USE_KISS_FFT;EXPORT=;OS_SUPPORT_CUSTOM" COMPILE_OPTIONS "-O2;-fwrapv")
set_source_files_properties(${VAD_SOURCES} PROPERTIES COMPILE_DEFINITIONS "WEBRTC_POSIX;NDEBUG" COMPILE_OPTIONS "-O2;-fwrapv")
# The per-sample FIR/capture/VAD wrappers are also on the 8ms audio deadline,
# not just the vendored kernels. Do not compile those loops for minimum size.
set_source_files_properties("xz_capture.c" "xz_downsample.c" "xz_bargein.c" "xz_echo.c"
    PROPERTIES COMPILE_OPTIONS "-O2")
# GCC14 warns on the upstream unused float adapter when N could be zero.
# The product calls only the bounded fixed-point FFT; keep the warning visible.
set_property(SOURCE "${SPX}/libspeexdsp/fftwrap.c" APPEND PROPERTY COMPILE_OPTIONS -Wno-error=maybe-uninitialized)
target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage)
