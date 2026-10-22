#include "decode/IDecoder.h"

namespace multiroom {

const char* errorName(DecodeError error)
{
    switch (error)
    {
    case DecodeError::AlreadyOpened:
        return "AlreadyOpened";
    case DecodeError::NotOpened:
        return "NotOpened";
    case DecodeError::SourceFailed:
        return "SourceFailed";
    case DecodeError::NoDecoder:
        return "NoDecoder";
    case DecodeError::UnknownContainer:
        return "UnknownContainer";
    case DecodeError::NoAudioStream:
        return "NoAudioStream";
    case DecodeError::UnsupportedCodec:
        return "UnsupportedCodec";
    case DecodeError::UnsupportedFormat:
        return "UnsupportedFormat";
    case DecodeError::InvalidChannels:
        return "InvalidChannels";
    case DecodeError::BufferTooSmall:
        return "BufferTooSmall";
    case DecodeError::DecodeFailed:
        return "DecodeFailed";
    case DecodeError::NotSeekable:
        return "NotSeekable";
    case DecodeError::OffsetOutOfRange:
        return "OffsetOutOfRange";
    case DecodeError::EndOfStream:
        return "EndOfStream";
    }
    return "?";
}

} // namespace multiroom
