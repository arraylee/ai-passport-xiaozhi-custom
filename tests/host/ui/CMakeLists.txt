cmake_minimum_required(VERSION 3.18)
project(passport_ui_preview LANGUAGES C CXX)
find_package(ZLIB REQUIRED)
set(CMAKE_C_STANDARD 11)
get_filename_component(REPO "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${REPO}/managed_components/lvgl__lvgl" lvgl-build)
set(PRESENTATION "${REPO}/components/ui/presentation")
set(PASSPORT_FONT_DIR "${PRESENTATION}/src" CACHE PATH "Native firmware font sources, or an uncompressed comparison set")
set(SERVICES "${REPO}/components/core/services")
add_executable(render_ui render_ui.c
    "${PRESENTATION}/src/ui_theme.c" "${PRESENTATION}/src/ui_profile.c" "${PRESENTATION}/src/ui_album.c"
    "${PRESENTATION}/src/ui_button_guide.c" "${PRESENTATION}/src/ui_selftest.c"
    "${PRESENTATION}/src/ui_licenses.c" "${PRESENTATION}/src/font_notices.c"
    "${PRESENTATION}/src/ui_game.c" "${PRESENTATION}/src/bobo_art.c" "${SERVICES}/src/dodge.c"
    "${PRESENTATION}/src/ui_plays.c" "${SERVICES}/src/answer_content.c"
    "${PASSPORT_FONT_DIR}/ui_font_body.c" "${PASSPORT_FONT_DIR}/ui_font_title.c"
    "${PASSPORT_FONT_DIR}/ui_font_answer.c" "${PRESENTATION}/src/lv_font_cn_16.c"
    "${PRESENTATION}/src/lv_font_cn_24.c")
target_include_directories(render_ui PRIVATE "${PRESENTATION}/include" "${SERVICES}/include"
    "${REPO}/components/core/ports/include")
target_link_libraries(render_ui PRIVATE lvgl m)
set(PASSPORT_PLAIN_FONT_DIR "" CACHE PATH "Optional plain-font reference for pixel-exact comparison")
if(PASSPORT_PLAIN_FONT_DIR)
    get_target_property(render_sources render_ui SOURCES)
    foreach(source ${render_sources})
        if(source MATCHES "/ui_font_[^/]+\\.c$")
            get_filename_component(font_name "${source}" NAME)
            list(APPEND plain_sources "${PASSPORT_PLAIN_FONT_DIR}/${font_name}")
        else()
            list(APPEND plain_sources "${source}")
        endif()
    endforeach()
    # Caption and book are attached below to the normal renderer.
    list(APPEND plain_sources "${PASSPORT_PLAIN_FONT_DIR}/ui_font_caption.c" "${PASSPORT_PLAIN_FONT_DIR}/ui_font_book.c")
    add_executable(render_ui_plain ${plain_sources})
    get_target_property(render_includes render_ui INCLUDE_DIRECTORIES)
    target_include_directories(render_ui_plain PRIVATE ${render_includes})
    target_compile_options(render_ui_plain PRIVATE -UNDEBUG)
    target_link_libraries(render_ui_plain PRIVATE lvgl m)
endif()
add_executable(test_plays_navigation test_plays_navigation.c
    "${PRESENTATION}/src/ui_theme.c" "${PRESENTATION}/src/ui_plays.c"
    "${SERVICES}/src/answer_content.c" "${PASSPORT_FONT_DIR}/ui_font_body.c"
    "${PASSPORT_FONT_DIR}/ui_font_title.c" "${PASSPORT_FONT_DIR}/ui_font_answer.c"
    "${PRESENTATION}/src/lv_font_cn_16.c" "${PRESENTATION}/src/lv_font_cn_24.c")
target_include_directories(test_plays_navigation PRIVATE "${PRESENTATION}/include" "${SERVICES}/include"
    "${REPO}/components/core/ports/include")
target_link_libraries(test_plays_navigation PRIVATE lvgl m)
# Legacy pet UI source remains archived; it is not part of the current firmware suite.
add_executable(test_game_ui test_game_ui.c
    "${PRESENTATION}/src/ui_theme.c" "${PRESENTATION}/src/ui_game.c" "${PRESENTATION}/src/bobo_art.c"
    "${SERVICES}/src/dodge.c" "${PASSPORT_FONT_DIR}/ui_font_body.c" "${PASSPORT_FONT_DIR}/ui_font_title.c"
    "${PRESENTATION}/src/lv_font_cn_16.c" "${PRESENTATION}/src/lv_font_cn_24.c")
target_include_directories(test_game_ui PRIVATE "${PRESENTATION}/include" "${SERVICES}/include")
target_link_libraries(test_game_ui PRIVATE lvgl m)
add_executable(test_ui_licenses test_ui_licenses.c
    "${PRESENTATION}/src/ui_theme.c" "${PRESENTATION}/src/ui_profile.c" "${PRESENTATION}/src/ui_album.c"
    "${PRESENTATION}/src/ui_licenses.c" "${PRESENTATION}/src/font_notices.c"
    "${PRESENTATION}/src/bobo_art.c" "${PASSPORT_FONT_DIR}/ui_font_body.c"
    "${PASSPORT_FONT_DIR}/ui_font_title.c"
    "${PRESENTATION}/src/lv_font_cn_16.c" "${PRESENTATION}/src/lv_font_cn_24.c")
target_include_directories(test_ui_licenses PRIVATE "${PRESENTATION}/include" "${SERVICES}/include"
    "${REPO}/components/core/ports/include")
target_link_libraries(test_ui_licenses PRIVATE lvgl m)
foreach(target render_ui test_plays_navigation test_game_ui test_ui_licenses)
    target_sources(${target} PRIVATE "${PASSPORT_FONT_DIR}/ui_font_caption.c" "${PASSPORT_FONT_DIR}/ui_font_book.c")
    target_compile_options(${target} PRIVATE -UNDEBUG)
endforeach()
enable_testing()
add_test(NAME plays_navigation COMMAND test_plays_navigation)
add_test(NAME game_navigation COMMAND test_game_ui)
add_test(NAME licenses_navigation COMMAND test_ui_licenses)
add_library(passport_font_test_codec OBJECT "${PRESENTATION}/src/passport_font_blocks.c" "${PRESENTATION}/vendor/puff/puff.c")
target_include_directories(passport_font_test_codec PRIVATE "${PRESENTATION}/include")
target_link_libraries(passport_font_test_codec PRIVATE lvgl)
target_compile_definitions(passport_font_test_codec PRIVATE malloc=passport_font_test_malloc free=passport_font_test_free)
add_executable(test_font_blocks test_font_blocks.c font_heap_cap.c $<TARGET_OBJECTS:passport_font_test_codec>)
target_include_directories(test_font_blocks PRIVATE "${PRESENTATION}/include")
target_compile_options(test_font_blocks PRIVATE -UNDEBUG)
target_link_libraries(test_font_blocks PRIVATE lvgl ZLIB::ZLIB)
add_test(NAME font_blocks COMMAND test_font_blocks)
foreach(target render_ui test_plays_navigation test_game_ui test_ui_licenses)
    target_sources(${target} PRIVATE "${PRESENTATION}/src/passport_font_blocks.c" "${PRESENTATION}/vendor/puff/puff.c")
    target_link_libraries(${target} PRIVATE ZLIB::ZLIB)
endforeach()
if(TARGET render_ui_plain)
    target_sources(render_ui_plain PRIVATE "${PRESENTATION}/src/passport_font_blocks.c" "${PRESENTATION}/vendor/puff/puff.c")
    target_link_libraries(render_ui_plain PRIVATE ZLIB::ZLIB)
endif()

add_executable(test_clicker_ui test_clicker_ui.c)
get_target_property(clicker_sources test_plays_navigation SOURCES)
list(REMOVE_ITEM clicker_sources test_plays_navigation.c)
target_sources(test_clicker_ui PRIVATE ${clicker_sources})
get_target_property(clicker_includes test_plays_navigation INCLUDE_DIRECTORIES)
target_include_directories(test_clicker_ui PRIVATE ${clicker_includes})
target_compile_options(test_clicker_ui PRIVATE -UNDEBUG)
target_link_libraries(test_clicker_ui PRIVATE lvgl m ZLIB::ZLIB)
add_test(NAME clicker_ui COMMAND test_clicker_ui)

add_executable(test_profile_album test_profile_album.c)
get_target_property(profile_sources test_ui_licenses SOURCES)
list(REMOVE_ITEM profile_sources test_ui_licenses.c)
target_sources(test_profile_album PRIVATE ${profile_sources})
get_target_property(profile_includes test_ui_licenses INCLUDE_DIRECTORIES)
target_include_directories(test_profile_album PRIVATE ${profile_includes})
target_compile_options(test_profile_album PRIVATE -UNDEBUG)
target_link_libraries(test_profile_album PRIVATE lvgl m ZLIB::ZLIB)
add_test(NAME profile_album COMMAND test_profile_album)

add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/wake_boot_production.inc"
    COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/extract_wake_boot.py"
        "${REPO}/components/app/src/app.c"
        "${REPO}/components/platform/platform_esp32/src/lvgl_port_setup.c"
        "${CMAKE_CURRENT_BINARY_DIR}/wake_boot_production.inc"
    DEPENDS extract_wake_boot.py "${REPO}/components/app/src/app.c"
        "${REPO}/components/platform/platform_esp32/src/lvgl_port_setup.c")
add_executable(test_wake_boot test_wake_boot.c "${CMAKE_CURRENT_BINARY_DIR}/wake_boot_production.inc")
target_sources(test_wake_boot PRIVATE ${profile_sources} "${PRESENTATION}/src/ui_button_guide.c")
target_include_directories(test_wake_boot PRIVATE ${profile_includes} "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(test_wake_boot PRIVATE -UNDEBUG)
target_link_libraries(test_wake_boot PRIVATE lvgl m ZLIB::ZLIB)
foreach(scenario cold cold_uploaded cold_failed cold_missing factory qr named uploaded failed missing)
    add_test(NAME wake_boot_${scenario} COMMAND test_wake_boot ${scenario})
endforeach()

foreach(target render_ui test_game_ui test_ui_licenses test_profile_album test_wake_boot)
    target_sources(${target} PRIVATE "${PRESENTATION}/src/pixel_sprite.c")
endforeach()
if(TARGET render_ui_plain)
    target_sources(render_ui_plain PRIVATE "${PRESENTATION}/src/pixel_sprite.c")
endif()

foreach(target render_ui test_wake_boot)
    target_sources(${target} PRIVATE "${SERVICES}/src/binding_url.c")
endforeach()
if(TARGET render_ui_plain)
    target_sources(render_ui_plain PRIVATE "${SERVICES}/src/binding_url.c")
endif()

add_executable(test_nav_resume test_nav_resume.c)
target_compile_options(test_nav_resume PRIVATE -UNDEBUG)
target_link_libraries(test_nav_resume PRIVATE lvgl m)
add_test(NAME nav_resume COMMAND test_nav_resume)
add_executable(test_pixel_sprite ../pixel_sprite/test_pixel_sprite.c
    "${PRESENTATION}/src/pixel_sprite.c" "${PRESENTATION}/src/bobo_art.c")
target_include_directories(test_pixel_sprite PRIVATE "${PRESENTATION}/include")
target_compile_options(test_pixel_sprite PRIVATE -UNDEBUG)
target_link_libraries(test_pixel_sprite PRIVATE lvgl m)
add_test(NAME pixel_sprite COMMAND test_pixel_sprite)

# Host tests map the exact same immutable pack before main; only this loader
# uses malloc/file I/O. Native firmware maps the Flash partition directly.
foreach(target render_ui render_ui_plain test_game_ui test_ui_licenses test_profile_album test_pixel_sprite test_wake_boot)
    if(TARGET ${target})
        target_sources(${target} PRIVATE "${PRESENTATION}/src/pet_art_pack.c"
            "${REPO}/tests/host/pixel_sprite/host_pet_art.c")
        target_compile_definitions(${target} PRIVATE PET_ART_TEST_FILE="${REPO}/assets/petart.bin")
    endif()
endforeach()
add_executable(test_pet_art_pack ../pixel_sprite/test_pet_art_pack.c
    "${PRESENTATION}/src/pixel_sprite.c" "${PRESENTATION}/src/pet_art_pack.c" "${PRESENTATION}/src/bobo_art.c")
target_include_directories(test_pet_art_pack PRIVATE "${PRESENTATION}/include")
target_compile_definitions(test_pet_art_pack PRIVATE PET_ART_TEST_FILE="${REPO}/assets/petart.bin")
target_compile_options(test_pet_art_pack PRIVATE -UNDEBUG)
target_link_libraries(test_pet_art_pack PRIVATE lvgl m)
add_test(NAME pet_art_pack COMMAND test_pet_art_pack)

include("${REPO}/tests/host/pixel_sprite/references.cmake")

add_executable(test_xiaozhi_ui test_xiaozhi_ui.c)
set(meter_source "${REPO}/components/platform/platform_esp32/src/audio_es8311.c")
set(voice_worker "${REPO}/components/platform/xiaozhi/xiaozhi.c")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_speaker_meter.c"
    COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/extract_speaker_meter.py" "${meter_source}" "${voice_worker}"
            "${CMAKE_CURRENT_BINARY_DIR}/native_speaker_meter.c"
    DEPENDS extract_speaker_meter.py ../test_ble_advertising.py "${meter_source}" "${voice_worker}")
target_sources(test_xiaozhi_ui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/native_speaker_meter.c")
get_target_property(xz_sources test_plays_navigation SOURCES)
list(REMOVE_ITEM xz_sources test_plays_navigation.c)
target_sources(test_xiaozhi_ui PRIVATE ${xz_sources} "${PRESENTATION}/src/ui_xiaozhi.c")
get_target_property(xz_includes test_plays_navigation INCLUDE_DIRECTORIES)
target_include_directories(test_xiaozhi_ui PRIVATE ${xz_includes} "${REPO}/components/platform/xiaozhi/include")
target_compile_options(test_xiaozhi_ui PRIVATE -UNDEBUG)
target_link_libraries(test_xiaozhi_ui PRIVATE lvgl m ZLIB::ZLIB)
add_test(NAME xiaozhi_ui COMMAND test_xiaozhi_ui)
target_sources(render_ui PRIVATE "${PRESENTATION}/src/ui_xiaozhi.c" "${PRESENTATION}/src/ui_boot.c")
target_include_directories(render_ui PRIVATE "${REPO}/components/platform/xiaozhi/include")

# Actual production overlays and immutable reward ARGB art, with no UI stubs.
add_executable(test_reward_ui test_reward_ui.c "${PRESENTATION}/src/ui_reward.c")
target_include_directories(test_reward_ui PRIVATE "${PRESENTATION}/include")
target_compile_options(test_reward_ui PRIVATE -UNDEBUG)
target_link_libraries(test_reward_ui PRIVATE lvgl m)
add_test(NAME reward_ui COMMAND test_reward_ui)
add_executable(test_factory_ui test_factory_ui.c "${PRESENTATION}/src/ui_factory.c")
target_include_directories(test_factory_ui PRIVATE "${PRESENTATION}/include")
target_compile_options(test_factory_ui PRIVATE -UNDEBUG)
target_link_libraries(test_factory_ui PRIVATE lvgl m)
add_test(NAME factory_ui COMMAND test_factory_ui)
set_tests_properties(reward_ui factory_ui PROPERTIES TIMEOUT 8)

# Every retained rare glyph is decoded by the same production puff path under
# a 512-byte temporary heap cap and compared with independent zlib pixels.
add_executable(test_font_all_glyphs test_font_all_glyphs.c font_heap_cap.c
    "${PRESENTATION}/src/lv_font_cn_24.c" "${PASSPORT_FONT_DIR}/ui_font_title.c" $<TARGET_OBJECTS:passport_font_test_codec>)
target_include_directories(test_font_all_glyphs PRIVATE "${PRESENTATION}/include")
target_compile_options(test_font_all_glyphs PRIVATE -UNDEBUG)
target_link_libraries(test_font_all_glyphs PRIVATE lvgl ZLIB::ZLIB)
add_test(NAME font_all_glyphs COMMAND test_font_all_glyphs)
set_tests_properties(font_blocks font_all_glyphs PROPERTIES TIMEOUT 60)

set(mapped_font_dir "${CMAKE_CURRENT_BINARY_DIR}/font-pack")
execute_process(COMMAND python3 "${REPO}/tools/companion_pack.py" --output "${mapped_font_dir}"
    RESULT_VARIABLE font_pack_result OUTPUT_QUIET)
if(NOT font_pack_result EQUAL 0)
    message(FATAL_ERROR "Native mapped-font pack generation failed")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${REPO}/tools/companion_pack.py" "${REPO}/tools/body_font_pack.py"
    "${PRESENTATION}/src/lv_font_cn_16.c" "${PRESENTATION}/src/lv_font_cn_24.c")
add_executable(test_font_mapped test_font_mapped.c font_heap_cap.c "${PRESENTATION}/src/hangul_font.c"
    "${mapped_font_dir}/lv_font_cn_16.c" "${mapped_font_dir}/lv_font_cn_24.c"
    "${PASSPORT_FONT_DIR}/ui_font_body.c" "${PASSPORT_FONT_DIR}/ui_font_title.c"
    $<TARGET_OBJECTS:passport_font_test_codec>)
target_include_directories(test_font_mapped PRIVATE "${PRESENTATION}/include"
    "${mapped_font_dir}" "${CMAKE_CURRENT_LIST_DIR}/resource_stubs")
target_compile_definitions(test_font_mapped PRIVATE FONT_COMPANION_FILE="${mapped_font_dir}/companion.bin"
    FONT_LANG_FILE="${REPO}/assets/fonts/languages.bin" FONT_BODY_FILE="${mapped_font_dir}/body_font.bin" FONT_ORACLE_FILE="${CMAKE_CURRENT_LIST_DIR}/font_oracle.bin")
target_compile_options(test_font_mapped PRIVATE -UNDEBUG)
target_link_libraries(test_font_mapped PRIVATE lvgl ZLIB::ZLIB)
add_test(NAME font_mapped_all_glyphs COMMAND test_font_mapped)
set_tests_properties(font_mapped_all_glyphs PROPERTIES TIMEOUT 60)

# Exercise the production compact image through LVGL as well as the flash-mapped
# font loader. Generate both variants from the same source as the ESP32 build.
set(mini_pack "${CMAKE_CURRENT_BINARY_DIR}/mini-pack")
execute_process(COMMAND python3 "${REPO}/tools/companion_pack.py" --output "${mini_pack}" --miniapp
    RESULT_VARIABLE mini_pack_result OUTPUT_QUIET)
if(NOT mini_pack_result EQUAL 0)
    message(FATAL_ERROR "Compact resource generation failed")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${REPO}/tools/compact_faces.py")
add_executable(test_font_miniapp test_font_mapped.c font_heap_cap.c "${PRESENTATION}/src/hangul_font.c"
    "${PRESENTATION}/src/compact_face.c" "${mini_pack}/lv_font_cn_16.c" "${mini_pack}/lv_font_cn_24.c"
    "${PASSPORT_FONT_DIR}/ui_font_body.c" "${PASSPORT_FONT_DIR}/ui_font_title.c" $<TARGET_OBJECTS:passport_font_test_codec>)
target_include_directories(test_font_miniapp PRIVATE "${PRESENTATION}/include" "${mini_pack}" "${CMAKE_CURRENT_LIST_DIR}/resource_stubs")
target_compile_definitions(test_font_miniapp PRIVATE PASSPORT_MINIAPP=1 FONT_COMPANION_FILE="${mini_pack}/companion.bin"
    FONT_LANG_FILE="${REPO}/assets/fonts/languages.bin" FONT_BODY_FILE="${mini_pack}/body_font.bin" FONT_ORACLE_FILE="${CMAKE_CURRENT_LIST_DIR}/font_oracle.bin")
target_compile_options(test_font_miniapp PRIVATE -UNDEBUG)
target_link_libraries(test_font_miniapp PRIVATE lvgl ZLIB::ZLIB)
add_test(NAME miniapp_mapped_all_glyphs COMMAND test_font_miniapp)
add_executable(test_compact_faces test_compact_faces.c "${PRESENTATION}/src/compact_face.c")
target_include_directories(test_compact_faces PRIVATE "${PRESENTATION}/include")
target_compile_options(test_compact_faces PRIVATE -UNDEBUG)
target_link_libraries(test_compact_faces PRIVATE lvgl)
add_test(NAME miniapp_face_pixels COMMAND test_compact_faces "${mapped_font_dir}/companion.bin" "${mini_pack}/companion.bin")

# Replay all retained renderer scenes with actual platform rounding and8-row
# double buffers. This closes the old native clipping omission, not MCU timing.
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/platform_display_production.inc"
    COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/extract_display.py"
        "${REPO}/components/platform/platform_esp32/src/lvgl_port_setup.c"
        "${CMAKE_CURRENT_BINARY_DIR}/platform_display_production.inc"
    DEPENDS extract_display.py "${REPO}/components/platform/platform_esp32/src/lvgl_port_setup.c")
get_target_property(platform_render_sources render_ui SOURCES)
get_target_property(platform_render_includes render_ui INCLUDE_DIRECTORIES)
get_target_property(platform_render_defines render_ui COMPILE_DEFINITIONS)
get_target_property(platform_render_libraries render_ui LINK_LIBRARIES)
add_executable(render_ui_platform ${platform_render_sources}
    "${CMAKE_CURRENT_BINARY_DIR}/platform_display_production.inc")
target_include_directories(render_ui_platform PRIVATE ${platform_render_includes}
    "${CMAKE_CURRENT_BINARY_DIR}" "${REPO}/components/platform/platform_esp32/include")
target_compile_definitions(render_ui_platform PRIVATE ${platform_render_defines} PASSPORT_PLATFORM_DISPLAY=1)
target_compile_options(render_ui_platform PRIVATE -UNDEBUG)
target_link_libraries(render_ui_platform PRIVATE ${platform_render_libraries})
add_test(NAME rounded_retained_render COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/run_platform_render.py"
    "$<TARGET_FILE:render_ui_platform>" "${REPO}" "${CMAKE_CURRENT_BINARY_DIR}/rounded-scenes")
set_tests_properties(rounded_retained_render PROPERTIES TIMEOUT 75)
target_sources(test_wake_boot PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/platform_display_production.inc"
    "${PRESENTATION}/src/ui_selftest.c")
target_include_directories(test_wake_boot PRIVATE "${REPO}/components/platform/platform_esp32/include")

add_executable(test_boot_ui test_boot_ui.c "${PRESENTATION}/src/ui_boot.c" "${PRESENTATION}/src/ui_xiaozhi.c" ${xz_sources})
target_include_directories(test_boot_ui PRIVATE ${xz_includes} "${REPO}/components/platform/xiaozhi/include")
target_compile_options(test_boot_ui PRIVATE -UNDEBUG)
target_link_libraries(test_boot_ui PRIVATE lvgl m ZLIB::ZLIB)
add_test(NAME boot_animation COMMAND test_boot_ui)

foreach(official_target test_xiaozhi_ui render_ui render_ui_platform test_boot_ui)
    target_sources(${official_target} PRIVATE official_io.c "${PRESENTATION}/src/ui_official.c"
        "${REPO}/components/platform/xiaozhi/xz_theme_font.c")
endforeach()
