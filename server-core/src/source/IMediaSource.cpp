#include "source/IMediaSource.h"

namespace multiroom {

const char* errorName(MediaSourceError error)
{
    switch (error)
    {
    case MediaSourceError::InvalidArgument:
        return "InvalidArgument";
    case MediaSourceError::NotFound:
        return "NotFound";
    case MediaSourceError::AccessDenied:
        return "AccessDenied";
    case MediaSourceError::OpenFailed:
        return "OpenFailed";
    case MediaSourceError::ReadFailed:
        return "ReadFailed";
    case MediaSourceError::SeekFailed:
        return "SeekFailed";
    case MediaSourceError::NotSeekable:
        return "NotSeekable";
    case MediaSourceError::OutOfRange:
        return "OutOfRange";
    case MediaSourceError::Timeout:
        return "Timeout";
    }
    return "?";
}

} // namespace multiroom
