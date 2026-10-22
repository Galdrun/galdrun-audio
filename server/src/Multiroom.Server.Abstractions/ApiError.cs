using System.Text.Json.Serialization;

namespace Multiroom.Server.Abstractions;

// The outward error shape, {error, message}, for every caller of this server.
// `error` is the failing module's code in lower camel case -- the spelling the
// core's own seven codes use on 4713 (`invalidArgument`, `notFound`, ...), so a
// code relayed from the core reads the same as one of ours. Which HTTP status
// goes with it is the outward API's decision (phase 10), not the module's.
public sealed record ApiError(
    [property: JsonPropertyName("error")] string Error,
    [property: JsonPropertyName("message")] string Message)
{
    public static ApiError From<TCode>(Failure<TCode> failure)
        where TCode : struct, Enum => new(CodeName(failure.Code), failure.Detail);

    private static string CodeName<TCode>(TCode code)
        where TCode : struct, Enum
    {
        var name = code.ToString();
        return char.ToLowerInvariant(name[0]) + name[1..];
    }
}
