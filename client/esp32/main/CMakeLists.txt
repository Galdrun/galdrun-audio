# The client and shared sources compiled as one component, with the board's
# own start-up and settings page: the common client sources
# (../../cmake/ClientSources.cmake) plus the ESP32 backends.
set(client "${CMAKE_CURRENT_LIST_DIR}/../..")
set(shared "${client}/../shared")

include("${client}/cmake/ClientSources.cmake")
list(TRANSFORM MULTIROOM_CLIENT_COMMON_SOURCES PREPEND "${client}/")

include("${shared}/web/EmbedStyle.cmake")
include("${shared}/branding/Branding.cmake")

idf_component_register(
    SRCS
        app_main.cpp
        CaptiveDns.cpp
        WebUi.cpp
        WifiSettings.cpp
        "${client}/src/main.cpp"
        ${MULTIROOM_CLIENT_COMMON_SOURCES}
        "${client}/src/app/esp32/TaskReport.cpp"
        "${client}/src/audio/esp32/Es8388.cpp"
        "${client}/src/audio/esp32/I2sBackend.cpp"
        "${client}/src/input/esp32/Esp32InputBackend.cpp"
        "${shared}/json/src/Json.cpp"
        "${shared}/matp/src/AudioClockSnapshot.cpp"
        "${shared}/matp/src/AudioFormat.cpp"
        "${shared}/matp/src/AudioPacket.cpp"
        "${shared}/matp/src/ClientBeacon.cpp"
        "${shared}/matp/src/CoreBeacon.cpp"
        "${shared}/matp/src/Input.cpp"
        "${shared}/matp/src/Protocol.cpp"
        "${shared}/system/src/SystemSampler.cpp"
        "${shared}/system/src/TextUtil.cpp"
        "${shared}/time/src/Time.cpp"
        "${shared}/discovery/src/BeaconSender.cpp"
        "${shared}/discovery/src/NodeId.cpp"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_BINARY_DIR}"
        "${client}/include"
        "${client}/src"
        "${shared}/json/include"
        "${shared}/matp/include"
        "${shared}/system/include"
        "${shared}/time/include"
        "${shared}/discovery/include"
    PRIV_REQUIRES
        esp_driver_gpio esp_driver_i2c esp_driver_pcnt esp_driver_i2s esp_event esp_http_server esp_netif esp_timer
        esp_wifi nvs_flash pthread littlefs)

# The ESP-IDF build already runs -Wall -Werror=all; these are the rest of what
# cmake/MultiroomWarnings.cmake applies on the other platforms.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Wshadow -Wnon-virtual-dtor)

# One version for every platform: the one project() declares in the client.
file(READ "${client}/CMakeLists.txt" clientCMake)
string(REGEX MATCH "VERSION ([0-9]+\\.[0-9]+\\.[0-9]+)" unused "${clientCMake}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE MULTIROOM_VERSION="${CMAKE_MATCH_1}")
multiroom_brand(${COMPONENT_LIB} PRIVATE)

# The board this build is for (../CMakeLists.txt) and the buttons a first
# start writes to client.json (app_main).
idf_build_get_property(board MULTIROOM_BOARD)
idf_build_get_property(boardInputs MULTIROOM_BOARD_INPUTS)
target_compile_definitions(${COMPONENT_LIB} PRIVATE MULTIROOM_BOARD="${board}")
target_add_binary_data(${COMPONENT_LIB} "${boardInputs}" TEXT)
