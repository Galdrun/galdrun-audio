namespace Multiroom.Server.Playback;

// The house's playback settings as answered and kept in playback.json.
// GroupBooks: a node of audiobook tracks (two or more) goes into a queue or a
// playlist as one item with parts (phase 31); off, as single tracks, and a
// stored book is queued part by part. ResumeBooks: a book queued by any path
// starts where a listener last stopped in it (phase 33); off, from its
// beginning. Its place is saved either way. FindRadioLikes: a song liked as
// a radio played it is looked for among the providers that stream tracks
// (RadioLikeFinder); off, it is not, and what was found stays.
public sealed record PlaybackSettingsView(bool GroupBooks = true, bool ResumeBooks = false, bool FindRadioLikes = true);

// Changed at runtime through the API, kept by PlaybackStore, read by
// PlaylistPlayer. A request's own `group` and `fromStart` win over it.
public sealed class PlaybackSettings
{
    private volatile PlaybackSettingsView _current = new();

    // Raised after a change, never while restoring.
    public event EventHandler<PlaybackSettingsView>? Changed;

    public PlaybackSettingsView Current => _current;

    public bool GroupBooks => _current.GroupBooks;

    public bool ResumeBooks => _current.ResumeBooks;

    public bool FindRadioLikes => _current.FindRadioLikes;

    // Null keeps. Answers the settings after the change.
    public PlaybackSettingsView Set(bool? groupBooks, bool? resumeBooks = null, bool? findRadioLikes = null)
    {
        var next = _current with
        {
            GroupBooks = groupBooks ?? _current.GroupBooks,
            ResumeBooks = resumeBooks ?? _current.ResumeBooks,
            FindRadioLikes = findRadioLikes ?? _current.FindRadioLikes,
        };
        if (next == _current)
            return next;
        _current = next;
        Changed?.Invoke(this, next);
        return next;
    }

    // At startup, before anyone reads them.
    public void Restore(PlaybackSettingsView saved) => _current = saved;
}
