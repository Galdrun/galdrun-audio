cmake_minimum_required(VERSION 3.16)

project(MultiroomClient
    VERSION 0.1.0
    DESCRIPTION "Multiroom audio client"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(MultiroomWarnings)
include("${CMAKE_CURRENT_SOURCE_DIR}/../shared/branding/Branding.cmake")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MULTIROOM_BUILD_TESTS "Build the unit tests" ON)

# ---------------------------------------------------------------------------
# Shared code. It lives outside this source tree because the server compiles
# the same files, so it needs an explicit binary directory. Both targets pin
# themselves to C++17 -- see shared/json/CMakeLists.txt for why that is not
# negotiable here.
#
# multiroom-matp is the wire format: framing, message types, the sample-format
# table and the audio packet, which the server has to agree with byte for byte.
# It brings audio/AudioFormat.h with it, so this client's own AudioFormat is
# gone rather than duplicated.
#
# multiroom-time is monotonicNowNs(), iso8601UtcNow() and localTimeOfDay(). It
# was this client's util/Time until the server's copy of the same three
# functions was retired in favour of it.
# ---------------------------------------------------------------------------

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../shared/json"
                 "${CMAKE_BINARY_DIR}/shared/json")

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../shared/matp"
                 "${CMAKE_BINARY_DIR}/shared/matp")

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../shared/time"
                 "${CMAKE_BINARY_DIR}/shared/time")

# multiroom-discovery is the beacon sender and the generated id, shared with the
# server core, which announces itself the same way.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../shared/discovery"
                 "${CMAKE_BINARY_DIR}/shared/discovery")

# Single-config generators default to an unoptimized, symbol-less build.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Core library: everything platform-independent, plus the small native helpers
# it needs. Tests link against this, so nothing lives in main.cpp that needs
# testing.
# ---------------------------------------------------------------------------

include(ClientSources)
set(MULTIROOM_CORE_SOURCES ${MULTIROOM_CLIENT_COMMON_SOURCES} src/config/BoardConfig.cpp)

# Exactly one platform backend is compiled in; the other is never seen by the
# compiler, so a Linux-only or Windows-only header can never leak into a build.
if(WIN32)
    list(APPEND MULTIROOM_CORE_SOURCES src/audio/windows/WasapiBackend.cpp)
elseif(UNIX AND NOT APPLE)
    list(APPEND MULTIROOM_CORE_SOURCES src/audio/linux/AlsaBackend.cpp
                                    src/audio/linux/AlsaMixer.cpp
                                    src/audio/linux/Connector3Mcu.cpp
                                    src/audio/linux/AlsaDacSettings.cpp
                                    src/net/NetworkSettings.cpp
                                    src/config/CpuSettings.cpp
                                    src/net/SshSettings.cpp
                                    src/input/linux/LinuxInputBackend.cpp)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../shared/web/EmbedStyle.cmake")

add_library(multiroom_core STATIC ${MULTIROOM_CORE_SOURCES})
target_include_directories(multiroom_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# The backend headers live next to their sources rather than in include/,
# because nothing outside the audio layer may include them.
target_include_directories(multiroom_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_include_directories(multiroom_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(multiroom_core PUBLIC multiroom-json multiroom-matp multiroom-time multiroom-discovery)
target_compile_definitions(multiroom_core PUBLIC MULTIROOM_VERSION="${PROJECT_VERSION}")
multiroom_set_warnings(multiroom_core)
multiroom_brand(multiroom_core PUBLIC)

if(WIN32)
    # WASAPI needs no import library of its own: the interfaces come from COM
    # (ole32) and the GUIDs are defined in WasapiBackend.cpp via initguid.h.
    # ws2_32 is the control server's Winsock2, iphlpapi the server link's interface type.
    target_link_libraries(multiroom_core PUBLIC ole32 avrt ws2_32 iphlpapi)

    # The control server and the command queue run on their own threads.
    find_package(Threads REQUIRED)
    target_link_libraries(multiroom_core PUBLIC Threads::Threads)
elseif(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(multiroom_core PUBLIC Threads::Threads)

    find_package(ALSA REQUIRED)
    target_link_libraries(multiroom_core PUBLIC ALSA::ALSA)
endif()

# ---------------------------------------------------------------------------
# Client executable
# ---------------------------------------------------------------------------

add_executable(multiroom-client src/main.cpp)
set_target_properties(multiroom-client PROPERTIES OUTPUT_NAME ${MULTIROOM_BRAND_SLUG}-client)
target_link_libraries(multiroom-client PRIVATE multiroom_core)
multiroom_set_warnings(multiroom-client)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(MULTIROOM_TARGET)
        set(release_platform "linux-${MULTIROOM_TARGET}")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
        set(release_platform "linux-x64")
    else()
        set(release_platform "linux-${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    set(export_command "${CMAKE_COMMAND}"
            "-DBINARY=$<TARGET_FILE:multiroom-client>"
            "-DBINARY_NAME=$<TARGET_FILE_NAME:multiroom-client>"
            "-DCOMPILER=${CMAKE_CXX_COMPILER}"
            "-DOBJDUMP=${CMAKE_OBJDUMP}"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/release"
            "-DDESTINATION=${CMAKE_CURRENT_SOURCE_DIR}/../releases/client/${release_platform}"
            "-DVERSION=${PROJECT_VERSION}"
            "-DCONFIG=$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExportLinuxClient.cmake")
    # Also export when an IDE builds just the multiroom-client target.
    add_custom_command(TARGET multiroom-client POST_BUILD COMMAND ${export_command} VERBATIM)
    # Runs even on an up-to-date default build, restoring deleted release files.
    add_custom_target(export-client ALL
        COMMAND ${export_command}
        DEPENDS multiroom-client
        COMMENT "Exporting client to releases/client/${release_platform}"
        VERBATIM)
elseif(WIN32)
    string(TOLOWER "windows-${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" release_platform)
    set(release_dir "${CMAKE_CURRENT_SOURCE_DIR}/../releases/client/${release_platform}")
    set(release_files "$<TARGET_FILE:multiroom-client>" "${CMAKE_CURRENT_SOURCE_DIR}/release/client.example.json")
    if(MSVC)
        # The MSVC runtime, app-local, so the target needs no redistributable.
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
        include(InstallRequiredSystemLibraries)
        list(APPEND release_files ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS})
    endif()
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/build-info.txt"
        CONTENT "Version: ${PROJECT_VERSION}\nConfiguration: $<CONFIG>\n")
    set(export_commands
        COMMAND ${CMAKE_COMMAND} -E make_directory "${release_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${release_files}
                "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/build-info.txt" "${release_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/release/README-windows.md" "${release_dir}/README.md")
    add_custom_command(TARGET multiroom-client POST_BUILD ${export_commands} VERBATIM)
    add_custom_target(export-client ALL ${export_commands}
        DEPENDS multiroom-client
        COMMENT "Exporting client to releases/client/${release_platform}"
        VERBATIM)

endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(MULTIROOM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
