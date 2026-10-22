#pragma once

// MULTIROOM_VERSION is injected by CMake from the project version.
#ifndef MULTIROOM_VERSION
#  define MULTIROOM_VERSION "0.0.0-dev"
#endif

namespace multiroom {

inline const char* versionString()
{
    return MULTIROOM_VERSION;
}

} // namespace multiroom
