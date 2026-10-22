# Discovery: the UDP beacon sender and the persistent node id, for the client
# and the server core alike. The client announces itself to the core, the
# core to the high-end server, and both with the same sender -- the interface
# listing in it is the part that took a real network to get right (the
# client's phase 19), and a second copy would have to relearn it.
#
# What each beacon *says* is in shared/matp beside the wire format, so this
# links multiroom-matp for the beacon interval.
#
# C++17 is pinned for the reason given in shared/json: the client compiles
# this with an ARMv6 toolchain.

cmake_minimum_required(VERSION 3.16)

project(MultiroomDiscovery
    VERSION 0.1.0
    DESCRIPTION "Shared discovery beacon sender and node id for the Galdrun client and server core"
    LANGUAGES CXX)

if(NOT TARGET multiroom-matp)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../matp"
                     "${CMAKE_CURRENT_BINARY_DIR}/matp")
endif()

add_library(multiroom-discovery STATIC
    src/BeaconSender.cpp
    src/NodeId.cpp
)

target_include_directories(multiroom-discovery PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(multiroom-discovery PUBLIC multiroom-matp)
if(WIN32)
    # ws2_32 for the socket, iphlpapi to list the interfaces.
    target_link_libraries(multiroom-discovery PRIVATE ws2_32 iphlpapi)
endif()

target_compile_features(multiroom-discovery PUBLIC cxx_std_17)
set_target_properties(multiroom-discovery PROPERTIES CXX_STANDARD 17
                                                  CXX_STANDARD_REQUIRED ON
                                                  CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(multiroom-discovery PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(multiroom-discovery PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
endif()
