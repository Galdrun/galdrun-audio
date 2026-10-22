using System.Globalization;
using System.Text.Json;

namespace Multiroom.Server.Providers.Tidal;

// Reading Tidal's JSON by hand, as the catalog mapping needs only a few
// fields of each answer and a missing one is null, never an exception.
internal static class TidalJson
{
    public static JsonElement? Get(this JsonElement element, string name) =>
        element.ValueKind == JsonValueKind.Object && element.TryGetProperty(name, out var value) &&
        value.ValueKind is not (JsonValueKind.Null or JsonValueKind.Undefined)
            ? value
            : null;

    public static string? Text(this JsonElement element, string name) => element.Get(name) switch
    {
        { ValueKind: JsonValueKind.String } value => value.GetString() is { Length: > 0 } text ? text : null,
        { ValueKind: JsonValueKind.Number } value => value.GetRawText(),
        _ => null,
    };

    public static long? Number(this JsonElement element, string name) => element.Get(name) switch
    {
        { ValueKind: JsonValueKind.Number } value when value.TryGetInt64(out var number) => number,
        { ValueKind: JsonValueKind.String } value when long.TryParse(value.GetString(), NumberStyles.Integer,
            CultureInfo.InvariantCulture, out var number) => number,
        _ => null,
    };

    public static IEnumerable<JsonElement> Items(this JsonElement element, string name = "items") =>
        element.Get(name) is { ValueKind: JsonValueKind.Array } array ? array.EnumerateArray() : [];
}
