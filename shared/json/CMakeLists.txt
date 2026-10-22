# The JSON value, parser and serializer both halves of the project use.
#
# It lives here rather than in either source tree because the client's config
# and control API and the server's config and control API all speak it, and
# MATP's message bodies are JSON documents (the wire code follows in its own
# move, before the server's phase 8).
#
# C++17 is pinned, not preferred. The client targets a 512 MB ARMv6 board with
# the toolchain that comes with it; a C++20 feature that slips in here still
# compiles in the server's C++23 build and fails in the client's, on the
# platform that is hardest to test. cxx_std_17 as a PUBLIC *feature* is a floor
# and leaves the server's own targets at C++23; the properties pin this target.

cmake_minimum_required(VERSION 3.16)

project(MultiroomJson
    VERSION 0.1.0
    DESCRIPTION "Shared JSON for the Galdrun client and server"
    LANGUAGES CXX)

add_library(multiroom-json STATIC src/Json.cpp)

target_include_directories(multiroom-json PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_compile_features(multiroom-json PUBLIC cxx_std_17)
set_target_properties(multiroom-json PROPERTIES CXX_STANDARD 17
                                             CXX_STANDARD_REQUIRED ON
                                             CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(multiroom-json PRIVATE /W4 /permissive- /utf-8)
    # strtod/snprintf are used deliberately; the "secure" variants are not
    # portable.
    target_compile_definitions(multiroom-json PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(multiroom-json PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
endif()
