namespace Multiroom.Server.Abstractions;

// Where a resolved stream is served to the core. Publish answers the URL a
// source.stage names; Retire ends it once the core is done with that source.
// An interface because the media endpoint (Multiroom.Server.Web) and the queue
// engine that stages its URLs (Multiroom.Server.Playback) do not reference each
// other: the outward API in Web will call the engine.
public interface IMediaPublisher
{
    Uri Publish(ResolvedStream stream);

    void Retire(Uri uri);
}
