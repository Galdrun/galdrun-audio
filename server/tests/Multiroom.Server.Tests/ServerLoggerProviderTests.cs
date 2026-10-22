using Microsoft.Extensions.Logging;
using Multiroom.Server.Logging;

namespace Multiroom.Server.Tests;

public sealed class ServerLoggerProviderTests : IDisposable
{
    private readonly TempDirectory _dir = new();

    public void Dispose() => _dir.Dispose();

    [Fact]
    public void Rotation_keeps_the_current_file_and_one_previous_both_whole_and_under_the_limit()
    {
        const int limit = 4096;
        const int lines = 400;
        var path = Path.Combine(_dir.Path, "server.log");

        using (var provider = ServerLoggerProvider.Open(TextWriter.Null, path, limit).Value)
        {
            var log = provider.CreateLogger("Multiroom.Test");
            for (var i = 0; i < lines; i++)
                log.LogInformation("line {Number:D4}", i);
        }

        Assert.Equal(["server.log", "server.log.1"],
            Directory.GetFiles(_dir.Path).Select(Path.GetFileName).Order(StringComparer.Ordinal));
        var previous = File.ReadAllText(path + ".1");
        var current = File.ReadAllText(path);
        foreach (var text in new[] { previous, current })
        {
            Assert.InRange(text.Length, 1, limit);
            Assert.EndsWith("\n", text, StringComparison.Ordinal);
        }

        // Newest line in the current file, and no line missing across the two.
        var numbers = (previous + current).Split('\n', StringSplitOptions.RemoveEmptyEntries)
            .Select(l => int.Parse(l[^4..], System.Globalization.CultureInfo.InvariantCulture))
            .ToList();
        Assert.Equal(lines - 1, numbers[^1]);
        Assert.Equal(Enumerable.Range(numbers[0], numbers.Count), numbers);
    }

    [Fact]
    public void A_line_names_level_category_and_message_on_the_console_and_in_the_file()
    {
        var path = Path.Combine(_dir.Path, "server.log");
        using var console = new StringWriter();

        using (var provider = ServerLoggerProvider.Open(console, path, 0).Value)
            provider.CreateLogger("Multiroom.Server.Host").LogWarning("disk {Name} is full", "D:");

        Assert.Matches(@"^\d\d:\d\d:\d\d\.\d{3} WARN  t\d+ +Host: disk D: is full\r?\n$", console.ToString());
        Assert.Matches(@"^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3} WARN  t\d+ +Host: disk D: is full\n$",
            File.ReadAllText(path));
    }
}
