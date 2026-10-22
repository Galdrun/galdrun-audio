using System.Globalization;
using System.Text.RegularExpressions;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Library;

// M3U and PLS station lists, as StationStore.Import takes them. A PLS file
// starts with "[playlist]"; anything else is read as M3U. An entry whose URL
// is not an absolute http or https URL refuses the whole file, by line, rather
// than being dropped unsaid.
//
// M3U: each URL line is one station, named by the #EXTINF line before it
// ("#EXTINF:-1 tvg-logo="..." group-title="Jazz",Name"): tvg-logo is its logo,
// group-title its genre. PLS: FileN is the URL and TitleN its name. A station
// with no name is named after its URL's host and path.
public static partial class StationPlaylist
{
    public static Result<IReadOnlyList<NewStation>, LibraryError> Parse(string text)
    {
        var lines = text.ReplaceLineEndings("\n").Split('\n');
        var parsed = lines.FirstOrDefault(l => l.Trim().Length > 0)?.Trim()
            .Equals("[playlist]", StringComparison.OrdinalIgnoreCase) == true
            ? ParsePls(lines)
            : ParseM3u(lines);
        if (!parsed.IsSuccess)
            return parsed.Failure;
        return parsed.Value.Count == 0
            ? Result.Fail(LibraryError.InvalidArgument, "the list holds no stations")
            : parsed;
    }

    private static Result<IReadOnlyList<NewStation>, LibraryError> ParseM3u(string[] lines)
    {
        var stations = new List<NewStation>();
        string? name = null, genre = null;
        Uri? logo = null;
        for (var i = 0; i < lines.Length; i++)
        {
            var line = lines[i].Trim();
            if (line.StartsWith("#EXTINF:", StringComparison.OrdinalIgnoreCase))
            {
                var comma = CommaAfterAttributes(line);
                name = comma < 0 ? null : line[(comma + 1)..].Trim();
                var attributes = comma < 0 ? line : line[..comma];
                genre = Attribute(attributes, "group-title");
                logo = Attribute(attributes, "tvg-logo") is { } l && Web(l) is { } u ? u : null;
                continue;
            }
            if (line.Length == 0 || line.StartsWith('#'))
                continue;

            if (Web(line) is not { } url)
                return NotAUrl(i + 1, line);
            stations.Add(new NewStation(string.IsNullOrEmpty(name) ? NameOf(url) : name, url, Genre: genre, Logo: logo));
            name = genre = null;
            logo = null;
        }
        return stations;
    }

    private static Result<IReadOnlyList<NewStation>, LibraryError> ParsePls(string[] lines)
    {
        var files = new SortedDictionary<int, (Uri Url, int Line)>();
        var titles = new Dictionary<int, string>();
        for (var i = 0; i < lines.Length; i++)
        {
            var match = PlsEntry().Match(lines[i].Trim());
            if (!match.Success)
                continue;
            var n = int.Parse(match.Groups[2].Value, CultureInfo.InvariantCulture);
            var value = match.Groups[3].Value.Trim();
            if (match.Groups[1].Value.Equals("File", StringComparison.OrdinalIgnoreCase))
            {
                if (Web(value) is not { } url)
                    return NotAUrl(i + 1, value);
                files[n] = (url, i + 1);
            }
            else
            {
                titles[n] = value;
            }
        }
        return files.Select(f => new NewStation(
            titles.TryGetValue(f.Key, out var title) && title.Length > 0 ? title : NameOf(f.Value.Url),
            f.Value.Url)).ToList();
    }

    // The comma that ends the attributes: the first one outside quotes.
    private static int CommaAfterAttributes(string line)
    {
        var quoted = false;
        for (var i = 0; i < line.Length; i++)
        {
            if (line[i] == '"')
                quoted = !quoted;
            else if (line[i] == ',' && !quoted)
                return i;
        }
        return -1;
    }

    private static string? Attribute(string attributes, string key)
    {
        var match = Regex.Match(attributes, $"(?:^|\\s){Regex.Escape(key)}=\"([^\"]*)\"",
            RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
        return match.Success && match.Groups[1].Value.Trim().Length > 0 ? match.Groups[1].Value.Trim() : null;
    }

    private static Uri? Web(string text) =>
        Uri.TryCreate(text, UriKind.Absolute, out var uri) && (uri.Scheme == Uri.UriSchemeHttp || uri.Scheme == Uri.UriSchemeHttps)
            ? uri
            : null;

    private static string NameOf(Uri url) => (url.Authority + url.AbsolutePath).TrimEnd('/');

    private static Failure<LibraryError> NotAUrl(int line, string text) =>
        Result.Fail(LibraryError.InvalidArgument, $"line {line}: not an http or https URL: {text}");

    [GeneratedRegex(@"^(File|Title)(\d+)=(.*)$", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant)]
    private static partial Regex PlsEntry();
}
