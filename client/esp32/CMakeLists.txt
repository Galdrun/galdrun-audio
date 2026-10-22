# ESP-IDF project for the client on ESP32 boards. The sources are the client's
# own and the shared ones; only the I2S backend (src/audio/esp32/) and app_main
# are specific to it. See README.md in this directory.
cmake_minimum_required(VERSION 3.16)

# One build per board: boards/<MULTIROOM_BOARD>.defaults sets the chip, its PSRAM
# and the wiring, boards/<MULTIROOM_BOARD>.inputs.json (optional) the buttons a
# first start writes to client.json. Each board keeps its own sdkconfig, so
# boards on different chips build side by side (each in its own -B directory).
set(MULTIROOM_BOARD "esp32-a1s" CACHE STRING "Board to build for: a file name in boards/, without .defaults")
set(board_defaults "${CMAKE_CURRENT_LIST_DIR}/boards/${MULTIROOM_BOARD}.defaults")
if(NOT EXISTS "${board_defaults}")
    message(FATAL_ERROR "No board file ${board_defaults}")
endif()
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${board_defaults}")
set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${MULTIROOM_BOARD}")
# The board file is the source of truth. Defaults only fill what a sdkconfig
# lacks, so one older than them is dropped and generated again: a changed pin
# reaches the build instead of the value the first build stored. A menuconfig
# change lasts until the board file next changes.
foreach(defaults ${SDKCONFIG_DEFAULTS})
    if(EXISTS "${SDKCONFIG}" AND "${defaults}" IS_NEWER_THAN "${SDKCONFIG}")
        file(REMOVE "${SDKCONFIG}")
    endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SDKCONFIG_DEFAULTS})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Embedded under one fixed name whatever the board, so the symbols app_main
# reads do not depend on it; a board without buttons embeds an empty list.
set(board_inputs "${CMAKE_CURRENT_LIST_DIR}/boards/${MULTIROOM_BOARD}.inputs.json")
if(EXISTS "${board_inputs}")
    configure_file("${board_inputs}" "${CMAKE_BINARY_DIR}/board_inputs.json" COPYONLY)
else()
    file(WRITE "${CMAKE_BINARY_DIR}/board_inputs.json" "[]")
endif()
idf_build_set_property(MULTIROOM_BOARD "${MULTIROOM_BOARD}")
idf_build_set_property(MULTIROOM_BOARD_INPUTS "${CMAKE_BINARY_DIR}/board_inputs.json")

include("${CMAKE_CURRENT_LIST_DIR}/../../shared/branding/Branding.cmake")
project(${MULTIROOM_BRAND_SLUG}-client)

# Every build copies the flash images to releases/client/<board>, laid out as
# flash_args names them so esptool can write them from there. The images of
# one chip do not boot on another.
set(release_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../releases/client/${MULTIROOM_BOARD}")
configure_file(release/README.md "${CMAKE_BINARY_DIR}/release/README.md" @ONLY)
add_custom_target(export-client ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${release_dir}/bootloader" "${release_dir}/partition_table"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin"
            "${CMAKE_BINARY_DIR}/flash_args"
            "${CMAKE_BINARY_DIR}/flasher_args.json"
            "${CMAKE_BINARY_DIR}/release/README.md"
            "${release_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/bootloader/bootloader.bin" "${release_dir}/bootloader"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/partition_table/partition-table.bin" "${release_dir}/partition_table"
    DEPENDS app bootloader partition_table_bin
    COMMENT "Exporting client to releases/client/${MULTIROOM_BOARD}"
    VERBATIM)
