#pragma once

// The one place a media URL is taken apart, and the rule every source is held
// to: it comes from the high-end server, over HTTP, from one origin (phase 20).
//
// The core decodes; it does not fetch music. A provider's stream -- tokens
// that expire mid-track, a DASH manifest, encryption -- is the high-end
// server's to turn into one plain HTTP response, and the local library is
// served over the same endpoint. So a `mediaUri` that names a file, another
// scheme or another host is not a source this server was meant to open, and
// is refused before a zone ever holds it.
//
// **The comparison is on parsed parts, never on a string prefix.**
// `http://127.0.0.1:4714.evil.example/` starts with the right characters and
// is another host; `http://127.0.0.1:4714@evil.example/` is another host
// wearing the right one as a user name, which is why user information is
// refused outright. Scheme and host compare case-insensitively, an absent port
// is the scheme's own, and the path must lie under the base's path as whole
// segments -- a `.` or `..` segment, spelled plainly or percent-encoded, is
// refused rather than resolved, because the far end is what would resolve it.
//
// Hosts are compared as written. `localhost` and `127.0.0.1` are two origins
// here: resolving one to see whether it is the other would be a DNS lookup on
// the control API's thread, and the high-end server hands out URLs under the
// base it was configured with anyway.

#include "core/Error.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace multiroom {

enum class MediaOriginError
{
    Malformed,         // not a URL this server can reason about
    UnsupportedScheme, // a path, file:, or anything else that is not http(s)
    Outside            // a well-formed http(s) URL under another origin or path
};

const char* errorName(MediaOriginError error);

// How a fetch from that origin waits, and how often it tries again (phase 22).
//
// `stallTimeoutMs` bounds each operation -- a connect with its headers, one
// read -- and never the transfer as a whole, so a slow but progressing
// download is never killed for its total. A stall past it is treated as a
// dropped connection: resumed when the source is seekable, the source's
// failure when it is not. That is why it has to be shorter than the audio the
// rooms hold ahead of what is audible: a stuck connection is meant to be
// replaced before anybody hears it.
//
// `retries` is how many times in a row one source may be tried again -- a
// 5xx or 429 at open, a drop or a stall mid-stream or on a seek -- before it
// fails. It
// counts consecutive failures and starts again from zero after a read that
// returned bytes, so a long track over a flaky link is not charged for
// every hiccup it ever had.
//
// `token` is phase 23's shared secret: when it is not empty, every request --
// the open, each probe seek, each resume -- carries it as
// `Authorization: Bearer <token>`. It is a secret, so nothing that logs or
// reports a fetch names it; the URL is logged, the header is not.
struct MediaFetchSettings
{
    uint32_t stallTimeoutMs = 2000;
    uint32_t retries = 3;
    std::string token;
};

class MediaOrigin
{
public:
    // `http://127.0.0.1:4714/` -- the high-end server on this machine.
    static constexpr std::string_view kDefaultBaseUrl = "http://127.0.0.1:4714/";

    // A base: http or https, a host, no user information, no query and no
    // fragment. Its path is read as a directory whether or not it ends in a
    // slash, so `/media` holds `/media/a.flac` and not `/media2/a.flac`.
    static Result<MediaOrigin, MediaOriginError> parse(std::string_view baseUrl);

    // Whether `uri` may be fetched: the same scheme, host and port as the
    // base, and a path under the base's. The detail names both the URI and
    // the origin it was held against, because the caller that sent it is code
    // and the fix is on its side.
    [[nodiscard]] Status<MediaOriginError> check(std::string_view uri) const;

    // The base as configured, for a log line and a refusal's detail.
    [[nodiscard]] const std::string& text() const noexcept { return text_; }

    [[nodiscard]] uint16_t port() const noexcept { return port_; }

    // Whether the host is this machine as written: `localhost`, `[::1]` or a
    // dotted 127.x.x.x. Only for phase 23's startup warning about a fetch
    // that leaves the machine without a token -- no name is resolved, so a
    // host name that happens to point here is not loopback.
    [[nodiscard]] bool loopback() const noexcept;

private:
    MediaOrigin() = default;

    std::string text_;
    std::string scheme_; // lower case
    std::string host_;   // lower case, brackets kept for IPv6
    uint16_t port_ = 0;  // explicit, or the scheme's own
    std::string path_;   // always ends in '/'
};

} // namespace multiroom
