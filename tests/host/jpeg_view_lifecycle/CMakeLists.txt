cmake_minimum_required(VERSION 3.18)
project(passport_jpeg_view_lifecycle LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(REPO "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(VIEW_SOURCE "${REPO}/components/platform/platform_esp32/src/jpeg_view.c")
set(EXTRACTED "${CMAKE_CURRENT_BINARY_DIR}/jpeg_view_production.inc")
add_custom_command(OUTPUT "${EXTRACTED}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/extract_production.py" "${VIEW_SOURCE}" "${EXTRACTED}"
    DEPENDS "${VIEW_SOURCE}" "${CMAKE_CURRENT_LIST_DIR}/extract_production.py"
    VERBATIM)
set(LV_BUILD_CONF_PATH "${REPO}/tests/host/ui/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${REPO}/managed_components/lvgl__lvgl" lvgl-build)
add_executable(test_jpeg_view_lifecycle test_jpeg_view_lifecycle.c "${EXTRACTED}")
target_include_directories(test_jpeg_view_lifecycle PRIVATE fakes "${CMAKE_CURRENT_BINARY_DIR}"
    "${REPO}/components/core/ports/include" "${REPO}/components/core/services/include"
    "${REPO}/components/platform/platform_esp32/include"
    "${REPO}/components/platform/platform_esp32/src")
target_compile_options(test_jpeg_view_lifecycle PRIVATE -Wall -Wextra -Werror -UNDEBUG)
target_link_libraries(test_jpeg_view_lifecycle PRIVATE lvgl m)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(test_jpeg_view_lifecycle PRIVATE -fsanitize=undefined -fno-sanitize-recover=all)
    target_link_options(test_jpeg_view_lifecycle PRIVATE -fsanitize=undefined -fno-sanitize-recover=all)
endif()
enable_testing()
foreach(scenario deferred replace decode_failure mmap_failure
        blocked_full_with_avatar active_named_avatar active_uploaded_avatar
        first_decode_failure first_mmap_failure seamless_return native_avatar_return exit_queued exit_decoding exit_before_show)
    add_test(NAME jpeg_view_${scenario} COMMAND test_jpeg_view_lifecycle ${scenario})
endforeach()

# Compile the whole production source as well: the original lifecycle target
# intentionally replaces decode_to_flash and cannot catch decode-buffer leaks.
add_executable(test_jpeg_view_decode test_jpeg_view_lifecycle.c
    "${REPO}/components/core/services/src/jpeg_probe.c")
target_compile_definitions(test_jpeg_view_decode PRIVATE JPEG_VIEW_REAL_DECODE=1)
target_include_directories(test_jpeg_view_decode PRIVATE fakes
    "${REPO}/components/core/ports/include" "${REPO}/components/core/services/include"
    "${REPO}/components/platform/platform_esp32/include"
    "${REPO}/components/platform/platform_esp32/src"
    "${REPO}/managed_components/espressif__esp_new_jpeg/include")
target_compile_options(test_jpeg_view_decode PRIVATE -Wall -Wextra -Werror -UNDEBUG)
target_link_libraries(test_jpeg_view_decode PRIVATE lvgl m)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(test_jpeg_view_decode PRIVATE -fsanitize=undefined -fno-sanitize-recover=all)
    target_link_options(test_jpeg_view_decode PRIVATE -fsanitize=undefined -fno-sanitize-recover=all)
endif()
foreach(scenario deferred replace decode_failure mmap_failure
        blocked_full_with_avatar active_named_avatar active_uploaded_avatar
        first_decode_failure first_mmap_failure seamless_return native_avatar_return
        exit_queued exit_decoding exit_before_show stripe_success stripe_alloc_failure
        stripe_begin_failure stripe_write_failure stripe_process_failure
        stripe_zero_outlen stripe_oversize_outlen stripe_header_failure
        stripe_png_unchanged)
    add_test(NAME jpeg_decode_${scenario} COMMAND test_jpeg_view_decode ${scenario})
endforeach()
