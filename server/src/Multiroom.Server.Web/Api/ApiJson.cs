using System.Reflection;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.Json.Serialization.Metadata;
using Microsoft.AspNetCore.Http;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Playback.Core;
using Multiroom.Server.Providers.Covers;

namespace Multiroom.Server.Web.Api;

// The outward API's own failures: those of a request, before any module is
// asked. Every other code in an answer is the failing module's.
public enum ApiRequestError
{
    InvalidRequest,
    Unauthorized,
    NotFound,
    MethodNotAllowed,
    Timeout,
}

// JSON both ways: camel case, enums as camel-case names, absent where
// unknown. A request body is read strictly, the core's discipline applied to
// our own callers: an unknown field is refused by name (`tracks.0.titel`),
// and so is a required one that is missing.
internal static class ApiJson
{
    // The largest body a request may carry: an M3U import is the big one.
    public const long MaxBodyBytes = 4 * 1024 * 1024;

    public static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        RespectNullableAnnotations = true,
        RespectRequiredConstructorParameters = true,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
        TypeInfoResolver = Resolver(),
        Converters =
        {
            new AudioControlJsonConverter(),
            new JsonStringEnumConverter(JsonNamingPolicy.CamelCase, allowIntegerValues: false),
            new QueueItemIdConverter(),
        },
    };

    // The body as T. An empty body is null when `optional`, and refused otherwise.
    public static async Task<Result<T?, ApiRequestError>> ReadAsync<T>(HttpRequest request, bool optional,
        CancellationToken cancellationToken)
        where T : class
    {
        using var buffer = new MemoryStream();
        await request.Body.CopyToAsync(buffer, cancellationToken).ConfigureAwait(false);
        if (buffer.Length == 0)
        {
            return optional
                ? (T?)null
                : Result.Fail(ApiRequestError.InvalidRequest, "this request needs a JSON body");
        }

        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(buffer.GetBuffer().AsMemory(0, (int)buffer.Length));
        }
        catch (JsonException e)
        {
            return Result.Fail(ApiRequestError.InvalidRequest, $"the body is not JSON: {e.Message}");
        }

        using (document)
        {
            if (document.RootElement.ValueKind != JsonValueKind.Object)
                return Result.Fail(ApiRequestError.InvalidRequest, "the body must be a JSON object");
            if (Refusal(document.RootElement, typeof(T), "") is { } refusal)
                return Result.Fail(ApiRequestError.InvalidRequest, refusal);
            try
            {
                return document.RootElement.Deserialize<T>(Options);
            }
            catch (JsonException e)
            {
                var field = e.Path is null or "$" ? "the body" : $"field {e.Path.TrimStart('$', '.')}";
                return Result.Fail(ApiRequestError.InvalidRequest, $"{field} has a value of the wrong kind");
            }
        }
    }

    // The first field no member of `type` names, or the first required one
    // missing, as a dotted path; null when there is none.
    private static string? Refusal(JsonElement element, Type type, string prefix)
    {
        // Binding dictionaries use gesture names as keys, rather than CLR
        // property names. Their values still receive strict field validation.
        if (type.IsGenericType && type.GetGenericTypeDefinition() == typeof(Dictionary<,>) &&
            type.GetGenericArguments()[0] == typeof(string))
        {
            var valueType = type.GetGenericArguments()[1];
            foreach (var field in element.EnumerateObject())
                if (Nested(field.Value, valueType, prefix + field.Name) is { } refusal)
                    return refusal;
            return null;
        }
        var constructor = type.GetConstructors().MaxBy(c => c.GetParameters().Length);
        var parameters = constructor?.GetParameters() ?? [];
        // A read-only property (a station's mediaType) is a member too: a
        // caller may send back what it was answered, and it is ignored.
        var members = type.GetProperties(BindingFlags.Public | BindingFlags.Instance)
            .ToDictionary(p => JsonNamingPolicy.CamelCase.ConvertName(p.Name), StringComparer.Ordinal);

        foreach (var field in element.EnumerateObject())
        {
            var path = prefix + field.Name;
            if (!members.TryGetValue(field.Name, out var member))
            {
                // An item answered in a list names its kind (phase 40); sent back, it is ignored.
                if (field.Name == "kind" && typeof(MediaItem).IsAssignableFrom(type))
                    continue;
                return $"unknown field {path}";
            }
            if (Nested(field.Value, member.PropertyType, path) is { } nested)
                return nested;
        }
        foreach (var parameter in parameters.Where(p => !p.HasDefaultValue))
        {
            var name = JsonNamingPolicy.CamelCase.ConvertName(parameter.Name!);
            if (!element.TryGetProperty(name, out _))
                return $"field {prefix}{name} is required";
        }
        return null;
    }

    private static string? Nested(JsonElement value, Type type, string path)
    {
        type = Nullable.GetUnderlyingType(type) ?? type;
        if (value.ValueKind == JsonValueKind.Object && IsObject(type))
            return Refusal(value, type, path + ".");
        if (value.ValueKind == JsonValueKind.Array && ElementOf(type) is { } item && IsObject(item))
        {
            var index = 0;
            foreach (var entry in value.EnumerateArray())
            {
                if (entry.ValueKind == JsonValueKind.Object && Refusal(entry, item, $"{path}.{index}.") is { } refusal)
                    return refusal;
                index++;
            }
        }
        return null;
    }

    private static bool IsObject(Type type) =>
        type.IsClass && type != typeof(string) && type != typeof(Uri) && ElementOf(type) is null;

    private static Type? ElementOf(Type type) =>
        type.IsGenericType && type.GetGenericTypeDefinition() is var generic &&
        (generic == typeof(IReadOnlyList<>) || generic == typeof(List<>))
            ? type.GetGenericArguments()[0]
            : null;

    // Phase 40: an item is answered with its artworkId, never its provider's
    // artwork reference (that stays the store's: ArtworkService), and noted
    // as it is written, so a request for its picture can be answered. A
    // list's entry (IMediaEntry) is answered with its one item as `item`,
    // its kind named, instead of the field that holds it. Read, both are
    // taken as before: a caller may send back what it was answered.
    // A resolver with that contract: the MCP tools answer as the routes do.
    public static DefaultJsonTypeInfoResolver Resolver() => new() { Modifiers = { MediaContract } };

    private static readonly HashSet<string> EntryFields =
        new(["track", "album", "artist", "station", "group", "audiobook"], StringComparer.Ordinal);

    private static void MediaContract(JsonTypeInfo info)
    {
        if (info.Kind != JsonTypeInfoKind.Object)
            return;
        if (typeof(MediaItem).IsAssignableFrom(info.Type))
        {
            foreach (var property in info.Properties.Where(p => p.Name == "artwork"))
                property.ShouldSerialize = static (_, _) => false;
            info.OnSerializing = static item => ArtworkHints.Note((MediaItem)item);
            // Its kind, sent back, is passed over; Refusal has refused any other unknown field.
            info.UnmappedMemberHandling = JsonUnmappedMemberHandling.Skip;
        }
        if (typeof(IMediaEntry).IsAssignableFrom(info.Type))
        {
            foreach (var property in info.Properties.Where(p => EntryFields.Contains(p.Name)))
                property.ShouldSerialize = static (_, _) => false;
            // The entry's own Item is ignored where the entry is stored; here it is the answer.
            foreach (var own in info.Properties.Where(p => p.Name == "item").ToList())
                info.Properties.Remove(own);
            var item = info.CreateJsonPropertyInfo(typeof(MediaItem), "item");
            item.Get = static entry => ((IMediaEntry)entry).Item;
            info.Properties.Insert(1, item);
        }
    }

    // A queue item's id is a plain number on the wire.
    private sealed class QueueItemIdConverter : JsonConverter<QueueItemId>
    {
        public override QueueItemId Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options) =>
            new(reader.GetInt64());

        public override void Write(Utf8JsonWriter writer, QueueItemId value, JsonSerializerOptions options) =>
            writer.WriteNumberValue(value.Value);
    }
}
