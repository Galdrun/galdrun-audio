# How the machine and this process are doing: CPU, memory, uptime and, where
# the board has a sensor, temperature (the server's phase 34, the client's
# phase 20).
#
# It lives here rather than in either source tree because the client and the
# core report the same figures under the same names, and a UI draws them side
# by side: a CPU percentage that meant "of one core" on one side and "of the
# machine" on the other is not a bug either end would notice.
#
# Nothing external: each figure is one OS call or one /proc file.
#
# C++17 is pinned for the same reason it is pinned in shared/json: the client
# targets a 512 MB ARMv6 board with the toolchain that comes with it, and a
# C++20 feature that slips in here still compiles in the server's C++23 build
# and fails in the client's, on the platform that is hardest to test.

cmake_minimum_required(VERSION 3.16)

project(MultiroomSystem
    VERSION 0.1.0
    DESCRIPTION "Shared system metrics for the Galdrun client and server"
    LANGUAGES CXX)

add_library(multiroom-system STATIC src/SystemSampler.cpp src/TextUtil.cpp)

target_include_directories(multiroom-system PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_compile_features(multiroom-system PUBLIC cxx_std_17)
set_target_properties(multiroom-system PROPERTIES CXX_STANDARD 17
                                               CXX_STANDARD_REQUIRED ON
                                               CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(multiroom-system PRIVATE /W4 /permissive- /utf-8)
    # fopen is used deliberately on fixed /proc paths; the "secure" variant is
    # not portable.
    target_compile_definitions(multiroom-system PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(multiroom-system PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
endif()
