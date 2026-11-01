namespace Multiroom.Server.Providers.Streaming;

// Where a streamed track's pieces are held while it plays.
public enum TrackBuffer
{
    Memory,
    Disk,
}

// The `streaming` section, resolved: one window for every on-demand
// streaming provider. `Directory` is <dataDirectory>/streams; each provider
// holds its disk pieces in <Directory>/<providerId>.
public sealed record StreamingSettings(TrackBuffer Buffer, long WindowBytes, string Directory)
{
    public const long DefaultWindowBytes = 16L * 1024 * 1024;
    public const long MinimumWindowBytes = 4L * 1024 * 1024;

    // 0: the whole track, downloaded and kept until it is released.
    public bool Whole(long length) => WindowBytes == 0 || WindowBytes >= length;
}
