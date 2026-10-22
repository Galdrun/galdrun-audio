using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading.Channels;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Playback.Core;

namespace Multiroom.Server.Playback;

// One queue as it is kept across a restart of this server. PositionNs is the
// playhead in the current item (in a group: in Part), best effort; Playing
// says it was playing.
public sealed record SavedQueueState(
    string Zone,
    IReadOnlyList<QueueItem> Items,
    QueueItemId? Current,
    RepeatMode Repeat,
    bool Shuffle,
    ulong? PositionNs,
    bool Playing,
    int Part = 0);

// AutoResume: queues that were playing start again by themselves. Off by
// default: a house that was playing when this server went down asks.
public sealed record PlaybackStoreOptions(string Path, bool AutoResume);

// What must outlive this process: every queue with its position, and
// CoreLink's desired state (zones, memberships, room settings), so a core that
// restarts after this server did is still rebuilt. One JSON file, written
// whole and moved into place, a second after a change, every 15 s while a
// queue plays (for the playhead) and once more when the server stops.
//
// Started before CoreLink, so the first connect asserts what was restored,
// and stopped after it, so the last write sees the final state.
public sealed partial class PlaybackStore : BackgroundService
{
    // 2 (phase 15) adds each zone's volume and mute; a format 1 file restores
    // every zone at 1.0, not muted. A room's "bitperfect" from before MATP
    // section 59 is an unknown key now, and ignored like any other. 3 (phase
    // 26) adds each zone's AutoFill; an older file restores none. 4 (phases
    // 30, 31) adds a track's media type, groups with their parts, and the
    // playhead's part; a format 3 file reads as before. 5 adds the playback
    // settings (groupBooks); an older file keeps the defaults. 6 (phases 32,
    // 33) adds a part's chapter, a group's saved place, and resumeBooks; an
    // older file reads as before. 7 added each room's feeds, since removed: the
    // key is unknown now and ignored. 8 (phase 36) adds each zone's user; an
    // older file leaves every zone to the household. 9 (phase 39) adds each
    // room's last name; an older file shows rooms by their ids until they say
    // one. 10 adds each room's equalizer and the user's equalizer profiles; an
    // older file has neither.
    private const int FormatVersion = 10;
    private static readonly TimeSpan Debounce = TimeSpan.FromSeconds(1);
    private static readonly TimeSpan Checkpoint = TimeSpan.FromSeconds(15);

    // Nulls are written: a queue item's track or station is a required
    // parameter that is null for the other kind.
    private static readonly JsonSerializerOptions Json = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        RespectNullableAnnotations = true,
        RespectRequiredConstructorParameters = true,
        Converters =
        {
            new AudioControlJsonConverter(),
            new JsonStringEnumConverter(JsonNamingPolicy.CamelCase, allowIntegerValues: false),
        },
    };

    private readonly PlaybackStoreOptions _options;
    private readonly QueueEngine _engine;
    private readonly DesiredState _desired;
    private readonly AutoFill? _autoFill;
    private readonly PlaybackSettings? _settings;
    private readonly ZoneUsers? _users;
    private readonly EqualizerProfiles? _equalizerProfiles;
    private readonly ILogger _log;
    private readonly Channel<bool> _changes =
        Channel.CreateBounded<bool>(new BoundedChannelOptions(1) { FullMode = BoundedChannelFullMode.DropWrite });

    public PlaybackStore(PlaybackStoreOptions options, QueueEngine engine, CoreLink link, ILogger<PlaybackStore> log,
        AutoFill? autoFill = null, PlaybackSettings? settings = null, ZoneUsers? users = null,
        EqualizerProfiles? equalizerProfiles = null)
    {
        _equalizerProfiles = equalizerProfiles;
        _settings = settings;
        _users = users;
        _options = options;
        _engine = engine;
        _desired = link.Desired;
        _autoFill = autoFill;
        _log = log;
    }

    private sealed record Saved(
        int Version,
        IReadOnlyList<SavedZone> Zones,
        IReadOnlyDictionary<string, DesiredRoom> Rooms,
        IReadOnlyList<SavedQueueState> Queues,
        IReadOnlyDictionary<string, AutoFillSettings>? AutoFill = null,
        PlaybackSettingsView? Settings = null,
        IReadOnlyDictionary<string, string>? Users = null,
        IReadOnlyList<EqualizerProfile>? EqualizerProfiles = null);

    private sealed record SavedZone(string Zone, IReadOnlyList<string>? Clients = null, double Volume = 1.0,
        bool Muted = false);

    public override Task StartAsync(CancellationToken cancellationToken)
    {
        if (Load() is { } saved)
        {
            _desired.Restore(saved.Zones.Select(z => (z.Zone, z.Clients)), saved.Rooms,
                saved.Zones.Where(z => z.Volume != 1.0 || z.Muted)
                    .ToDictionary(z => z.Zone, z => new DesiredZoneLevel(z.Volume, z.Muted), StringComparer.Ordinal));
            _engine.Restore(saved.Queues, _options.AutoResume);
            if (saved.AutoFill is { } autoFill)
                _autoFill?.Restore(autoFill);
            if (saved.Settings is { } playback)
                _settings?.Restore(playback);
            if (saved.Users is { } users)
                _users?.Restore(users);
            if (saved.EqualizerProfiles is { } profiles)
                _equalizerProfiles?.Restore(profiles);
            LogRestored(_log, saved.Queues.Count, saved.Zones.Count, _options.Path,
                _options.AutoResume ? "those that were playing resume" : "stopped");
        }
        _engine.QueueChanged += (_, _) => _changes.Writer.TryWrite(true);
        _engine.QueueDeleted += (_, _) => _changes.Writer.TryWrite(true);
        _desired.Changed += (_, _) => _changes.Writer.TryWrite(true);
        if (_autoFill is not null)
            _autoFill.Changed += (_, _) => _changes.Writer.TryWrite(true);
        if (_settings is not null)
            _settings.Changed += (_, _) => _changes.Writer.TryWrite(true);
        if (_users is not null)
            _users.Changed += (_, _) => _changes.Writer.TryWrite(true);
        if (_equalizerProfiles is not null)
            _equalizerProfiles.Changed += (_, _) => _changes.Writer.TryWrite(true);
        return base.StartAsync(cancellationToken);
    }

    public override async Task StopAsync(CancellationToken cancellationToken)
    {
        await base.StopAsync(cancellationToken).ConfigureAwait(false);
        Write();
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        try
        {
            while (true)
            {
                var playing = _engine.Queues.Any(q => q.Transport == QueueTransport.Playing);
                try
                {
                    await _changes.Reader.WaitToReadAsync(stoppingToken).AsTask()
                        .WaitAsync(playing ? Checkpoint : Timeout.InfiniteTimeSpan, stoppingToken)
                        .ConfigureAwait(false);
                }
                catch (TimeoutException)
                {
                    // Playing: the playhead has moved on.
                }
                // Changes come in bursts: one write after the burst.
                await Task.Delay(Debounce, stoppingToken).ConfigureAwait(false);
                while (_changes.Reader.TryRead(out _))
                {
                }
                Write();
            }
        }
        catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
        {
        }
    }

    // A file that cannot be read is logged and replaced by the next write: a
    // lost queue is not worth a server that will not start.
    private Saved? Load()
    {
        if (!File.Exists(_options.Path))
            return null;
        try
        {
            using var file = File.OpenRead(_options.Path);
            var saved = JsonSerializer.Deserialize<Saved>(file, Json);
            if (saved?.Version is 1 or 2 or 3 or 4 or 5 or 6 or 7 or 8 or 9 or FormatVersion)
                return saved;
            LogUnreadable(_log, _options.Path, $"format {saved?.Version} is not {FormatVersion}");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException)
        {
            LogUnreadable(_log, _options.Path, e.Message);
        }
        return null;
    }

    private void Write()
    {
        var levels = _desired.ZoneLevels;
        var saved = new Saved(FormatVersion, [.. _desired.Zones.Select(z =>
            {
                var level = levels.GetValueOrDefault(z.Zone) ?? new DesiredZoneLevel();
                return new SavedZone(z.Zone, z.Clients, level.Volume, level.Muted);
            })],
            _desired.Rooms, _engine.Save(), _autoFill?.Save(), _settings?.Current, _users?.Save(),
            _equalizerProfiles?.Save());
        var temporary = _options.Path + ".tmp";
        try
        {
            using (var file = File.Create(temporary))
                JsonSerializer.Serialize(file, saved, Json);
            File.Move(temporary, _options.Path, overwrite: true);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            LogUnwritable(_log, _options.Path, e.Message);
        }
    }

    [LoggerMessage(Level = LogLevel.Information,
        Message = "restored {Queues} queue(s) and {Zones} zone(s) from {Path}; {Resume}")]
    private static partial void LogRestored(ILogger log, int queues, int zones, string path, string resume);

    [LoggerMessage(Level = LogLevel.Warning, Message = "could not read {Path} ({Reason}); starting with no queues")]
    private static partial void LogUnreadable(ILogger log, string path, string reason);

    [LoggerMessage(Level = LogLevel.Warning, Message = "could not write {Path}: {Reason}")]
    private static partial void LogUnwritable(ILogger log, string path, string reason);
}
