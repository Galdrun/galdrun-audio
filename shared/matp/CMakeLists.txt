# The MATP wire format: framing, message types, the sample-format table, the
# binary audio packet, and both directions' make/parse.
#
# It lives here rather than in either source tree because the client and the
# server are the two ends of the same socket. A message id, a field name or a
# sample-format code that differs between them is not a bug either side can
# find on its own, and the client's copy is the one that has been verified
# against real hardware -- so the server compiles those same files rather than
# a second implementation of them.
#
# What is *not* here: the state machines. The client keeps Session,
# AudioStream, ClockModel and TimelineMap; the server keeps its own. Only the
# bytes are shared.
#
# C++17 is pinned for the same reason it is pinned in shared/json: the client
# targets a 512 MB ARMv6 board with the toolchain that comes with it, and a
# C++20 feature that slips in here still compiles in the server's C++23 build
# and fails in the client's, on the platform that is hardest to test.
# cxx_std_17 as a PUBLIC *feature* is a floor and leaves the server's own
# targets at C++23; the properties pin this target.

cmake_minimum_required(VERSION 3.16)

project(MultiroomMatp
    VERSION 0.1.0
    DESCRIPTION "Shared MATP wire format for the Galdrun client and server"
    LANGUAGES CXX)

# multiroom-json may already have been added by whichever project pulled us in --
# both halves need it in their own right -- so this is guarded rather than
# unconditional.
if(NOT TARGET multiroom-json)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../json"
                     "${CMAKE_CURRENT_BINARY_DIR}/json")
endif()

# The same for multiroom-system: SYSTEM_REPORT (section 55) carries its
# SystemMetrics.
if(NOT TARGET multiroom-system)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../system"
                     "${CMAKE_CURRENT_BINARY_DIR}/system")
endif()

add_library(multiroom-matp STATIC
    src/AudioClockSnapshot.cpp
    src/AudioFormat.cpp
    src/AudioPacket.cpp
    src/ClientBeacon.cpp
    src/CoreBeacon.cpp
    src/Input.cpp
    src/Protocol.cpp
)

target_include_directories(multiroom-matp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(multiroom-matp PUBLIC multiroom-json multiroom-system)

target_compile_features(multiroom-matp PUBLIC cxx_std_17)
set_target_properties(multiroom-matp PROPERTIES CXX_STANDARD 17
                                             CXX_STANDARD_REQUIRED ON
                                             CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(multiroom-matp PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(multiroom-matp PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
endif()
