using System.Text.Json;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Playback;

public enum LogSettingsError
{
    InvalidArgument,
    Unavailable,
}

// Level is what the server logs at now; Saved says it was set through the API
// rather than read from the configuration.
public sealed record LogSettingsView(string Level, bool Saved, IReadOnlyList<string> Levels);

// The one log level of this server and its core, as the API set it, kept in
// <dataDirectory>/logging.json. Until one is set, each logs at the level its
// own configuration names. Once set, it wins over both: the server applies it
// on Changed, and CoreLink sends it (log.configure) on every connect, so a
// core that restarts at its configured level is put back.
public sealed class LogSettings
{
    public static readonly IReadOnlyList<string> Levels = ["error", "warn", "info", "debug", "trace"];

    private sealed record Saved(string Level);

    private readonly string _path;
    private readonly string _configured;
    private readonly Lock _lock = new();
    private volatile string? _saved;

    // `configured` is the level the configuration names, one of Levels.
    public LogSettings(string path, string configured)
    {
        _path = path;
        _configured = configured;
    }

    // Raised with the new level after a change.
    public event EventHandler<string>? Changed;

    public string Level => _saved ?? _configured;

    // The level set through the API, or null while the configuration's applies.
    public string? SavedLevel => _saved;

    public LogSettingsView View => new(Level, _saved is not null, Levels);

    // At startup, before anyone reads it. A file that cannot be read leaves
    // the configured level and is replaced by the next change, never fatal;
    // the failure says why for the caller to log.
    public Result<bool, LogSettingsError> Load()
    {
        Saved? read;
        try
        {
            read = JsonSerializer.Deserialize<Saved>(File.ReadAllBytes(_path), JsonSerializerOptions.Web);
        }
        catch (Exception e) when (e is FileNotFoundException or DirectoryNotFoundException)
        {
            return false;
        }
        catch (Exception e) when (e is JsonException or IOException or UnauthorizedAccessException)
        {
            return Result.Fail(LogSettingsError.Unavailable, $"{_path}: {e.Message}");
        }
        if (read?.Level is not { } level || !Levels.Contains(level))
            return Result.Fail(LogSettingsError.Unavailable, $"{_path}: level must be one of {string.Join(", ", Levels)}");
        _saved = level;
        return true;
    }

    // Written before it is used, so a level in effect is one the next start
    // keeps. Answers whether it changed.
    public Result<bool, LogSettingsError> Set(string level)
    {
        if (!Levels.Contains(level))
        {
            return Result.Fail(LogSettingsError.InvalidArgument,
                $"level: one of {string.Join(", ", Levels)}, was {level}");
        }
        lock (_lock)
        {
            if (_saved == level)
                return false;
            var temporary = _path + ".tmp";
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
                File.WriteAllBytes(temporary, JsonSerializer.SerializeToUtf8Bytes(new Saved(level), JsonSerializerOptions.Web));
                File.Move(temporary, _path, overwrite: true);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                return Result.Fail(LogSettingsError.Unavailable, $"the log level cannot be written: {e.Message}");
            }
            _saved = level;
        }
        Changed?.Invoke(this, level);
        return true;
    }
}
