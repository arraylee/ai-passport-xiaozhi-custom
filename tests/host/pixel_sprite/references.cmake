# Shared by the focused decoder suite and the complete native UI suite.
# Each normal build refreshes references only when pack/manifest/PNG inputs
# change; ctest always compares the entire generated catalog with native PNGs.
# Respect the active shell's Python, matching the asset-generation commands.
# FindPython's newest-version preference can select an unrelated interpreter
# without Pillow even when `python3` in PATH has the required dependency.
find_program(PET_ART_REFERENCE_PYTHON NAMES python3 python REQUIRED)
execute_process(COMMAND "${PET_ART_REFERENCE_PYTHON}" -c "import PIL"
    RESULT_VARIABLE pet_art_pillow_status ERROR_QUIET)
if(NOT pet_art_pillow_status EQUAL 0)
    message(FATAL_ERROR "Native sprite references require Pillow. Configure -DPET_ART_REFERENCE_PYTHON=/path/to/python-with-Pillow")
endif()
set(pixel_sprite_reference_dir "${CMAKE_CURRENT_BINARY_DIR}/pixel-sprite-references")
file(GLOB pixel_sprite_reference_pngs CONFIGURE_DEPENDS "${REPO}/assets/bobo/*.png")
add_custom_command(
    OUTPUT "${pixel_sprite_reference_dir}/references.json"
    COMMAND "${PET_ART_REFERENCE_PYTHON}" "${REPO}/tests/host/pixel_sprite/prepare_references.py"
            "${REPO}/assets/bobo" "${pixel_sprite_reference_dir}"
    DEPENDS "${REPO}/tests/host/pixel_sprite/prepare_references.py"
            "${REPO}/tools/pet_art_pack.py"
            "${REPO}/assets/petart.bin" "${REPO}/assets/petart_catalog.json"
            "${REPO}/assets/pixel-assets.json" ${pixel_sprite_reference_pngs}
    VERBATIM)
add_custom_target(pixel_sprite_references DEPENDS "${pixel_sprite_reference_dir}/references.json")
add_dependencies(test_pixel_sprite pixel_sprite_references)
set_tests_properties(pixel_sprite PROPERTIES
    ENVIRONMENT "PIXEL_SPRITE_REFERENCE_DIR=${pixel_sprite_reference_dir}")
