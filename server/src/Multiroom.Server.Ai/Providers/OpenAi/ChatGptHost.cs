using System.Text.Json;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Ai.Providers.OpenAi;

// This installation's ext_agent_host_id (phase 28): `urn:uuid:` and a new
// GUID, made before the first sign-in and kept for the installation's life in
// <dataDirectory>/ai/chatgpt-host.json, owner-only. A sign-in, a logout or an
// update never changes it. It goes in no log line and no answer (rule 7).
public sealed class ChatGptHost(string path)
{
    public const string FileName = "chatgpt-host.json";

    private readonly Lock _lock = new();
    private string? _id;

    // The id, made and written the first time. A file that exists but cannot
    // be read is a failure, never replaced: a new id would be a new host.
    public Result<string, AiError> Id()
    {
        lock (_lock)
        {
            if (_id is not null)
                return _id;
            try
            {
                using var document = JsonDocument.Parse(File.ReadAllBytes(path));
                if (document.RootElement.Text("hostId") is { Length: > 0 } read)
                    return _id = read;
                return Result.Fail(AiError.Unavailable, $"{path} holds no hostId");
            }
            catch (Exception e) when (e is FileNotFoundException or DirectoryNotFoundException)
            {
            }
            catch (Exception e) when (e is JsonException or IOException or UnauthorizedAccessException)
            {
                return Result.Fail(AiError.Unavailable, $"{path} cannot be read: {e.Message}");
            }

            var id = $"urn:uuid:{Guid.NewGuid():D}";
            try
            {
                OwnerOnlyFile.Write(path, JsonSerializer.SerializeToUtf8Bytes(new Dictionary<string, string> { ["hostId"] = id }));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                return Result.Fail(AiError.Unavailable, $"{path} cannot be written: {e.Message}");
            }
            return _id = id;
        }
    }
}
