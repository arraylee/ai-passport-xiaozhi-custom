cmake_minimum_required(VERSION 3.18)
project(passport_app_plays_input LANGUAGES C)
set(CMAKE_C_STANDARD 11)
get_filename_component(REPO "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(SERVICES "${REPO}/components/core/services")
add_executable(test_app_plays_input test_app_plays_input.c
    "${SERVICES}/src/answer_picker.c" "${SERVICES}/src/answer_content.c"
    "${SERVICES}/src/answer_session.c" "${SERVICES}/src/answer_audio_meter.c"
    "${SERVICES}/src/play_runtime.c")
target_include_directories(test_app_plays_input PRIVATE fakes "${REPO}/components/platform/xiaozhi/include"
    "${REPO}/components/app/include" "${REPO}/components/core/ports/include"
    "${SERVICES}/include" "${REPO}/components/ui/presentation/include"
    "${REPO}/components/platform/platform_esp32/include")
target_compile_options(test_app_plays_input PRIVATE -Wall -Wextra -Werror -UNDEBUG)
target_link_libraries(test_app_plays_input PRIVATE m)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(test_app_plays_input PRIVATE -fsanitize=undefined -fno-sanitize-recover=all)
    target_link_options(test_app_plays_input PRIVATE -fsanitize=undefined -fno-sanitize-recover=all)
endif()
enable_testing()
add_test(NAME app_plays_input COMMAND test_app_plays_input)
