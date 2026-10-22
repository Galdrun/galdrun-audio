using System.Globalization;
using System.Text;
using System.Text.Json;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Ai.Suggestions;

// What a proposal asks for beside songs (phase 38).
public enum ProposalAsk
{
    Albums,
    Artists,
    Audiobooks,
}

// What a scope likes, as plain names (phase 24's rule: never an id, a path
// or a URL): its liked and favourite songs, albums (title and artist),
// artists, audiobooks (title and author), and what it played lately.
public sealed record ProposalSeed(
    IReadOnlyList<SongSuggestion> Tracks,
    IReadOnlyList<SongSuggestion> Albums,
    IReadOnlyList<string> Artists,
    IReadOnlyList<SongSuggestion> Audiobooks,
    IReadOnlyList<SongSuggestion> Played);

// One name proposed: an album's or a book's title with its artist or author,
// or an artist's name in Title; and why, in one line.
public sealed record ProposedName(string Title, string? Artist, string? Reason);

public sealed record ProposedNames(IReadOnlyList<ProposedName> Items, AiUsage Usage);

public sealed record ProposalRequest(
    ProposalAsk Kind,
    string Request,
    ProposalSeed Seed,
    IReadOnlyList<ProposedName> Avoid,
    int Count,
    string Provider,
    string Model,
    AiReasoning Reasoning = AiReasoning.Default);

// Asks a model for albums, artists or audiobooks by name (phase 38), beside
// SongSuggester and in its shape: one forced call of suggest_albums,
// suggest_artists or suggest_audiobooks, structured on every vendor, and
// anything else is InvalidAnswer. What it tells the model is
// AiInstructions.Proposals, not code. It knows no service: whether the house
// can play a name is the caller's to find.
public sealed partial class ProposalSuggester(AiProviderRegistry ai, AiInstructions instructions,
    ILogger<ProposalSuggester> log)
{
    public const int MaxCount = 100;

    public static string ToolOf(ProposalAsk kind) => kind switch
    {
        ProposalAsk.Albums => "suggest_albums",
        ProposalAsk.Artists => "suggest_artists",
        _ => "suggest_audiobooks",
    };

    // The array the tool answers, and the field that names the artist or author.
    private static (string Items, string? By) FieldsOf(ProposalAsk kind) => kind switch
    {
        ProposalAsk.Albums => ("albums", "artist"),
        ProposalAsk.Artists => ("artists", null),
        _ => ("audiobooks", "author"),
    };

    private static string TitleField(ProposalAsk kind) => kind == ProposalAsk.Artists ? "name" : "title";

    public async Task<Result<ProposedNames, AiError>> SuggestAsync(ProposalRequest request,
        CancellationToken cancellationToken)
    {
        if (request.Count is < 1 or > MaxCount)
            return Result.Fail(AiError.InvalidArgument, $"count: 1 to {MaxCount}, was {request.Count}");
        var text = instructions.Proposals;
        var name = ToolOf(request.Kind);
        var description = request.Kind switch
        {
            ProposalAsk.Albums => text.AlbumsTool,
            ProposalAsk.Artists => text.ArtistsTool,
            _ => text.AudiobooksTool,
        };
        var tool = new AiTool(name, description, Schema(request.Kind, request.Count, text.ReasonDescription));
        var asked = new AiRequest(request.Model, [new AiMessage(AiRole.User, Prompt(request, text))], text.System, [tool],
            Math.Min(8192, 256 + 96 * request.Count), name, request.Reasoning);
        LogAsking(log, request.Provider, request.Count, name);
        var answer = await ai.CompleteAsync(request.Provider, asked, cancellationToken).ConfigureAwait(false);
        if (!answer.IsSuccess)
            return answer.Failure;
        var call = answer.Value.ToolCalls.FirstOrDefault(c => c.Name == name);
        if (call is null)
            return Result.Fail(AiError.InvalidAnswer, $"{request.Provider} did not answer with {name}");
        var items = ItemsOf(request.Kind, call.Arguments);
        if (!items.IsSuccess)
            return Result.Fail(AiError.InvalidAnswer, $"{request.Provider}: {items.Failure.Detail}");
        return new ProposedNames([.. items.Value.Take(request.Count)], answer.Value.Usage);
    }

    // The request, the seed by kind, what to avoid, then the count.
    public static string Prompt(ProposalRequest request, ProposalInstructions text)
    {
        var prompt = new StringBuilder();
        prompt.Append(AiInstructions.Fill(text.Request, "request", request.Request.Trim())).Append('\n');
        Section(prompt, text.SeedTracks, request.Seed.Tracks.Select(Line));
        Section(prompt, text.SeedAlbums, request.Seed.Albums.Select(Line));
        Section(prompt, text.SeedArtists, request.Seed.Artists);
        Section(prompt, text.SeedAudiobooks, request.Seed.Audiobooks.Select(Line));
        Section(prompt, text.Played, request.Seed.Played.Select(Line));
        Section(prompt, text.Avoid, request.Avoid.Select(a => a.Artist is { Length: > 0 } by ? $"{a.Title} — {by}" : a.Title));
        var count = request.Kind switch
        {
            ProposalAsk.Albums => text.Albums,
            ProposalAsk.Artists => text.Artists,
            _ => text.Audiobooks,
        };
        prompt.Append('\n').Append(AiInstructions.Fill(count, "count", request.Count.ToString(CultureInfo.InvariantCulture)));
        return prompt.ToString();
    }

    private static void Section(StringBuilder prompt, string heading, IEnumerable<string> lines)
    {
        var list = lines.ToList();
        if (list.Count == 0)
            return;
        prompt.Append('\n').Append(heading).Append('\n');
        foreach (var line in list)
            prompt.Append("- ").Append(line).Append('\n');
    }

    private static string Line(SongSuggestion item)
    {
        var line = new StringBuilder(item.Title);
        if (item.Artist is { Length: > 0 } artist)
            line.Append(" — ").Append(artist);
        if (item.Album is { Length: > 0 } album)
            line.Append(" (album: ").Append(album).Append(')');
        if (item.Year is { } year)
            line.Append(CultureInfo.InvariantCulture, $" [{year}]");
        return line.ToString();
    }

    private static JsonElement Schema(ProposalAsk kind, int count, string reasonDescription)
    {
        var (items, by) = FieldsOf(kind);
        var title = TitleField(kind);
        var properties = new Dictionary<string, object>
        {
            [title] = new { type = "string" },
            ["reason"] = new { type = "string", description = reasonDescription },
        };
        if (by is not null)
            properties[by] = new { type = "string" };
        string[] required = by is null ? [title, "reason"] : [title, by, "reason"];
        return JsonSerializer.SerializeToElement(new Dictionary<string, object>
        {
            ["type"] = "object",
            ["properties"] = new Dictionary<string, object>
            {
                [items] = new Dictionary<string, object>
                {
                    ["type"] = "array",
                    ["maxItems"] = count,
                    ["items"] = new Dictionary<string, object>
                    {
                        ["type"] = "object",
                        ["properties"] = properties,
                        ["required"] = required,
                    },
                },
            },
            ["required"] = new[] { items },
        });
    }

    // The tool's arguments as its schema has them; a reason may be missing,
    // a title (and an artist or author) may not. Anything else is refused whole.
    private static Result<List<ProposedName>, AiError> ItemsOf(ProposalAsk kind, JsonElement arguments)
    {
        var (items, by) = FieldsOf(kind);
        var title = TitleField(kind);
        if (arguments.ValueKind != JsonValueKind.Object ||
            !arguments.TryGetProperty(items, out var array) || array.ValueKind != JsonValueKind.Array)
        {
            return Result.Fail(AiError.InvalidAnswer, $"{ToolOf(kind)} without an {items} array");
        }
        var list = new List<ProposedName>();
        foreach (var item in array.EnumerateArray())
        {
            var at = list.Count;
            if (item.ValueKind != JsonValueKind.Object || Text(item, title) is not { } named)
                return Result.Fail(AiError.InvalidAnswer, $"{items}.{at}: a {title} is required");
            string? artist = null;
            if (by is not null && (artist = Text(item, by)) is null)
                return Result.Fail(AiError.InvalidAnswer, $"{items}.{at}: an {by} is required");
            list.Add(new ProposedName(named, artist, Text(item, "reason")));
        }
        return list;
    }

    private static string? Text(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String &&
        !string.IsNullOrWhiteSpace(value.GetString())
            ? value.GetString()!.Trim()
            : null;

    // The counts only: the request and the names are user content.
    [LoggerMessage(Level = LogLevel.Debug, Message = "asking {Provider} for {Count} through {Tool}")]
    private static partial void LogAsking(ILogger log, string provider, int count, string tool);
}
