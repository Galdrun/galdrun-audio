using System.Text;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Configuration;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Logging;

// The server's own logger, behind the framework's ILogger so the host's and
// Kestrel's lines land in the same place as ours. Writes to stderr and, if
// configured, to a file -- the core's util/Logger shape, with no logging
// package on top.
//
// Rotation is the core's phase 29 rule: a line that would take the file past
// the limit first renames it to `<path>.1`, replacing an older one, so the disk
// holds at most twice the limit and each file stays under it (a single line
// longer than the limit is written whole, alone in its file). It happens under
// the lock every line takes, so no line is split between two files.
//
// Writes are synchronous under one lock. That is a control-path facility:
// nothing on the media path may log per request, because a blocked disk would
// become a stalled room.
public sealed class ServerLoggerProvider : ILoggerProvider
{
    private readonly Lock _lock = new();
    private readonly TextWriter _console;
    private readonly string? _path;
    private readonly long _maxBytes;
    private FileStream? _file;
    private long _fileBytes;
    private volatile int _minimum = (int)LogLevel.Information;

    private ServerLoggerProvider(TextWriter console, string? path, long maxBytes, FileStream? file)
    {
        _console = console;
        _path = path;
        _maxBytes = maxBytes;
        _file = file;
        _fileBytes = file?.Length ?? 0;
    }

    // A null path logs to the console only. A file that cannot be opened is a
    // startup error: the operator asked for it and would look there first.
    public static Result<ServerLoggerProvider, ConfigError> Open(TextWriter console, string? path, long maxBytes)
    {
        if (path is null)
            return new ServerLoggerProvider(console, null, maxBytes, null);
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            return new ServerLoggerProvider(console, path, maxBytes, OpenAppend(path));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return Result.Fail(ConfigError.UnusableLogFile, $"{path}: {e.Message}");
        }
    }

    public ILogger CreateLogger(string categoryName) => new ServerLogger(this, ShortCategory(categoryName));

    // The level a line needs to be written, changed while the server runs
    // (LogSettings). The factory's own minimum stays at Trace for this.
    public void SetMinimumLevel(LogLevel level) => _minimum = (int)level;

    public void Dispose()
    {
        lock (_lock)
        {
            _file?.Dispose();
            _file = null;
        }
    }

    internal void Write(LogLevel level, string category, string message, Exception? exception)
    {
        var now = DateTime.Now;
        var name = LevelName(level);
        var thread = Environment.CurrentManagedThreadId;
        var text = exception is null ? message : $"{message}{Environment.NewLine}{exception}";

        lock (_lock)
        {
            _console.WriteLine($"{now:HH:mm:ss.fff} {name,-5} t{thread,-2} {category}: {text}");
            _console.Flush();

            if (_file is null)
                return;
            // LF on every platform: the count is of bytes on disk.
            var line = Encoding.UTF8.GetBytes(
                $"{now:yyyy-MM-dd HH:mm:ss.fff} {name,-5} t{thread,-2} {category}: {text.ReplaceLineEndings("\n")}\n");
            if (_maxBytes > 0 && _fileBytes > 0 && _fileBytes + line.Length > _maxBytes)
                RotateLocked();
            _file.Write(line);
            _file.Flush();
            _fileBytes += line.Length;
        }
    }

    // If the rename fails -- something holds the old `.1` open -- the file is
    // started again in place, because a limit that holds only when nothing goes
    // wrong is not a limit.
    private void RotateLocked()
    {
        _file!.Dispose();
        try
        {
            File.Move(_path!, _path + ".1", overwrite: true);
            _file = OpenAppend(_path!);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            _file = new FileStream(_path!, FileMode.Create, FileAccess.Write, FileShare.Read | FileShare.Delete);
            _console.WriteLine($"log rotation could not rename {_path} ({e.Message}); started it again");
        }
        _fileBytes = 0;
    }

    // Shared for reading and deleting, so an operator can tail the file and a
    // rotation is not blocked by a reader.
    private static FileStream OpenAppend(string path) =>
        new(path, FileMode.Append, FileAccess.Write, FileShare.Read | FileShare.Delete);

    private static string ShortCategory(string category)
    {
        var dot = category.LastIndexOf('.');
        return dot < 0 ? category : category[(dot + 1)..];
    }

    private static string LevelName(LogLevel level) => level switch
    {
        LogLevel.Trace => "TRACE",
        LogLevel.Debug => "DEBUG",
        LogLevel.Information => "INFO",
        LogLevel.Warning => "WARN",
        LogLevel.Error => "ERROR",
        LogLevel.Critical => "FATAL",
        _ => "?",
    };

    private sealed class ServerLogger(ServerLoggerProvider provider, string category) : ILogger
    {
        // Scopes are not rendered; the line format has no place for them.
        public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;

        // The factory's per-category filters (Kestrel's, the MCP SDK's) come
        // first; the level that can change at runtime is the provider's.
        public bool IsEnabled(LogLevel logLevel) =>
            logLevel != LogLevel.None && (int)logLevel >= provider._minimum;

        public void Log<TState>(LogLevel logLevel, EventId eventId, TState state, Exception? exception,
            Func<TState, Exception?, string> formatter)
        {
            if (IsEnabled(logLevel))
                provider.Write(logLevel, category, formatter(state, exception), exception);
        }
    }
}
