# Vendored resampler libraries, built from source.
#
# Neither upstream build is used. libsoxr's own CMakeLists.txt asks for
# cmake_minimum_required(VERSION 3.1), which CMake 4 refuses outright, and it
# also builds examples, a libsamplerate shim and its own test
# suite -- none of which this server wants. r8brain has no build system at all;
# it is headers.
#
# So each library gets a small target here that compiles exactly the sources
# the adapter in src/resampler/<id>/ needs, with warnings left at the
# compiler's default. multiroom_set_warnings() is deliberately not applied: these
# are not our sources, /W4 -Wpedantic is our standard for our code, and
# patching a dependency to satisfy it is how a vendored tree becomes a fork.
#
# The include directories are marked SYSTEM so that the adapters -- which *are*
# built warning-free at /W4 -- do not inherit diagnostics from soxr.h or
# r8bbase.h.
#
# Licences: libsoxr is LGPL-2.1-or-later (third_party/soxr/LICENCE), r8brain is
# MIT (third_party/r8brain/LICENSE). Linux uses a shared libsoxr; Windows
# retains the static library and its LGPL relinking obligation.

# ---------------------------------------------------------------------------
# libsoxr 0.1.3
# ---------------------------------------------------------------------------

if(MULTIROOM_WITH_SOXR)
    # libsoxr is C, and the project declares LANGUAGES CXX only -- deliberately,
    # since a build without soxr has not a line of C in it. Enabling C here
    # rather than at the top keeps that true.
    enable_language(C)

    include(CheckIncludeFile)
    include(CheckFunctionExists)

    check_include_file(fenv.h HAVE_FENV_H)
    check_include_file(stdbool.h HAVE_STDBOOL_H)
    check_include_file(stdint.h HAVE_STDINT_H)
    check_function_exists(lrint HAVE_LRINT)

    if(CMAKE_C_BYTE_ORDER STREQUAL "BIG_ENDIAN")
        set(HAVE_BIGENDIAN 1)
    else()
        set(HAVE_BIGENDIAN 0)
    endif()

    # The feature set soxr-config.h.in expects, chosen rather than probed.
    #
    # WITH_VR32 is the reason libsoxr is here at all: it is the variable-rate
    # engine that phase 13's drift correction needs. WITH_CR32 and WITH_CR64
    # are the constant-rate engines the fixed conversions use, and fft4g is
    # their DFT -- the pick between fft4g and pffft is a performance question
    # with no measurement behind it yet, and fft4g needs no extra sources.
    #
    # The SIMD variants (CR32S, CR64S) are off: they need per-source
    # -msse/-mavx flags and a runtime dispatch that differs per compiler, and
    # resampling cost has not been measured on anything yet. Turning them on
    # is a later, measured change, not a default.
    set(AVCODEC_FOUND 0)
    set(AVUTIL_FOUND 0)
    set(WITH_PFFFT 0)
    set(WITH_CR32 1)
    set(WITH_CR32S 0)
    set(WITH_CR64 1)
    set(WITH_CR64S 0)
    set(WITH_VR32 1)
    set(WITH_HI_PREC_CLOCK 1)
    set(WITH_FLOAT_STD_PREC_CLOCK 0)
    set(WITH_DEV_TRACE 0)

    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/soxr/soxr-config.h.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/soxr-config.h")

    set(soxr_library_type STATIC)
    if(UNIX AND NOT APPLE)
        set(soxr_library_type SHARED)
    endif()
    add_library(multiroom-soxr ${soxr_library_type}
        soxr/src/soxr.c
        soxr/src/data-io.c
        soxr/src/dbesi0.c
        soxr/src/filter.c
        soxr/src/fft4g64.c
        soxr/src/cr.c
        soxr/src/cr32.c
        soxr/src/fft4g32.c
        soxr/src/cr64.c
        soxr/src/vr32.c
    )

    # soxr.c is C; the project sets CMAKE_CXX_STANDARD 23 and nothing else, so
    # the C standard has to be named here or the compiler's default decides.
    set_target_properties(multiroom-soxr PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        POSITION_INDEPENDENT_CODE ON)
    if(UNIX AND NOT APPLE)
        set_target_properties(multiroom-soxr PROPERTIES VERSION 0.1.3 SOVERSION 0)
    endif()

    # SOXR_LIB is what tells soxr.h it is being compiled, not consumed.
    target_compile_definitions(multiroom-soxr PRIVATE SOXR_LIB)
    if(MSVC)
        target_compile_definitions(multiroom-soxr PRIVATE _USE_MATH_DEFINES
                                                       _CRT_SECURE_NO_WARNINGS)
    endif()

    target_include_directories(multiroom-soxr PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_include_directories(multiroom-soxr SYSTEM PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/soxr/src")

    if(UNIX)
        target_link_libraries(multiroom-soxr PUBLIC m)
    endif()
endif()

# ---------------------------------------------------------------------------
# r8brain-free-src
# ---------------------------------------------------------------------------

if(MULTIROOM_WITH_R8BRAIN)
    # Header-only since upstream removed r8bbase.cpp, so there is nothing to
    # compile: an INTERFACE target carrying the include path is the whole of
    # it. The roadmap calls r8brain "a single .cpp dropped into the tree";
    # upstream has since made that unnecessary, and the reason it was chosen --
    # that a second implementation costs almost nothing -- is only more true.
    add_library(multiroom-r8brain INTERFACE)
    target_include_directories(multiroom-r8brain SYSTEM INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/r8brain")
endif()

# ---------------------------------------------------------------------------
# FFmpeg: libavformat, libavcodec, libavutil
# ---------------------------------------------------------------------------
#
# Unlike the two resamplers this is not built from source. FFmpeg is three
# million lines with its own configure script, and vendoring that build would
# be a second project rather than a dependency -- so the vendored tree under
# ffmpeg/ is a *binary* drop for one platform, and every other platform is
# expected to have the libraries installed.
#
# The lookup order is deliberate, most specific first:
#
#   1. MULTIROOM_FFMPEG_ROOT, if the operator set it -- an SDK laid out as
#      include/ and lib/, which is what every prebuilt package looks like.
#   2. ffmpeg/<platform>/, the vendored drop. Windows x64 only today; the
#      windows-armv8 preset points MULTIROOM_FFMPEG_ROOT at ../ffmpeg/dist.
#   3. pkg-config, which is how every Linux and macOS package manager
#      publishes it.
#
# A build that finds none of them fails here, naming all three, rather than
# failing later with a missing avformat/avformat.h. FFmpeg is not optional to
# what this server does -- a server that cannot decode cannot play anything --
# so MULTIROOM_WITH_FFMPEG defaults ON, which is the one place this dependency is
# treated differently from libsoxr and r8brain.
#
# Licence. The vendored drop is an LGPL-2.1-or-later build of FFmpeg 9.0.2,
# built by ../ffmpeg with only the audio decoders, demuxers and protocols
# (third_party/ffmpeg/LICENSE.txt and README.md), shared rather than static
# precisely so that the LGPL's relinking obligation is met by the DLLs sitting
# beside the executable. GPL-only components are not in it and must not be: a build
# configured with --enable-gpl would make this server's binaries GPL.

if(MULTIROOM_WITH_FFMPEG)
    set(MULTIROOM_FFMPEG_ROOT "" CACHE PATH
        "An FFmpeg SDK laid out as include/ and lib/, if not the vendored or system one")

    set(_multiroom_ffmpeg_root "")
    set(_multiroom_ffmpeg_origin "")

    if(MULTIROOM_FFMPEG_ROOT)
        set(_multiroom_ffmpeg_root "${MULTIROOM_FFMPEG_ROOT}")
        set(_multiroom_ffmpeg_origin "MULTIROOM_FFMPEG_ROOT")
    elseif(WIN32 AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64"
           AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg/win64/lib/avformat.lib")
        set(_multiroom_ffmpeg_root "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg")
        set(_multiroom_ffmpeg_origin "vendored win64")
    endif()

    add_library(multiroom-ffmpeg INTERFACE)

    if(_multiroom_ffmpeg_root)
        # The include directory is shared by every layout; the libraries are
        # under win64/ in the vendored drop and directly under lib/ in an SDK.
        # A shared MinGW build of ../ffmpeg puts its MSVC import libraries
        # beside the DLLs in bin/, so that is searched too.
        set(_multiroom_ffmpeg_include "${_multiroom_ffmpeg_root}/include")
        if(EXISTS "${_multiroom_ffmpeg_root}/win64/lib")
            set(_multiroom_ffmpeg_lib "${_multiroom_ffmpeg_root}/win64/lib")
            set(_multiroom_ffmpeg_bin "${_multiroom_ffmpeg_root}/win64/bin")
        else()
            set(_multiroom_ffmpeg_lib "${_multiroom_ffmpeg_root}/lib")
            set(_multiroom_ffmpeg_bin "${_multiroom_ffmpeg_root}/bin")
        endif()

        foreach(_lib avformat avcodec avutil)
            find_library(MULTIROOM_FFMPEG_${_lib}
                NAMES ${_lib} lib${_lib}
                PATHS "${_multiroom_ffmpeg_lib}" "${_multiroom_ffmpeg_bin}"
                NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
            if(NOT MULTIROOM_FFMPEG_${_lib})
                message(FATAL_ERROR
                    "MULTIROOM_WITH_FFMPEG is on and ${_multiroom_ffmpeg_origin} was used, "
                    "but ${_lib} is not in ${_multiroom_ffmpeg_lib}")
            endif()
            target_link_libraries(multiroom-ffmpeg INTERFACE "${MULTIROOM_FFMPEG_${_lib}}")
        endforeach()

        # SYSTEM for the same reason soxr's and r8brain's are: our code is
        # built at /W4 -Wpedantic and FFmpeg's headers are not written for it.
        target_include_directories(multiroom-ffmpeg SYSTEM INTERFACE "${_multiroom_ffmpeg_include}")

        # Where the DLLs a shared build needs at runtime live, so the targets
        # that link this can copy them next to themselves. Empty for a static
        # or system install, and the copy step then does nothing.
        set(MULTIROOM_FFMPEG_RUNTIME_DIR "" CACHE INTERNAL "" FORCE)
        if(WIN32 AND EXISTS "${_multiroom_ffmpeg_bin}")
            set(MULTIROOM_FFMPEG_RUNTIME_DIR "${_multiroom_ffmpeg_bin}" CACHE INTERNAL "" FORCE)
        endif()

        message(STATUS "FFmpeg: ${_multiroom_ffmpeg_origin} (${_multiroom_ffmpeg_lib})")
    else()
        find_package(PkgConfig QUIET)
        if(NOT PkgConfig_FOUND)
            message(FATAL_ERROR
                "MULTIROOM_WITH_FFMPEG is on but FFmpeg was not found. Set MULTIROOM_FFMPEG_ROOT "
                "to an SDK with include/ and lib/, install the libavformat, libavcodec and "
                "libavutil development packages so pkg-config can find them, or configure "
                "with -DMULTIROOM_WITH_FFMPEG=OFF.")
        endif()

        pkg_check_modules(MULTIROOM_FFMPEG_PC REQUIRED IMPORTED_TARGET
            libavformat libavcodec libavutil)
        target_link_libraries(multiroom-ffmpeg INTERFACE PkgConfig::MULTIROOM_FFMPEG_PC)
        set(MULTIROOM_FFMPEG_RUNTIME_DIR "" CACHE INTERNAL "" FORCE)
        message(STATUS "FFmpeg: pkg-config (libavformat ${MULTIROOM_FFMPEG_PC_libavformat_VERSION})")
    endif()

    unset(_multiroom_ffmpeg_root)
    unset(_multiroom_ffmpeg_origin)
    unset(_multiroom_ffmpeg_include)
    unset(_multiroom_ffmpeg_lib)
    unset(_multiroom_ffmpeg_bin)
endif()
