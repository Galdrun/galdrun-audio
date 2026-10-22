using System.Text.Json;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Ai;

// What the AI is told, kept out of the code (phases 24 and 25): the song
// suggester's and the agent's prompts, read from a JSON file so they can be
// changed without a build. `ai-instructions.json` ships beside the
// executable; `<dataDirectory>/ai/instructions.json`, when present, replaces
// it whole (an update of the server does not overwrite it). Placeholders in
// braces are filled by the code: {request}, {count}, {zone}. Phase 27 adds
// the web lookup's system prompt and the heading its notes go under; phase 38
// the proposals' section (an override from before it must add one, or the
// server stops at start, named).
public sealed record AiInstructions(SongInstructions Songs, AgentInstructions Agent, ProposalInstructions Proposals)
{
    public const string FileName = "ai-instructions.json";

    private static readonly JsonSerializerOptions Json = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        RespectNullableAnnotations = true,
        RespectRequiredConstructorParameters = true,
        UnmappedMemberHandling = System.Text.Json.Serialization.JsonUnmappedMemberHandling.Disallow,
        AllowTrailingCommas = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
    };

    // The override when it exists, else the shipped file. A file that cannot
    // be read, or lacks a field, is refused by name: the code holds no
    // fallback text.
    public static Result<AiInstructions, AiError> Load(string shipped, string overriding)
    {
        var path = File.Exists(overriding) ? overriding : shipped;
        try
        {
            return JsonSerializer.Deserialize<AiInstructions>(File.ReadAllBytes(path), Json) is { } read
                ? read
                : Result.Fail(AiError.NotConfigured, $"{path} is empty");
        }
        catch (Exception e) when (e is JsonException or IOException or UnauthorizedAccessException)
        {
            return Result.Fail(AiError.NotConfigured, $"the AI instructions cannot be read from {path}: {e.Message}");
        }
    }

    public static string Fill(string text, string name, string value) =>
        text.Replace("{" + name + "}", value, StringComparison.Ordinal);
}

public sealed record SongInstructions(
    string System,
    string ToolDescription,
    string ArtistDescription,
    string Request,
    string Seed,
    string Avoid,
    string Count,
    string Preferred,
    string LookupSystem,
    string LookupNotes);

// Added to the system prompt where they apply, and an override without one
// simply tells the agent nothing: WebSearch when the agent has the vendor's
// web search (phase 27), User ({user}) when the command names who speaks,
// AskBack when it is part of a conversation, where a question can be answered.
public sealed record AgentInstructions(string System, string Zone, string NoZone, string? WebSearch = null,
    string? User = null, string? AskBack = null);

// Phase 38: what the proposals ask beside the songs (which go through
// SongInstructions with Request). Count, Albums/Artists/Audiobooks take
// {count}; Request takes {request}.
public sealed record ProposalInstructions(
    string System,
    string DefaultRequest,
    string Request,
    string SeedTracks,
    string SeedAlbums,
    string SeedArtists,
    string SeedAudiobooks,
    string Played,
    string Avoid,
    string Albums,
    string Artists,
    string Audiobooks,
    string AlbumsTool,
    string ArtistsTool,
    string AudiobooksTool,
    string ReasonDescription);
