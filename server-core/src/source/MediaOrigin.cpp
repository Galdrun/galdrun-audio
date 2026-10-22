#include "source/MediaOrigin.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace multiroom {

const char* errorName(MediaOriginError error)
{
    switch (error)
    {
    case MediaOriginError::Malformed:
        return "Malformed";
    case MediaOriginError::UnsupportedScheme:
        return "UnsupportedScheme";
    case MediaOriginError::Outside:
        return "Outside";
    }
    return "?";
}

namespace {

struct ParsedUrl
{
    std::string scheme;
    std::string host;
    uint16_t port = 0;
    std::string path;
    bool hasQueryOrFragment = false;
};

std::string lowered(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool startsWithNoCase(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && lowered(text.substr(0, prefix.size())) == prefix;
}

Result<ParsedUrl, MediaOriginError> parseUrl(std::string_view url)
{
    const std::string quoted = "\"" + std::string(url) + "\"";

    // A space, a control character or a backslash is something one parser
    // reads one way and the next another -- which is the whole attack a
    // look-alike URL is -- so none of them is given a reading at all.
    for (const char c : url)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte == 0x7f || c == '\\')
            return fail(MediaOriginError::Malformed, quoted + " contains a space, a control "
                                                              "character or a backslash");
    }

    ParsedUrl parsed;
    if (startsWithNoCase(url, "http://"))
        parsed.scheme = "http";
    else if (startsWithNoCase(url, "https://"))
        parsed.scheme = "https";
    else
        return fail(MediaOriginError::UnsupportedScheme, quoted + " is not an http(s) URL");

    std::string_view rest = url.substr(parsed.scheme.size() + 3);
    const size_t authorityEnd = rest.find_first_of("/?#");
    const std::string_view authority = rest.substr(0, authorityEnd);
    rest = authorityEnd == std::string_view::npos ? std::string_view{} : rest.substr(authorityEnd);

    if (authority.find('@') != std::string_view::npos)
        return fail(MediaOriginError::Malformed, quoted + " carries user information");

    std::string_view host;
    std::string_view port;
    if (!authority.empty() && authority.front() == '[')
    {
        const size_t close = authority.find(']');
        if (close == std::string_view::npos)
            return fail(MediaOriginError::Malformed, quoted + " has an unclosed IPv6 host");
        host = authority.substr(0, close + 1);
        const std::string_view after = authority.substr(close + 1);
        if (!after.empty() && after.front() != ':')
            return fail(MediaOriginError::Malformed, quoted + " has text after its IPv6 host");
        port = after.empty() ? after : after.substr(1);
        if (!after.empty() && port.empty())
            return fail(MediaOriginError::Malformed, quoted + " has an empty port");
    }
    else
    {
        const size_t colon = authority.find(':');
        host = authority.substr(0, colon);
        if (colon != std::string_view::npos)
        {
            port = authority.substr(colon + 1);
            if (port.empty())
                return fail(MediaOriginError::Malformed, quoted + " has an empty port");
        }
    }

    if (host.empty() || host == "[]")
        return fail(MediaOriginError::Malformed, quoted + " has no host");
    parsed.host = lowered(host);

    if (port.empty())
    {
        parsed.port = parsed.scheme == "https" ? 443 : 80;
    }
    else
    {
        uint32_t value = 0;
        for (const char c : port)
        {
            if (c < '0' || c > '9' || value > 65535)
                return fail(MediaOriginError::Malformed,
                            quoted + " has a port that is not 1..65535");
            value = value * 10 + static_cast<uint32_t>(c - '0');
        }
        if (value == 0 || value > 65535)
            return fail(MediaOriginError::Malformed, quoted + " has a port that is not 1..65535");
        parsed.port = static_cast<uint16_t>(value);
    }

    const size_t pathEnd = rest.find_first_of("?#");
    parsed.path = std::string(rest.substr(0, pathEnd));
    parsed.hasQueryOrFragment = pathEnd != std::string_view::npos;
    if (parsed.path.empty())
        parsed.path = "/";

    return parsed;
}

// Whether any segment of `path` is `.` or `..` once the encodings a web
// server would decode before resolving it -- `%2e` for the dot, `%2f` and
// `%5c` for a separator -- have been decoded here too.
bool hasDotSegment(std::string_view path)
{
    std::string decoded;
    for (size_t index = 0; index < path.size(); ++index)
    {
        if (path[index] == '%' && index + 2 < path.size())
        {
            const std::string code = lowered(path.substr(index + 1, 2));
            if (code == "2e")
            {
                decoded.push_back('.');
                index += 2;
                continue;
            }
            if (code == "2f" || code == "5c")
            {
                decoded.push_back('/');
                index += 2;
                continue;
            }
        }
        decoded.push_back(path[index]);
    }

    size_t start = 0;
    while (start <= decoded.size())
    {
        const size_t end = std::min(decoded.find('/', start), decoded.size());
        const std::string_view segment = std::string_view(decoded).substr(start, end - start);
        if (segment == "." || segment == "..")
            return true;
        start = end + 1;
    }
    return false;
}

} // namespace

Result<MediaOrigin, MediaOriginError> MediaOrigin::parse(std::string_view baseUrl)
{
    if (baseUrl.empty())
        return fail(MediaOriginError::Malformed, "the media base URL is empty");

    auto parsed = parseUrl(baseUrl);
    if (!parsed)
        return std::unexpected(parsed.error());
    if (parsed->hasQueryOrFragment)
        return fail(MediaOriginError::Malformed,
                    "\"" + std::string(baseUrl) + "\" has a query or fragment, which a base cannot");
    if (hasDotSegment(parsed->path))
        return fail(MediaOriginError::Malformed,
                    "\"" + std::string(baseUrl) + "\" has a . or .. segment");

    MediaOrigin origin;
    origin.text_ = std::string(baseUrl);
    origin.scheme_ = std::move(parsed->scheme);
    origin.host_ = std::move(parsed->host);
    origin.port_ = parsed->port;
    origin.path_ = std::move(parsed->path);
    if (origin.path_.back() != '/')
        origin.path_.push_back('/');
    return origin;
}

Status<MediaOriginError> MediaOrigin::check(std::string_view uri) const
{
    auto parsed = parseUrl(uri);
    if (!parsed)
    {
        return fail(parsed.error().code,
                    parsed.error().detail + ", and media comes only from " + text_);
    }

    const std::string refused = "\"" + std::string(uri) + "\" is not under " + text_;
    if (parsed->scheme != scheme_)
        return fail(MediaOriginError::Outside, refused + " (scheme " + parsed->scheme + ")");
    if (parsed->host != host_)
        return fail(MediaOriginError::Outside, refused + " (host " + parsed->host + ")");
    if (parsed->port != port_)
        return fail(MediaOriginError::Outside,
                    refused + " (port " + std::to_string(parsed->port) + ")");
    if (!parsed->path.starts_with(path_) || hasDotSegment(parsed->path))
        return fail(MediaOriginError::Outside, refused + " (path " + parsed->path + ")");

    return {};
}

bool MediaOrigin::loopback() const noexcept
{
    if (host_ == "localhost" || host_ == "[::1]")
        return true;
    if (!host_.starts_with("127."))
        return false;

    // Four numbers and three dots, or it is a name that starts with 127.
    int dots = 0;
    for (const char c : host_)
    {
        if (c == '.')
            ++dots;
        else if (c < '0' || c > '9')
            return false;
    }
    return dots == 3;
}

} // namespace multiroom
