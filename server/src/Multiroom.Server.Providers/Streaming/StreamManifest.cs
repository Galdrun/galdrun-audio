using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Providers.Streaming;

// Where a streamed track's bytes are, as a provider learns it from its
// service: one URL (fetched in 1 MiB ranges when its server takes ranges) or
// a list of segment URLs that concatenated are the track. The URLs are
// usually signed: they go in no message and no log line (rule 7).
//
// `Format` is what the bytes are, when the service says so on resolving;
// `Tag` names that grant in the disk store's directory name (Tidal: the
// tier it granted).
public sealed record StreamManifest(string ContentType, IReadOnlyList<Uri> Pieces, AudioFormat? Format = null,
    string Tag = "");
