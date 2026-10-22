using System.Globalization;
using System.Text;
using System.Text.Json;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Ai.Suggestions;

// One request for songs: what the user asked ("calmer", "more like the
// 70s"), the seed and the songs not to suggest, as plain names, and how many.
// Notes: what a web lookup found for it (phase 27), given to every round.
public sealed record SongRequest(
    string Request,
    IReadOnlyList<SeedSong> Seed,
    IReadOnlyList<SongSuggestion> Avoid,
    int Count,
    string Provider,
    string Model,
    AiReasoning Reasoning = AiReasoning.Default,
    string? Notes = null);

// A song of the seed, and whether the household liked it or made it a
// favourite: one mark either way, never two (the owner's decision). The model
// is told to weight marked songs more (AiInstructions.Songs.Preferred).
public sealed record SeedSong(SongSuggestion Song, bool Preferred = false);

// What a web lookup found (phase 27): the model's notes, the pages the
// vendor cited, and what it cost.
public sealed record SongLookup(string Notes, IReadOnlyList<AiSource> Sources, AiUsage Usage);

// The songs the model named, in its order, and what asking cost.
public sealed record SuggestedSongs(IReadOnlyList<SongSuggestion> Songs, AiUsage Usage);

// Asks a model for songs by name. It knows no
// service: what the house holds is the resolver's to find. What it tells the
// model is AiInstructions.Songs, not code. The answer is one
// forced call of suggest_songs, so it is structured on every vendor; anything
// else is InvalidAnswer. The seed and the songs to avoid go as plain titles,
// artists, albums and years, never an id, a path or a URL.
public sealed partial class SongSuggester(AiProviderRegistry ai, AiInstructions instructions, ILogger<SongSuggester> log)
{
    public const int MaxCount = 100;
    public const string ToolName = "suggest_songs";

    // The notes a web lookup may write (phase 27).
    public const int LookupTokens = 2048;

    private static readonly string[] SongFields = ["title", "artist"];
    private static readonly string[] SongsField = ["songs"];

    public async Task<Result<SuggestedSongs, AiError>> SuggestAsync(SongRequest request,
        CancellationToken cancellationToken)
    {
        if (request.Count is < 1 or > MaxCount)
            return Result.Fail(AiError.InvalidArgument, $"count: 1 to {MaxCount}, was {request.Count}");
        var text = instructions.Songs;
        var tool = new AiTool(ToolName, text.ToolDescription, Schema(request.Count, text.ArtistDescription));
        var asked = new AiRequest(request.Model, [new AiMessage(AiRole.User, Prompt(request, text))], text.System, [tool],
            Math.Min(8192, 256 + 64 * request.Count), ToolName, request.Reasoning);
        LogAsking(log, request.Provider, request.Count, request.Seed.Count, request.Avoid.Count);
        var answer = await ai.CompleteAsync(request.Provider, asked, cancellationToken).ConfigureAwait(false);
        if (!answer.IsSuccess)
            return answer.Failure;
        var call = answer.Value.ToolCalls.FirstOrDefault(c => c.Name == ToolName);
        if (call is null)
            return Result.Fail(AiError.InvalidAnswer, $"{request.Provider} did not answer with {ToolName}");
        var songs = SongsOf(call.Arguments);
        if (!songs.IsSuccess)
            return Result.Fail(AiError.InvalidAnswer, $"{request.Provider}: {songs.Failure.Detail}");
        return new SuggestedSongs([.. songs.Value.Take(request.Count)], answer.Value.Usage);
    }

    // Phase 27's first step, the owner's choice of two (2026-10-01): one turn
    // with the vendor's web search and no function, so a forced suggest_songs
    // cannot keep the model from searching. It is told the same request,
    // seed and songs to avoid, and answers notes in words; the rounds that
    // name the songs are then told them (SongRequest.Notes).
    public async Task<Result<SongLookup, AiError>> LookupAsync(SongRequest request, CancellationToken cancellationToken)
    {
        var text = instructions.Songs;
        var asked = new AiRequest(request.Model, [new AiMessage(AiRole.User, Prompt(request with { Notes = null }, text))],
            text.LookupSystem, MaxTokens: LookupTokens, Reasoning: request.Reasoning, WebSearch: true);
        LogLookingUp(log, request.Provider);
        var answer = await ai.CompleteAsync(request.Provider, asked, cancellationToken).ConfigureAwait(false);
        if (!answer.IsSuccess)
            return answer.Failure;
        if (string.IsNullOrWhiteSpace(answer.Value.Text))
            return Result.Fail(AiError.InvalidAnswer, $"{request.Provider} answered the web lookup without notes");
        return new SongLookup(answer.Value.Text.Trim(), answer.Value.Sources ?? [], answer.Value.Usage);
    }

    // The user's request, then the seed and what to avoid, one song a line.
    public static string Prompt(SongRequest request, SongInstructions instructions)
    {
        var text = new StringBuilder();
        text.Append(AiInstructions.Fill(instructions.Request, "request", request.Request.Trim())).Append('\n');
        if (request.Seed.Count > 0)
        {
            text.Append('\n').Append(instructions.Seed).Append('\n');
            foreach (var seed in request.Seed)
            {
                text.Append("- ").Append(Line(seed.Song));
                if (seed.Preferred)
                    text.Append(' ').Append(instructions.Preferred);
                text.Append('\n');
            }
        }
        if (request.Avoid.Count > 0)
        {
            text.Append('\n').Append(instructions.Avoid).Append('\n');
            foreach (var song in request.Avoid)
                text.Append("- ").Append(Line(song)).Append('\n');
        }
        if (request.Notes is { Length: > 0 } notes)
            text.Append('\n').Append(instructions.LookupNotes).Append('\n').Append(notes).Append('\n');
        text.Append('\n').Append(AiInstructions.Fill(instructions.Count, "count",
            request.Count.ToString(CultureInfo.InvariantCulture)));
        return text.ToString();
    }

    private static string Line(SongSuggestion song)
    {
        var line = new StringBuilder(song.Title);
        if (song.Artist is { Length: > 0 } artist)
            line.Append(" — ").Append(artist);
        if (song.Album is { Length: > 0 } album)
            line.Append(" (album: ").Append(album).Append(')');
        if (song.Year is { } year)
            line.Append(CultureInfo.InvariantCulture, $" [{year}]");
        return line.ToString();
    }

    private static JsonElement Schema(int count, string artistDescription) => JsonSerializer.SerializeToElement(new Dictionary<string, object>
    {
        ["type"] = "object",
        ["properties"] = new Dictionary<string, object>
        {
            ["songs"] = new Dictionary<string, object>
            {
                ["type"] = "array",
                ["maxItems"] = count,
                ["items"] = new Dictionary<string, object>
                {
                    ["type"] = "object",
                    ["properties"] = new Dictionary<string, object>
                    {
                        ["title"] = new { type = "string" },
                        ["artist"] = new { type = "string", description = artistDescription },
                        ["album"] = new { type = "string" },
                        ["year"] = new { type = "integer" },
                    },
                    ["required"] = SongFields,
                },
            },
        },
        ["required"] = SongsField,
    });

    // The tool's arguments as the schema has them: a title and an artist each,
    // an album and a year where given. Anything else is refused whole.
    private static Result<List<SongSuggestion>, AiError> SongsOf(JsonElement arguments)
    {
        if (arguments.ValueKind != JsonValueKind.Object ||
            !arguments.TryGetProperty("songs", out var songs) || songs.ValueKind != JsonValueKind.Array)
        {
            return Result.Fail(AiError.InvalidAnswer, $"{ToolName} without a songs array");
        }
        var list = new List<SongSuggestion>();
        foreach (var song in songs.EnumerateArray())
        {
            var at = list.Count;
            if (song.ValueKind != JsonValueKind.Object ||
                Text(song, "title") is not { } title || Text(song, "artist") is not { } artist)
            {
                return Result.Fail(AiError.InvalidAnswer, $"songs.{at}: a title and an artist are required");
            }
            string? album = null;
            if (song.TryGetProperty("album", out var a) && a.ValueKind != JsonValueKind.Null)
            {
                if (a.ValueKind != JsonValueKind.String)
                    return Result.Fail(AiError.InvalidAnswer, $"songs.{at}.album is not text");
                album = string.IsNullOrWhiteSpace(a.GetString()) ? null : a.GetString()!.Trim();
            }
            int? year = null;
            if (song.TryGetProperty("year", out var y) && y.ValueKind != JsonValueKind.Null)
            {
                if (y.ValueKind != JsonValueKind.Number || !y.TryGetInt32(out var number))
                    return Result.Fail(AiError.InvalidAnswer, $"songs.{at}.year is not a whole number");
                year = number;
            }
            list.Add(new SongSuggestion(title, artist, album, year));
        }
        return list;
    }

    private static string? Text(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String &&
        !string.IsNullOrWhiteSpace(value.GetString())
            ? value.GetString()!.Trim()
            : null;

    // The counts only: the request and the songs are user content (debug at most).
    [LoggerMessage(Level = LogLevel.Debug, Message = "asking {Provider} for {Count} songs: {Seed} in the seed, {Avoid} to avoid")]
    private static partial void LogAsking(ILogger log, string provider, int count, int seed, int avoid);

    [LoggerMessage(Level = LogLevel.Debug, Message = "asking {Provider} to look the request up on the web")]
    private static partial void LogLookingUp(ILogger log, string provider);
}
