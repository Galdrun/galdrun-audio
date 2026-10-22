using System.Text.Json;
using System.Text.Json.Serialization;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Providers.Covers;

// Where a setting comes from: the config file (or the default), or the API.
public enum ArtworkOrigin
{
    Config,
    Api,
    None,
}

public sealed record MusicBrainzView(bool Enabled, ArtworkOrigin Origin);

// Enabled is the switch; Active, whether fanart.tv is asked: switched on
// with a key. The key is never answered: HasKey and KeyOrigin say.
public sealed record FanartTvView(bool Enabled, bool HasKey, ArtworkOrigin KeyOrigin, bool Active);

public sealed record ArtworkSettingsView(MusicBrainzView MusicBrainz, FanartTvView FanartTv);

// The artwork settings at runtime: the config's (artwork.*), with what the
// API set over them, kept in <dataDirectory>/artwork/settings.json, owner
// only since it holds fanart.tv's key. What the API set wins; removing the
// key through the API falls back to the config's. Changed is raised after
// each change, so the add-in asks again what it could not find.
public sealed class ArtworkSettingsStore
{
    private static readonly JsonSerializerOptions Options = new(JsonSerializerDefaults.Web)
    {
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    };

    private readonly string _path;
    private readonly ArtworkSettings _config;
    private readonly Lock _lock = new();
    private Stored _stored;

    public ArtworkSettingsStore(string path, ArtworkSettings config)
    {
        _path = path;
        _config = config;
        _stored = Read(path);
    }

    public event EventHandler? Changed;

    public bool MusicBrainz
    {
        get
        {
            lock (_lock)
                return _stored.MusicBrainz ?? _config.MusicBrainz;
        }
    }

    // The key fanart.tv is asked with, or null when it is not to be asked.
    public string? FanartTvKey
    {
        get
        {
            lock (_lock)
                return (_stored.FanartTv ?? true) ? _stored.FanartTvKey ?? _config.FanartTvKey : null;
        }
    }

    public ArtworkSettingsView Current
    {
        get
        {
            lock (_lock)
            {
                var key = _stored.FanartTvKey ?? _config.FanartTvKey;
                var enabled = _stored.FanartTv ?? true;
                return new ArtworkSettingsView(
                    new MusicBrainzView(_stored.MusicBrainz ?? _config.MusicBrainz,
                        _stored.MusicBrainz is null ? ArtworkOrigin.Config : ArtworkOrigin.Api),
                    new FanartTvView(enabled, key is not null,
                        _stored.FanartTvKey is not null ? ArtworkOrigin.Api
                            : _config.FanartTvKey is not null ? ArtworkOrigin.Config : ArtworkOrigin.None,
                        enabled && key is not null));
            }
        }
    }

    // Null keeps a switch as it is.
    public Result<ArtworkSettingsView, ProviderError> Set(bool? musicBrainz, bool? fanartTv) =>
        Update(s => s with { MusicBrainz = musicBrainz ?? s.MusicBrainz, FanartTv = fanartTv ?? s.FanartTv });

    public Result<ArtworkSettingsView, ProviderError> SetFanartTvKey(string key) =>
        string.IsNullOrWhiteSpace(key)
            ? Result.Fail(ProviderError.InvalidArgument, "key: a fanart.tv project key")
            : Update(s => s with { FanartTvKey = key.Trim() });

    public Result<ArtworkSettingsView, ProviderError> ClearFanartTvKey() => Update(s => s with { FanartTvKey = null });

    private Result<ArtworkSettingsView, ProviderError> Update(Func<Stored, Stored> change)
    {
        bool changed;
        lock (_lock)
        {
            var next = change(_stored);
            changed = next != _stored;
            if (changed)
            {
                try
                {
                    OwnerOnlyFile.Write(_path, JsonSerializer.SerializeToUtf8Bytes(next, Options));
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                {
                    return Result.Fail(ProviderError.Failed, $"the artwork settings cannot be kept: {e.Message}");
                }
                _stored = next;
            }
        }
        if (changed)
            Changed?.Invoke(this, EventArgs.Empty);
        return Current;
    }

    // A file that cannot be read is no settings: the config's hold, and the
    // next change writes it anew.
    private static Stored Read(string path)
    {
        try
        {
            return File.Exists(path) ? JsonSerializer.Deserialize<Stored>(File.ReadAllBytes(path), Options) ?? new() : new();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException)
        {
            return new();
        }
    }

    // What the API set; null is not set.
    private sealed record Stored(bool? MusicBrainz = null, bool? FanartTv = null, string? FanartTvKey = null)
    {
        // The key goes in no message.
        public override string ToString() => $"{nameof(Stored)} {{ MusicBrainz = {MusicBrainz}, FanartTv = {FanartTv} }}";
    }
}
