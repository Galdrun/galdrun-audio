using System.Text.Json;
using System.Text.Json.Serialization;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Ai;

// The user's AI settings, set from an app: the provider and model a command
// uses when it names none, and the providers switched off. Every provider is
// on until switched off. Phase 24 adds where found songs come from
// (SongProviders, music provider ids in order; null is the default order) and
// how many rounds a suggestion may take; phase 25 the agent's limits. The
// tool permissions are not here but in ai/mcp.json (phase 23). Phase 27 adds
// whether the suggestions and the agent may look things up on the web; phase
// 38 the proposals.
public sealed record AiSettings(
    string? DefaultProvider = null,
    string? DefaultModel = null,
    IReadOnlyList<string>? DisabledProviders = null,
    IReadOnlyList<string>? SongProviders = null,
    int? MaxSuggestionRounds = null,
    AgentLimits? Agent = null,
    AiReasoningSettings? Reasoning = null,
    AiWebLookup? WebLookup = null,
    ProposalSettings? Proposals = null,
    AiSpeechSettings? Speech = null)
{
    public const int DefaultSuggestionRounds = 2;

    [JsonIgnore]
    public IReadOnlyList<string> Disabled => DisabledProviders ?? [];

    [JsonIgnore]
    public int SuggestionRounds => MaxSuggestionRounds ?? DefaultSuggestionRounds;

    [JsonIgnore]
    public AgentLimits Limits => Agent ?? new AgentLimits();

    [JsonIgnore]
    public AiReasoningSettings ReasoningLevels => Reasoning ?? new AiReasoningSettings();

    [JsonIgnore]
    public AiWebLookup Lookup => WebLookup ?? AiWebLookup.Off;

    [JsonIgnore]
    public ProposalSettings ProposalOptions => Proposals ?? new ProposalSettings();

    [JsonIgnore]
    public AiSpeechSettings SpeechOptions => Speech ?? new AiSpeechSettings();
}

// Who turns the assistant's microphone into words: the browser's own speech
// recognition, or this server, through a provider's speech model.
public enum AiSpeechRecognition
{
    Browser,
    Server,
}

// The assistant's voice: how speech is recognised, the provider and model
// the server transcribes with (Recognition server), the language spoken
// (BCP 47; null is the browser's), and whether replies to spoken messages
// are read aloud.
public sealed record AiSpeechSettings(
    AiSpeechRecognition Recognition = AiSpeechRecognition.Browser,
    string? Provider = null,
    string? Model = null,
    string? Language = null,
    bool ReadReplies = false);

// Null keeps a setting; an empty provider, model or language clears it.
public sealed record AiSpeechPatch(
    AiSpeechRecognition? Recognition = null,
    string? Provider = null,
    string? Model = null,
    string? Language = null,
    bool? ReadReplies = null);

// Whether a model may look things up on the web (phase 27): never (off, the
// default: a lookup costs per search on most vendors), or on every provider
// that has a search of its own (whenAvailable), for the suggestions (AutoFill's
// too) and the agent's commands.
public enum AiWebLookup
{
    Off,
    WhenAvailable,
}

// How hard a model thinks, by use (phase 29): the agent's commands, and the
// song suggestions (AutoFill's too, phase 26). A request's own level wins.
public sealed record AiReasoningSettings(AiReasoning Agent = AiReasoning.Default,
    AiReasoning Suggestions = AiReasoning.Default);

public sealed record AiReasoningPatch(AiReasoning? Agent = null, AiReasoning? Suggestions = null);

// The proposals (phase 38): whether they run in the background and how
// often, at most how many runs a day (every scope counted), and how many of
// each kind a run proposes (0 asks for none). Request is what is asked, the
// instructions' defaultRequest when null. A manual refresh runs whatever
// Enabled says. Runs add to the lists; Keep is how many of each kind they
// hold, the oldest going first.
public sealed record ProposalSettings(
    bool Enabled = false,
    int IntervalHours = 24,
    int DebounceMinutes = 10,
    int MaxRunsPerDay = 4,
    int Tracks = 25,
    int Albums = 10,
    int Artists = 10,
    int Audiobooks = 5,
    string? Request = null,
    int Keep = 200);

public sealed record ProposalSettingsPatch(
    bool? Enabled = null,
    int? IntervalHours = null,
    int? DebounceMinutes = null,
    int? MaxRunsPerDay = null,
    int? Tracks = null,
    int? Albums = null,
    int? Artists = null,
    int? Audiobooks = null,
    string? Request = null,
    int? Keep = null);

// What one agent command may cost (phase 25): tool calls, wall time, tokens,
// and how many commands run at once.
public sealed record AgentLimits(int MaxToolCalls = 12, int TimeoutSeconds = 60, long TokenBudget = 50_000,
    int MaxConcurrent = 2);

// Null keeps a setting; an empty default provider or model clears it; a list
// is the full list, never a delta, and an empty song provider list is the
// default order again.
public sealed record AiSettingsPatch(
    string? DefaultProvider = null,
    string? DefaultModel = null,
    IReadOnlyList<string>? DisabledProviders = null,
    IReadOnlyList<string>? SongProviders = null,
    int? MaxSuggestionRounds = null,
    AgentLimitsPatch? Agent = null,
    AiReasoningPatch? Reasoning = null,
    AiWebLookup? WebLookup = null,
    ProposalSettingsPatch? Proposals = null,
    AiSpeechPatch? Speech = null);

public sealed record AgentLimitsPatch(int? MaxToolCalls = null, int? TimeoutSeconds = null, long? TokenBudget = null,
    int? MaxConcurrent = null);

// <dataDirectory>/ai/settings.json, written whole. A file that cannot be read
// is logged by the registry and the defaults are used; it is replaced by the
// next change, never fatal.
public sealed class AiSettingsStore(string path)
{
    private static readonly JsonSerializerOptions Json = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        Converters = { new JsonStringEnumConverter(JsonNamingPolicy.CamelCase) },
    };

    private readonly Lock _lock = new();
    private AiSettings _current = new();

    public AiSettings Current
    {
        get
        {
            lock (_lock)
                return _current;
        }
    }

    // The file's settings, or the failure that left the defaults in place.
    public Result<AiSettings, AiError> Load()
    {
        AiSettings? read;
        try
        {
            read = JsonSerializer.Deserialize<AiSettings>(File.ReadAllBytes(path), Json);
        }
        catch (Exception e) when (e is FileNotFoundException or DirectoryNotFoundException)
        {
            return Current;
        }
        catch (Exception e) when (e is JsonException or IOException or UnauthorizedAccessException)
        {
            return Result.Fail(AiError.Unavailable, $"{path}: {e.Message}");
        }
        lock (_lock)
            _current = read ?? new AiSettings();
        return Current;
    }

    public Result<AiSettings, AiError> Save(AiSettings settings)
    {
        lock (_lock)
        {
            try
            {
                OwnerOnlyFile.Write(path, JsonSerializer.SerializeToUtf8Bytes(settings, Json));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                return Result.Fail(AiError.Unavailable, $"the AI settings cannot be written: {e.Message}");
            }
            _current = settings;
            return settings;
        }
    }
}
