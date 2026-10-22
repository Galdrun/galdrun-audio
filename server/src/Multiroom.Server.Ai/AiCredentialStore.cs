using System.Text.Json;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Ai;

public enum AiKeyOrigin
{
    None,

    // ai.providers in the config file: read-only through the API.
    Config,

    // Set through the API, kept in <dataDirectory>/ai/credentials.json.
    Api,
}

// A provider's block of the config's ai.providers, resolved: its key and its
// base URL, each optional. A class, not a record: a record's ToString would
// print the key (rule 7).
public sealed class AiProviderConfig(string id, string? apiKey, Uri? baseUrl)
{
    public string Id { get; } = id;

    public string? ApiKey { get; } = apiKey;

    public Uri? BaseUrl { get; } = baseUrl;
}

// The AI providers' keys: the config's, which win and are read-only, then
// those set through the API, in an owner-only file as the Tidal token and the
// SMB passwords are (phase 13's decision; no DPAPI). A key goes in no log
// line, no status and no answer: the API answers only whether there is one.
public sealed class AiCredentialStore
{
    private readonly string _path;
    private readonly Dictionary<string, string> _config;
    private readonly Lock _lock = new();
    private Dictionary<string, StoredAiCredential> _stored;

    public AiCredentialStore(string path, IEnumerable<AiProviderConfig> config)
    {
        _path = path;
        _config = config.Where(c => c.ApiKey is not null).ToDictionary(c => c.Id, c => c.ApiKey!, StringComparer.Ordinal);
        _stored = Read(path);
    }

    public string? Key(string id)
    {
        if (_config.TryGetValue(id, out var key))
            return key;
        lock (_lock)
            return _stored.TryGetValue(id, out var stored) ? stored.ApiKey : null;
    }

    public AiKeyOrigin Origin(string id)
    {
        if (_config.ContainsKey(id))
            return AiKeyOrigin.Config;
        lock (_lock)
            return _stored.TryGetValue(id, out var stored) && (stored.ApiKey ?? stored.AccessToken) is not null
                ? AiKeyOrigin.Api
                : AiKeyOrigin.None;
    }

    public Result<bool, AiError> Set(string id, string key)
    {
        if (_config.ContainsKey(id))
            return Result.Fail(AiError.InvalidArgument, $"the key of {id} is from the config file; change it there");
        if (string.IsNullOrWhiteSpace(key))
            return Result.Fail(AiError.InvalidArgument, "key: the provider's API key");
        return Update(id, old => new StoredAiCredential
        {
            ApiKey = key.Trim(),
            ClientId = old?.ClientId,
            ClientSecret = old?.ClientSecret,
        });
    }

    // A login's credential (phase 22): a key replaces any token and a token
    // any key, so a provider is authorised one way at a time. The OAuth
    // client stays, unless the vendor issued one with the token (phase 28).
    public Result<bool, AiError> SetCredential(string id, AiCredential credential)
    {
        if (credential.ApiKey is { } key)
            return Set(id, key);
        if (_config.ContainsKey(id))
            return Result.Fail(AiError.InvalidArgument, $"the key of {id} is from the config file; change it there");
        return Update(id, old => new StoredAiCredential
        {
            AccessToken = credential.AccessToken,
            RefreshToken = credential.RefreshToken,
            ExpiresAt = credential.ExpiresAt,
            RefreshAfter = credential.RefreshAfter,
            ClientId = credential.Client?.ClientId ?? old?.ClientId,
            ClientSecret = credential.Client is { } issued ? issued.ClientSecret : old?.ClientSecret,
        });
    }

    // The login's token, null when there is none.
    public AiCredential? Token(string id)
    {
        lock (_lock)
        {
            return _stored.TryGetValue(id, out var stored) && stored.AccessToken is not null
                ? new AiCredential
                {
                    AccessToken = stored.AccessToken,
                    RefreshToken = stored.RefreshToken,
                    ExpiresAt = stored.ExpiresAt,
                    RefreshAfter = stored.RefreshAfter,
                }
                : null;
        }
    }

    // True when there was a key or a token. The OAuth client stays: logging
    // out is not forgetting the app.
    public Result<bool, AiError> Remove(string id)
    {
        if (_config.ContainsKey(id))
            return Result.Fail(AiError.InvalidArgument, $"the key of {id} is from the config file; change it there");
        return Update(id, old => old is null or { ApiKey: null, AccessToken: null }
            ? old
            : Keep(new StoredAiCredential { ClientId = old.ClientId, ClientSecret = old.ClientSecret }));
    }

    public AiOAuthClient? Client(string id)
    {
        lock (_lock)
        {
            return _stored.TryGetValue(id, out var stored) && stored.ClientId is not null
                ? new AiOAuthClient(stored.ClientId, stored.ClientSecret)
                : null;
        }
    }

    // Null forgets it; a key or a token stays.
    public Result<bool, AiError> SetClient(string id, AiOAuthClient? client) =>
        Update(id, old => old is null && client is null ? null : Keep(new StoredAiCredential
        {
            ApiKey = old?.ApiKey,
            AccessToken = old?.AccessToken,
            RefreshToken = old?.RefreshToken,
            ExpiresAt = old?.ExpiresAt,
            RefreshAfter = old?.RefreshAfter,
            ClientId = client?.ClientId,
            ClientSecret = client?.ClientSecret,
        }));

    // An entry with nothing left in it is no entry.
    private static StoredAiCredential? Keep(StoredAiCredential credential) =>
        credential is { ApiKey: null, AccessToken: null, ClientId: null } ? null : credential;

    // The entry changed and the file written; false when nothing changed
    // (the old entry answered, or none before and none after).
    private Result<bool, AiError> Update(string id, Func<StoredAiCredential?, StoredAiCredential?> change)
    {
        lock (_lock)
        {
            var old = _stored.GetValueOrDefault(id);
            var credential = change(old);
            if (ReferenceEquals(credential, old))
                return false;
            var next = new Dictionary<string, StoredAiCredential>(_stored, StringComparer.Ordinal);
            if (credential is null)
                next.Remove(id);
            else
                next[id] = credential;
            try
            {
                OwnerOnlyFile.Write(_path, JsonSerializer.SerializeToUtf8Bytes(next));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                return Result.Fail(AiError.Unavailable, $"the AI credentials cannot be written: {e.Message}");
            }
            _stored = next;
            return true;
        }
    }

    // A file that cannot be read leaves every API key unset: the providers
    // then say they are not configured.
    private static Dictionary<string, StoredAiCredential> Read(string path)
    {
        try
        {
            var read = JsonSerializer.Deserialize<Dictionary<string, StoredAiCredential>>(File.ReadAllBytes(path));
            return new Dictionary<string, StoredAiCredential>(read ?? [], StringComparer.Ordinal);
        }
        catch (Exception e) when (e is FileNotFoundException or DirectoryNotFoundException or JsonException
            or IOException or UnauthorizedAccessException)
        {
            return new Dictionary<string, StoredAiCredential>(StringComparer.Ordinal);
        }
    }

    // One provider's entry in the file: a key or a login's tokens, and the
    // owner's OAuth client. A class, not a record, for the same reason as
    // AiProviderConfig.
    private sealed class StoredAiCredential
    {
        public string? ApiKey { get; set; }

        public string? AccessToken { get; set; }

        public string? RefreshToken { get; set; }

        public DateTimeOffset? ExpiresAt { get; set; }

        public DateTimeOffset? RefreshAfter { get; set; }

        public string? ClientId { get; set; }

        public string? ClientSecret { get; set; }
    }
}
