# Embed the canonical CSS, the shared scripts and the client settings page as
# C++ string literals for offline client pages.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/theme.css"
    "${CMAKE_CURRENT_LIST_DIR}/inputs.js"
    "${CMAKE_CURRENT_LIST_DIR}/input-events.js"
    "${CMAKE_CURRENT_LIST_DIR}/usage-charts.js"
    "${CMAKE_CURRENT_LIST_DIR}/loading.js"
    "${CMAKE_CURRENT_LIST_DIR}/navigation.js")
file(READ "${CMAKE_CURRENT_LIST_DIR}/theme.css" MULTIROOM_WEB_CSS)
# MSVC limits individual literals to 16KiB; adjacent literals preserve the CSS.
string(SUBSTRING "${MULTIROOM_WEB_CSS}" 12000 -1 MULTIROOM_WEB_CSS_TAIL)
string(SUBSTRING "${MULTIROOM_WEB_CSS}" 0 12000 MULTIROOM_WEB_CSS)
configure_file("${CMAKE_CURRENT_LIST_DIR}/MultiroomWebStyle.inc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/MultiroomWebStyle.inc" @ONLY)
file(READ "${CMAKE_CURRENT_LIST_DIR}/inputs.js" MULTIROOM_WEB_INPUTS)
file(READ "${CMAKE_CURRENT_LIST_DIR}/input-events.js" MULTIROOM_WEB_INPUT_EVENTS)
file(READ "${CMAKE_CURRENT_LIST_DIR}/usage-charts.js" MULTIROOM_WEB_USAGE_CHARTS)
file(READ "${CMAKE_CURRENT_LIST_DIR}/loading.js" MULTIROOM_WEB_LOADING)
configure_file("${CMAKE_CURRENT_LIST_DIR}/MultiroomWebInputs.inc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/MultiroomWebInputs.inc" @ONLY)
file(READ "${CMAKE_CURRENT_LIST_DIR}/navigation.js" MULTIROOM_WEB_NAVIGATION)
configure_file("${CMAKE_CURRENT_LIST_DIR}/MultiroomWebNavigation.inc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/MultiroomWebNavigation.inc" @ONLY)

# A source file as adjacent raw string literals, cut at line ends below MSVC's
# 16 KiB per-literal limit. Two markers are replaced: @MULTIROOM_BRAND_NAME@
# by that C macro, and "@INCLUDE <file>@" by an #include of another generated
# fragment. The source must not contain ")<delimiter>\"" and should stay
# ASCII. Written only when changed, so a configure does not force a rebuild.
function(multiroom_embed_text source delimiter output)
    file(READ "${source}" text)
    set(literals "")
    string(LENGTH "${text}" remaining)
    while(remaining GREATER 0)
        set(cut ${remaining})
        if(remaining GREATER 12000)
            string(SUBSTRING "${text}" 0 12000 head)
            string(FIND "${head}" "\n" cut REVERSE)
            if(cut LESS 1)
                set(cut 12000)
            else()
                math(EXPR cut "${cut} + 1")
            endif()
        endif()
        string(SUBSTRING "${text}" 0 ${cut} chunk)
        string(SUBSTRING "${text}" ${cut} -1 text)
        string(APPEND literals "R\"${delimiter}(${chunk})${delimiter}\"\n")
        math(EXPR remaining "${remaining} - ${cut}")
    endwhile()
    string(REPLACE "@MULTIROOM_BRAND_NAME@" ")${delimiter}\" MULTIROOM_BRAND_NAME R\"${delimiter}("
           literals "${literals}")
    string(REGEX REPLACE "@INCLUDE ([A-Za-z0-9_.]+)@" ")${delimiter}\"\n#include \"\\1\"\nR\"${delimiter}("
           literals "${literals}")
    set(target "${CMAKE_CURRENT_BINARY_DIR}/${output}")
    set(previous "")
    if(EXISTS "${target}")
        file(READ "${target}" previous)
    endif()
    if(NOT previous STREQUAL literals)
        file(WRITE "${target}" "${literals}")
    endif()
endfunction()

# The client's one settings page (ControlHttp.cpp serves it, and WebUi.cpp on
# the ESP32): the markup includes the style, the shared scripts and its own.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/client-settings.html"
    "${CMAKE_CURRENT_LIST_DIR}/client-settings.js")
multiroom_embed_text("${CMAKE_CURRENT_LIST_DIR}/client-settings.js" script
    MultiroomClientSettingsScript.inc)
multiroom_embed_text("${CMAKE_CURRENT_LIST_DIR}/client-settings.html" page
    MultiroomClientSettings.inc)
