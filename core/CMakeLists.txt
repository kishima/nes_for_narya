cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(narya_core)

# Bake every .nes file under data/roms/ into the dedicated 'roms' raw
# data partition. ROM data is fetched at runtime via esp_partition_mmap so
# the CPU sees a flash-XIP virtual address (no PSRAM round-trip), which
# keeps the NES core's PPU timing inside cache-friendly bounds.
set(NARYA_ROMS_BIN ${CMAKE_BINARY_DIR}/roms.bin)
file(GLOB NARYA_ROM_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/data/roms/*.nes)

add_custom_command(
    OUTPUT  ${NARYA_ROMS_BIN}
    COMMAND ${PYTHON} ${CMAKE_CURRENT_LIST_DIR}/tools/build_roms.py
            ${CMAKE_CURRENT_LIST_DIR}/data/roms
            ${NARYA_ROMS_BIN}
    DEPENDS ${NARYA_ROM_FILES}
            ${CMAKE_CURRENT_LIST_DIR}/tools/build_roms.py
    COMMENT "Building Narya ROM partition image"
    VERBATIM
)
add_custom_target(narya_roms_image ALL DEPENDS ${NARYA_ROMS_BIN})

# Hook the image into the standard 'idf.py flash' workflow.
esptool_py_flash_to_partition(flash "roms" ${NARYA_ROMS_BIN})
add_dependencies(flash narya_roms_image)
