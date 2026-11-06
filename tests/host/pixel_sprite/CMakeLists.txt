cmake_minimum_required(VERSION 3.18)
project(pixel_sprite_test LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
get_filename_component(REPO "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(LV_BUILD_CONF_PATH "${REPO}/tests/host/ui/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${REPO}/managed_components/lvgl__lvgl" lvgl-build)
add_executable(test_pixel_sprite test_pixel_sprite.c
    "${REPO}/components/ui/presentation/src/pixel_sprite.c"
    "${REPO}/components/ui/presentation/src/bobo_art.c")
target_include_directories(test_pixel_sprite PRIVATE "${REPO}/components/ui/presentation/include")
target_compile_options(test_pixel_sprite PRIVATE -UNDEBUG)
target_link_libraries(test_pixel_sprite PRIVATE lvgl m)
option(PIXEL_SPRITE_SANITIZE "Instrument the sprite decoder/test with address and undefined-behavior checks" OFF)
set(PIXEL_SPRITE_SANITIZERS "address,undefined" CACHE STRING "Compiler sanitizers for this host")
if(PIXEL_SPRITE_SANITIZE)
    target_compile_options(test_pixel_sprite PRIVATE "-fsanitize=${PIXEL_SPRITE_SANITIZERS}" -fno-omit-frame-pointer)
    target_link_options(test_pixel_sprite PRIVATE "-fsanitize=${PIXEL_SPRITE_SANITIZERS}")
endif()
enable_testing()
add_test(NAME pixel_sprite COMMAND test_pixel_sprite)

target_sources(test_pixel_sprite PRIVATE
    "${REPO}/components/ui/presentation/src/pet_art_pack.c" host_pet_art.c)
target_compile_definitions(test_pixel_sprite PRIVATE PET_ART_TEST_FILE="${REPO}/assets/petart.bin")
add_executable(test_pet_art_pack test_pet_art_pack.c
    "${REPO}/components/ui/presentation/src/pixel_sprite.c"
    "${REPO}/components/ui/presentation/src/pet_art_pack.c"
    "${REPO}/components/ui/presentation/src/bobo_art.c")
target_include_directories(test_pet_art_pack PRIVATE "${REPO}/components/ui/presentation/include")
target_compile_options(test_pet_art_pack PRIVATE -UNDEBUG)
target_compile_definitions(test_pet_art_pack PRIVATE PET_ART_TEST_FILE="${REPO}/assets/petart.bin")
target_link_libraries(test_pet_art_pack PRIVATE lvgl m)
if(PIXEL_SPRITE_SANITIZE)
    target_compile_options(test_pet_art_pack PRIVATE "-fsanitize=${PIXEL_SPRITE_SANITIZERS}" -fno-omit-frame-pointer)
    target_link_options(test_pet_art_pack PRIVATE "-fsanitize=${PIXEL_SPRITE_SANITIZERS}")
endif()
add_test(NAME pet_art_pack COMMAND test_pet_art_pack)

include("${REPO}/tests/host/pixel_sprite/references.cmake")
