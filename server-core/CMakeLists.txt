cmake_minimum_required(VERSION 3.16)

project(MultiroomCore
    VERSION 0.1.0
    DESCRIPTION "Multiroom audio server core"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(MultiroomWarnings)
include("${CMAKE_CURRENT_SOURCE_DIR}/../shared/branding/Branding.cmake")

# C++23 applies to this project's own sources and to nothing else. Anything
# under shared/ pins itself to C++17 because the client compiles it too, and
# the client targets a 512 MB ARMv6 board with the toolchain that comes with
# it. See shared/json/CMakeLists.txt.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MULTIROOM_BUILD_TESTS "Build the unit tests" ON)

# Both resamplers are optional, and the build with both off must still compile
# and pass every test that does not name a real resampler (phases 2 and 3).
option(MULTIROOM_WITH_SOXR "Build the libsoxr resampler" OFF)
option(MULTIROOM_WITH_R8BRAIN "Build the r8brain-free-src resampler" OFF)

# FFmpeg is not in the same category. The two resamplers are alternatives to
# each other, and a build without both is still a server that can serve rooms
# whose format matches their zone's; a build without a decoder cannot play
# anything at all. So this defaults ON, and turning it off is for bringing the
# tree up on a machine that has no FFmpeg yet -- the media sources and their
# tests still build, the decoder and the HTTP source do not.
option(MULTIROOM_WITH_FFMPEG "Build the FFmpeg decoder and the HTTP media source" ON)

# Single-config generators default to an unoptimized, symbol-less build.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Shared code. It lives outside this source tree because the client compiles
# the same files, so it needs an explicit binary directory.
#
# multiroom-matp is the wire format, and it is shared for the reason its own
# CMakeLists gives: a field name or a sample-format code that differs between
# the two ends of the socket is not a bug either end can find alone. It brings
# audio/AudioFormat.h with it, which is why this project no longer has one --
# the two were the same type written twice, and the server's copy is the one
# that went away.
#
# multiroom-time is the monotonic clock and the two wall-clock strings, which
# both halves had written. The client's copy is the one that moved (phase 30);
# IClock and SystemClock stay here, and SystemClock calls it.
# ---------------------------------------------------------------------------

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../shared/json"
                 "${CMAKE_BINARY_DIR}/shared/json")

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../shared/matp"
                 "${CMAKE_BINARY_DIR}/shared/matp")

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../shared/time"
                 "${CMAKE_BINARY_DIR}/shared/time")

# multiroom-discovery is the client's beacon sender and generated id, which this
# core announces itself to the high-end server with.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../shared/discovery"
                 "${CMAKE_BINARY_DIR}/shared/discovery")

# ---------------------------------------------------------------------------
# Vendored resampler libraries. Each target only exists when its option is on,
# so the default build configures neither and compiles neither.
# ---------------------------------------------------------------------------

add_subdirectory(third_party)

# ---------------------------------------------------------------------------
# Core library: everything the server does. Tests link against this, so nothing
# lives in main.cpp that needs testing.
# ---------------------------------------------------------------------------

set(MULTIROOM_SERVER_SOURCES
    src/app/CommandLine.cpp
    src/audio/AudioBlock.cpp
    src/audio/BlockPool.cpp
    src/audio/NativePcmStore.cpp
    src/client/ClientClockModel.cpp
    src/client/ClientOutput.cpp
    src/client/ClientRegistry.cpp
    src/client/ClientSession.cpp
    src/client/ClientSyncController.cpp
    src/client/FormatMatch.cpp
    src/client/IPacketSink.cpp
    src/client/OutputClock.cpp
    src/client/OutputPacketizer.cpp
    src/client/OutputPlan.cpp
    src/client/OutputPlanner.cpp
    src/config/ServerConfig.cpp
    src/control/ControlApi.cpp
    src/control/ControlListener.cpp
    src/control/RoomDirectory.cpp
    src/discovery/ClientControl.cpp
    src/discovery/DiscoveredClients.cpp
    src/discovery/DiscoveryListener.cpp
    src/core/Ids.cpp
    src/core/Units.cpp
    src/decode/DecoderFactory.cpp
    src/decode/IDecoder.cpp
    src/matp/AudioServer.cpp
    src/matp/ControlServer.cpp
    src/matp/RoomCommand.cpp
    src/matp/SocketPacketSink.cpp
    src/net/Socket.cpp
    src/processing/DirectPcmProcessor.cpp
    src/processing/Equalizer.cpp
    src/processing/IClientAudioProcessor.cpp
    src/processing/ResamplingProcessor.cpp
    src/processing/SampleConvert.cpp
    src/resampler/BuiltinResamplers.cpp
    src/resampler/FakeResampler.cpp
    src/resampler/IAudioResampler.cpp
    src/resampler/RatioText.cpp
    src/resampler/ResamplerRegistry.cpp
    src/source/FileMediaSource.cpp
    src/source/IMediaSource.cpp
    src/source/MediaOrigin.cpp
    src/source/MemoryMediaSource.cpp
    src/timeline/SourceTimeline.cpp
    src/util/Clock.cpp
    src/util/FileUtil.cpp
    src/util/Logger.cpp
    src/zone/StartScheduler.cpp
    src/zone/Zone.cpp
    src/zone/ZoneManager.cpp
)

add_library(multiroom_server_core STATIC ${MULTIROOM_SERVER_SOURCES})

target_include_directories(multiroom_server_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# A header only one layer may use lives next to its source rather than in
# include/, and reaches it through this. That is what keeps the FFmpeg, soxr
# and r8brain headers of the later phases from leaking upward.
target_include_directories(multiroom_server_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(multiroom_server_core PUBLIC multiroom-json multiroom-matp multiroom-time multiroom-discovery)
target_compile_definitions(multiroom_server_core PUBLIC
    MULTIROOM_SERVER_VERSION="${PROJECT_VERSION}")
multiroom_set_warnings(multiroom_server_core)
multiroom_brand(multiroom_server_core PUBLIC)

# An adapter is compiled into the core library only when its library is built.
# The definition is PUBLIC because BuiltinResamplers.cpp is not the only place
# that has to agree about which implementations exist: the tests decide which
# cases of the shared test body apply the same way.
if(MULTIROOM_WITH_SOXR)
    target_compile_definitions(multiroom_server_core PUBLIC MULTIROOM_WITH_SOXR=1)
    target_sources(multiroom_server_core PRIVATE src/resampler/soxr/SoxrResampler.cpp)
    target_link_libraries(multiroom_server_core PRIVATE multiroom-soxr)
endif()
if(MULTIROOM_WITH_R8BRAIN)
    target_compile_definitions(multiroom_server_core PUBLIC MULTIROOM_WITH_R8BRAIN=1)
    target_sources(multiroom_server_core PRIVATE src/resampler/r8brain/R8BrainResampler.cpp)
    target_link_libraries(multiroom_server_core PRIVATE multiroom-r8brain)
endif()

# The decoder and the HTTP source are the only two files that may see an FFmpeg
# header, which is why the link is PRIVATE and the include directory never
# reaches include/. The definition is PUBLIC because the tests have to agree
# with the library about which of them exist, exactly as for the resamplers.
if(MULTIROOM_WITH_FFMPEG)
    target_compile_definitions(multiroom_server_core PUBLIC MULTIROOM_WITH_FFMPEG=1)
    target_sources(multiroom_server_core PRIVATE
        src/decode/ffmpeg/FfmpegDecoder.cpp
        src/source/http/HttpMediaSource.cpp)
    target_link_libraries(multiroom_server_core PRIVATE multiroom-ffmpeg)
endif()

# A shared FFmpeg needs its DLLs beside whatever runs, and on Windows there is
# no rpath to arrange that. The vendored drop is shared on purpose -- that is
# what satisfies the LGPL's relinking obligation -- so the copy is part of the
# build rather than something a developer has to remember.
function(multiroom_copy_ffmpeg_runtime target)
    if(NOT MULTIROOM_FFMPEG_RUNTIME_DIR)
        return()
    endif()
    file(GLOB _multiroom_ffmpeg_dlls "${MULTIROOM_FFMPEG_RUNTIME_DIR}/*.dll")
    foreach(_dll IN LISTS _multiroom_ffmpeg_dlls)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_dll}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endforeach()
endfunction()

find_package(Threads REQUIRED)
target_link_libraries(multiroom_server_core PUBLIC Threads::Threads)

# Winsock2, for the MATP listeners. PRIVATE like every other library that only
# src/net/Socket.h may see: nothing under include/ pulls a socket header in.
if(WIN32)
    target_link_libraries(multiroom_server_core PRIVATE ws2_32)
endif()

# ---------------------------------------------------------------------------
# Server executable
# ---------------------------------------------------------------------------

add_executable(multiroom-core src/main.cpp)
set_target_properties(multiroom-core PROPERTIES OUTPUT_NAME ${MULTIROOM_BRAND_SLUG}-core)
target_link_libraries(multiroom-core PRIVATE multiroom_server_core)
multiroom_set_warnings(multiroom-core)
multiroom_copy_ffmpeg_runtime(multiroom-core)

# `--service` (phase 29). Only main() calls it, so it is the executable's and
# not the library's; systemd needs no code, so there is no POSIX half.
if(WIN32)
    target_sources(multiroom-core PRIVATE src/app/WindowsService.cpp)
    target_link_libraries(multiroom-core PRIVATE advapi32)
endif()

# ---------------------------------------------------------------------------
# Install (phase 29): where a service expects to find the server
# ---------------------------------------------------------------------------

include(GNUInstallDirs)
install(TARGETS multiroom-core RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND MULTIROOM_WITH_SOXR)
    install(TARGETS multiroom-soxr LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
    set_target_properties(multiroom-core PROPERTIES INSTALL_RPATH "${CMAKE_INSTALL_FULL_LIBDIR}")
endif()

# The shared FFmpeg goes beside the binary for the same reason the build
# copies it there: Windows has no rpath. On Linux FFmpeg is the system's.
if(MULTIROOM_FFMPEG_RUNTIME_DIR)
    file(GLOB _multiroom_ffmpeg_install_dlls "${MULTIROOM_FFMPEG_RUNTIME_DIR}/*.dll")
    install(FILES ${_multiroom_ffmpeg_install_dlls} DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# The systemd unit is installed by `cmake --install`; it has not been run as a service yet.
if(UNIX AND NOT APPLE)
    configure_file(packaging/core.service.in
                   "${CMAKE_CURRENT_BINARY_DIR}/${MULTIROOM_BRAND_SLUG}-core.service" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${MULTIROOM_BRAND_SLUG}-core.service"
            DESTINATION lib/systemd/system)
endif()

# ---------------------------------------------------------------------------
# Release export: every build copies the server and the DLLs it loads to
# releases/server-core/<platform>, named like the client's presets.
# ---------------------------------------------------------------------------

if(WIN32)
    string(TOLOWER "windows-${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" release_platform)
elseif(MULTIROOM_TARGET)
    set(release_platform "linux-${MULTIROOM_TARGET}")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(release_platform "linux-x64")
else()
    set(release_platform "linux-${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(release_dir "${CMAKE_CURRENT_SOURCE_DIR}/../releases/server-core/${release_platform}")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(MULTIROOM_FFMPEG_ROOT)
        target_link_options(multiroom_server_core INTERFACE "-Wl,-rpath-link,${MULTIROOM_FFMPEG_ROOT}/lib")
    endif()
    set(soxr_runtime "")
    if(MULTIROOM_WITH_SOXR)
        set(soxr_runtime "$<TARGET_FILE:multiroom-soxr>")
    endif()
    set(export_command "${CMAKE_COMMAND}"
        "-DBINARY=$<TARGET_FILE:multiroom-core>"
        "-DBINARY_NAME=$<TARGET_FILE_NAME:multiroom-core>"
        "-DSOXR=${soxr_runtime}"
        "-DCOMPILER=${CMAKE_CXX_COMPILER}" "-DOBJDUMP=${CMAKE_OBJDUMP}"
        "-DFFMPEG=${MULTIROOM_FFMPEG_ROOT}" "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DDESTINATION=${release_dir}" "-DVERSION=${PROJECT_VERSION}" "-DCONFIG=$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExportLinuxServer.cmake")
    add_custom_command(TARGET multiroom-core POST_BUILD COMMAND ${export_command} VERBATIM)
    add_custom_target(export-server ALL COMMAND ${export_command} DEPENDS multiroom-core
        COMMENT "Exporting ServerCore to releases/server-core/${release_platform}" VERBATIM)
else()
    set(release_files "$<TARGET_FILE:multiroom-core>")
    if(MULTIROOM_FFMPEG_RUNTIME_DIR)
        list(APPEND release_files ${_multiroom_ffmpeg_install_dlls})
    endif()
    if(MSVC)
        # The MSVC runtime, app-local, so the target needs no redistributable.
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
        include(InstallRequiredSystemLibraries)
        list(APPEND release_files ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS})
    endif()
    if(UNIX AND NOT APPLE)
        list(APPEND release_files "${CMAKE_CURRENT_BINARY_DIR}/${MULTIROOM_BRAND_SLUG}-core.service")
    endif()

    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/build-info.txt"
        CONTENT "Version: ${PROJECT_VERSION}\nConfiguration: $<CONFIG>\n")
    set(export_commands
        COMMAND ${CMAKE_COMMAND} -E make_directory "${release_dir}/licenses"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${release_files}
                "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/build-info.txt"
                "${CMAKE_CURRENT_SOURCE_DIR}/release/server.example.json" "${release_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/release/README-windows.md" "${release_dir}/README.md")
    # Notices of everything linked in, named like the Linux export's.
    set(notices "")
    if(MULTIROOM_FFMPEG_RUNTIME_DIR)
        list(APPEND notices ffmpeg/LICENSE.txt)
    endif()
    if(MULTIROOM_WITH_SOXR)
        list(APPEND notices soxr/LICENCE)
    endif()
    if(MULTIROOM_WITH_R8BRAIN)
        list(APPEND notices r8brain/LICENSE)
    endif()
    foreach(notice IN LISTS notices)
        get_filename_component(dependency "${notice}" DIRECTORY)
        list(APPEND export_commands
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${notice}"
                    "${release_dir}/licenses/${dependency}-LICENSE.txt")
    endforeach()
    add_custom_command(TARGET multiroom-core POST_BUILD ${export_commands} VERBATIM)
    add_custom_target(export-server ALL ${export_commands}
        DEPENDS multiroom-core
        COMMENT "Exporting ServerCore to releases/server-core/${release_platform}"
        VERBATIM)

endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(MULTIROOM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
