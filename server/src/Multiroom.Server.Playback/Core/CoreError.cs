namespace Multiroom.Server.Playback.Core;

// CoreLink's error codes. The first seven are the core's own codes on 4713,
// surfaced unchanged: ApiError.From spells them exactly as the core does
// (`invalidArgument`, `notFound`, ...). The rest are this side of the wire.
public enum CoreError
{
    InvalidJson,
    InvalidRequest,
    UnknownCommand,
    InvalidArgument,
    NotFound,
    Failed,
    Unavailable,

    // No connection to the core right now. A desired-state command was
    // recorded anyway and is asserted when the link comes back.
    NotConnected,

    // The connection went away before the answer arrived.
    ConnectionLost,

    // The core answered something this client could not read: not JSON, a
    // missing field, or an error code outside the seven.
    MalformedAnswer,

    // This server's own refusal of what the state forbids: a room that is
    // connected cannot be forgotten (phase 39).
    Conflict,
}
