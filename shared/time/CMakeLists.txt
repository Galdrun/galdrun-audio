# The monotonic clock and the two wall-clock strings both halves use.
#
# It lives here rather than in either source tree because the client and the
# server had the same three functions written twice. The client's copy is the
# one that moved: it is the one that has run on Linux. The server keeps its
# IClock interface -- injected time is the server's rule, and the client has no
# use for it -- and its SystemClock calls monotonicNowNs() from here.
#
# The logger is deliberately not here: the server's formats with std::format,
# which may not cross into C++17.
#
# C++17 is pinned for the same reason it is pinned in shared/json: the client
# targets a 512 MB ARMv6 board with the toolchain that comes with it, and a
# C++20 feature that slips in here still compiles in the server's C++23 build
# and fails in the client's, on the platform that is hardest to test.
# cxx_std_17 as a PUBLIC *feature* is a floor and leaves the server's own
# targets at C++23; the properties pin this target.

cmake_minimum_required(VERSION 3.16)

project(MultiroomTime
    VERSION 0.1.0
    DESCRIPTION "Shared time helpers for the Galdrun client and server"
    LANGUAGES CXX)

add_library(multiroom-time STATIC src/Time.cpp)

target_include_directories(multiroom-time PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_compile_features(multiroom-time PUBLIC cxx_std_17)
set_target_properties(multiroom-time PROPERTIES CXX_STANDARD 17
                                             CXX_STANDARD_REQUIRED ON
                                             CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(multiroom-time PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(multiroom-time PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
endif()
