using System.Buffers.Text;
using System.Collections.Concurrent;
using System.Diagnostics.CodeAnalysis;
using System.Security.Cryptography;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Web.Media;

// The handles behind /media/<handle>. A handle is 128 random bits in base64url:
// opaque, not guessable, and no secret, because the core logs every mediaUri.
// It holds one handle per source the queue engine has staged and the core is
// not yet done with: the engine retires each when the core ends, replaces or
// clears its source.
public sealed class MediaHandles
{
    private readonly ConcurrentDictionary<string, ResolvedStream> _sources = new(StringComparer.Ordinal);

    public int Count => _sources.Count;

    public string Register(ResolvedStream source)
    {
        while (true)
        {
            var handle = Base64Url.EncodeToString(RandomNumberGenerator.GetBytes(16));
            if (_sources.TryAdd(handle, source))
                return handle;
        }
    }

    public bool TryGet(string handle, [NotNullWhen(true)] out ResolvedStream? source) =>
        _sources.TryGetValue(handle, out source);

    // A GET already being served keeps its reader; the next one is 404.
    public void Retire(string handle)
    {
        if (_sources.TryRemove(handle, out var source))
            source.Release();
    }
}
