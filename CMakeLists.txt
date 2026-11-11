# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# components/ 按架构分层分组存放,ESP-IDF 默认只扫描 components/ 的直接子目录,
# 因此显式把各分组目录加入组件搜索路径。组件名仍是各叶子目录名(ports/services/
# presentation/platform_esp32/app),REQUIRES 依赖链不受影响。
set(EXTRA_COMPONENT_DIRS
    components/core
    components/ui
    components/platform
    components/app)

# 固件版本(供 release.py 命名与 esp_app_desc 使用)。发版时按需递增。
set(PROJECT_VER "2.1.0-xiaozhi.22")

option(PASSPORT_MINIAPP "Build for the FoloToy mini-program Recovery installer" OFF)
if(PASSPORT_MINIAPP)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.miniapp.defaults")
else()
    set(BOOTLOADER_IGNORE_EXTRA_COMPONENT recovery_boot_hook)
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(trae_card)
if(PASSPORT_MINIAPP)
    if(NOT CONFIG_PARTITION_TABLE_CUSTOM_FILENAME STREQUAL "partitions-miniapp.csv")
        message(FATAL_ERROR "The mini-program build needs a separate SDKCONFIG with partitions-miniapp.csv")
    endif()
    idf_build_set_property(COMPILE_DEFINITIONS PASSPORT_MINIAPP=1 APPEND)
elseif(CONFIG_PARTITION_TABLE_CUSTOM_FILENAME STREQUAL "partitions-miniapp.csv")
    message(FATAL_ERROR "Use PASSPORT_MINIAPP with the mini-program partition table")
endif()

# The console accepts 3 or 4 numeric components. Keep the board edition in
# the package name and use the same release numbers for every cloud report.
string(REPLACE "-xiaozhi." "." xiaozhi_console_version "${PROJECT_VER}")
if(NOT xiaozhi_console_version MATCHES "^[0-9]+[.][0-9]+[.][0-9]+([.][0-9]+)?$")
    message(FATAL_ERROR "Invalid XiaoZhi console version derived from ${PROJECT_VER}")
endif()
idf_component_get_property(xiaozhi_lib xiaozhi COMPONENT_LIB)
target_compile_definitions(${xiaozhi_lib} PRIVATE XZ_CONSOLE_VERSION="${xiaozhi_console_version}")

# esp_codec_dev 1.5.10 ignores DMA-format failure during open and can start
# freed descriptors. Build two reviewed, hash-pinned corrections without
# modifying the dependency cache or the installed ESP-IDF SDK.
idf_component_get_property(codec_device_lib espressif__esp_codec_dev COMPONENT_LIB)
idf_component_get_property(codec_device_dir espressif__esp_codec_dev COMPONENT_DIR)
set(codec_i2s_fixed "${CMAKE_BINARY_DIR}/audio_codec_data_i2s_fixed.c")
set(codec_open_fixed "${CMAKE_BINARY_DIR}/esp_codec_dev_fixed.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_DIR}/tools/patch_codec_i2s.py"
    "${codec_device_dir}/platform/audio_codec_data_i2s.c"
    "${codec_device_dir}/esp_codec_dev.c")
execute_process(COMMAND "${PYTHON}" "${PROJECT_DIR}/tools/patch_codec_i2s.py"
    --input "${codec_device_dir}/platform/audio_codec_data_i2s.c"
    --output "${codec_i2s_fixed}"
    --device-input "${codec_device_dir}/esp_codec_dev.c"
    --device-output "${codec_open_fixed}"
    RESULT_VARIABLE codec_patch_result)
if(NOT codec_patch_result EQUAL 0)
    message(FATAL_ERROR "Pinned codec failure-handling patch failed")
endif()
get_target_property(codec_device_sources ${codec_device_lib} SOURCES)
set(codec_device_original_sources ${codec_device_sources})
list(FILTER codec_device_sources EXCLUDE REGEX "(^|/)(audio_codec_data_i2s|esp_codec_dev)\\.c$")
list(LENGTH codec_device_original_sources codec_device_before)
list(LENGTH codec_device_sources codec_device_after)
math(EXPR codec_device_replaced "${codec_device_before} - ${codec_device_after}")
if(NOT codec_device_replaced EQUAL 2)
    message(FATAL_ERROR "Expected exactly two codec sources to replace")
endif()
set_property(TARGET ${codec_device_lib} PROPERTY SOURCES ${codec_device_sources})
target_sources(${codec_device_lib} PRIVATE "${codec_i2s_fixed}" "${codec_open_fixed}")
target_include_directories(${codec_device_lib} PRIVATE "${codec_device_dir}")

# Built-in named avatars are immutable release resources.  Generate the raw
# partition image as part of every normal build and register it with `flash`.
partition_table_get_partition_info(imgava_offset "--partition-name imgava" "offset")
partition_table_get_partition_info(imgava_size "--partition-name imgava" "size")
set(imgava_bin "${CMAKE_BINARY_DIR}/imgava.bin")
set(avatar_catalog "${CMAKE_BINARY_DIR}/avatar_catalog.json")
file(GLOB avatar_sources CONFIGURE_DEPENDS "${PROJECT_DIR}/assets/avatars/*.png")
add_custom_command(
    OUTPUT "${imgava_bin}" "${avatar_catalog}"
    COMMAND "${PYTHON}" "${PROJECT_DIR}/tools/avatar_pack.py"
            --input "${PROJECT_DIR}/assets/avatars"
            --output "${imgava_bin}"
            --catalog-output "${avatar_catalog}"
            --size "${imgava_size}"
    DEPENDS
        "${PROJECT_DIR}/tools/avatar_pack.py"
        ${avatar_sources}
    VERBATIM)
add_custom_target(avatar_pack_bin ALL DEPENDS "${imgava_bin}" "${avatar_catalog}")
esptool_py_flash_target_image(flash imgava "${imgava_offset}" "${imgava_bin}")
add_dependencies(flash avatar_pack_bin)

# Shared Dodge/home-avatar PSR1 payloads use the legacy petart name. Existing
# runtime/identity partitions retain their exact deployed addresses and sizes.
partition_table_get_partition_info(petart_offset "--partition-name petart" "offset")
partition_table_get_partition_info(petart_size "--partition-name petart" "size")
set(petart_bin "${CMAKE_BINARY_DIR}/petart.bin")
set(petart_catalog "${CMAKE_BINARY_DIR}/petart_catalog.json")
add_custom_command(
    OUTPUT "${petart_bin}" "${petart_catalog}"
    COMMAND "${PYTHON}" "${PROJECT_DIR}/tools/pet_art_pack.py"
            --input "${PROJECT_DIR}/assets/petart.bin"
            --catalog "${PROJECT_DIR}/assets/petart_catalog.json"
            --output "${petart_bin}" --catalog-output "${petart_catalog}"
            --size "${petart_size}"
            --body-font "${CMAKE_BINARY_DIR}/companion/body_font.bin"
    DEPENDS "${PROJECT_DIR}/tools/pet_art_pack.py"
            "${PROJECT_DIR}/tools/body_font_pack.py" "${CMAKE_BINARY_DIR}/companion/body_font.bin"
            "${PROJECT_DIR}/assets/petart.bin" "${PROJECT_DIR}/assets/petart_catalog.json"
    VERBATIM)
add_custom_target(pet_art_pack_bin ALL DEPENDS "${petart_bin}" "${petart_catalog}")
esptool_py_flash_target_image(flash petart "${petart_offset}" "${petart_bin}")
add_dependencies(flash pet_art_pack_bin)

# Never span the identity/runtime regions with a raw image.
partition_table_get_partition_info(companion_offset "--partition-name companion" "offset")
esptool_py_flash_target_image(flash companion "${companion_offset}" "${CMAKE_BINARY_DIR}/companion/companion.bin")

# esp_audio_codec 2.5 already links absolute imported archives. Its redundant
# raw '-L path' item is split by the linker when the checkout path has spaces.
# Remove only that item; leave the managed dependency and archives untouched.
idf_component_get_property(codec_lib espressif__esp_audio_codec COMPONENT_LIB)
idf_component_get_property(codec_dir espressif__esp_audio_codec COMPONENT_DIR)
foreach(prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(codec_links ${codec_lib} ${prop})
    if(codec_links)
        list(REMOVE_ITEM codec_links "-L ${codec_dir}/lib/${IDF_TARGET}"
             "$<LINK_ONLY:-L ${codec_dir}/lib/${IDF_TARGET}>")
        set_property(TARGET ${codec_lib} PROPERTY ${prop} "${codec_links}")
    endif()
endforeach()
