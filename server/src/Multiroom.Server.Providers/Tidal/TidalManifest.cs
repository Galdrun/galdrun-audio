using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Xml;
using System.Xml.Linq;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Providers.Tidal;

// A track's playback manifest, as tracks/<id>/playbackinfopostpaywall answers
// it: where the bytes are. Either one file (Tidal's own "BTS" manifest, a
// JSON list of URLs) or MPEG-DASH segments (an init segment and numbered
// media segments, which concatenated are one fragmented MP4). The segment
// URLs are signed: they go in no message and no log line (rule 7).
//
// Encrypted manifests are refused. Decryption would belong here and nowhere
// else, but no tier this server asks for is encrypted today.
//
// Grant is the tier Tidal answered with, which may be lower than the one
// asked for (TidalProvider.GrantOf); the cache keeps it with the file.
internal sealed record TidalManifest(string ContentType, IReadOnlyList<Uri> Segments, string Grant = TidalManifest.UnknownGrant)
{
    public const string UnknownGrant = "UNKNOWN";

    public const string BtsType = "application/vnd.tidal.bts";
    public const string DashType = "application/dash+xml";

    public static Result<TidalManifest, ProviderError> Parse(string? mimeType, string? base64, string name)
    {
        string text;
        try
        {
            text = Encoding.UTF8.GetString(Convert.FromBase64String(base64 ?? ""));
        }
        catch (FormatException)
        {
            return Result.Fail(ProviderError.Failed, $"{name}: the manifest is not base64");
        }

        var parsed = mimeType switch
        {
            BtsType => ParseBts(text, name),
            DashType => ParseDash(text, name),
            _ => Result.Fail(ProviderError.Failed, $"{name}: unknown manifest type {mimeType ?? "(none)"}"),
        };
        return parsed.IsSuccess && parsed.Value.Segments.Count == 0
            ? Result.Fail(ProviderError.Failed, $"{name}: the manifest names no media")
            : parsed;
    }

    private static Result<TidalManifest, ProviderError> ParseBts(string text, string name)
    {
        try
        {
            using var document = JsonDocument.Parse(text);
            var root = document.RootElement;
            if (root.Text("encryptionType") is { } encryption && encryption != "NONE")
                return Result.Fail(ProviderError.Unavailable, $"{name}: the stream is encrypted ({encryption})");
            var urls = new List<Uri>();
            foreach (var url in root.Items("urls"))
            {
                if (url.ValueKind != JsonValueKind.String || !Uri.TryCreate(url.GetString(), UriKind.Absolute, out var uri) ||
                    uri.Scheme is not ("http" or "https"))
                {
                    return Result.Fail(ProviderError.Failed, $"{name}: the manifest holds a URL that is not http");
                }
                urls.Add(uri);
            }
            return new TidalManifest(root.Text("mimeType") ?? "application/octet-stream", urls);
        }
        catch (JsonException)
        {
            return Result.Fail(ProviderError.Failed, $"{name}: the manifest is not JSON");
        }
    }

    // The one SegmentTemplate Tidal's audio MPDs carry: `initialization`,
    // `media` with $Number$, `startNumber`, and a SegmentTimeline whose S
    // elements each stand for 1 + r segments.
    private static Result<TidalManifest, ProviderError> ParseDash(string text, string name)
    {
        XDocument document;
        try
        {
            using var reader = XmlReader.Create(new StringReader(text),
                new XmlReaderSettings { DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null });
            document = XDocument.Load(reader);
        }
        catch (XmlException)
        {
            return Result.Fail(ProviderError.Failed, $"{name}: the manifest is not XML");
        }

        var elements = document.Descendants().ToList();
        if (elements.Any(e => e.Name.LocalName == "ContentProtection"))
            return Result.Fail(ProviderError.Unavailable, $"{name}: the stream is encrypted (DASH ContentProtection)");
        var template = elements.FirstOrDefault(e => e.Name.LocalName == "SegmentTemplate");
        var initialization = template?.Attribute("initialization")?.Value;
        var media = template?.Attribute("media")?.Value;
        if (template is null || initialization is null || media is null)
            return Result.Fail(ProviderError.Failed, $"{name}: the manifest has no segment template");

        var start = 1L;
        if (template.Attribute("startNumber") is { } startNumber &&
            !long.TryParse(startNumber.Value, NumberStyles.None, CultureInfo.InvariantCulture, out start))
        {
            return Result.Fail(ProviderError.Failed, $"{name}: the manifest's startNumber is not a number");
        }
        long count = 0;
        foreach (var s in template.Descendants().Where(e => e.Name.LocalName == "S"))
        {
            var repeat = 0L;
            if (s.Attribute("r") is { } r &&
                (!long.TryParse(r.Value, NumberStyles.None, CultureInfo.InvariantCulture, out repeat) || repeat > 100_000))
            {
                return Result.Fail(ProviderError.Failed, $"{name}: the manifest's timeline has an unusable repeat");
            }
            count += 1 + repeat;
        }
        if (count == 0 || count > 100_000)
            return Result.Fail(ProviderError.Failed, $"{name}: the manifest's timeline has {count} segments");

        var baseUrl = elements.FirstOrDefault(e => e.Name.LocalName == "BaseURL")?.Value.Trim();
        Uri? baseUri = null;
        if (baseUrl is { Length: > 0 } && !Uri.TryCreate(baseUrl, UriKind.Absolute, out baseUri))
            return Result.Fail(ProviderError.Failed, $"{name}: the manifest's BaseURL is not absolute");

        var segments = new List<Uri>((int)count + 1);
        for (var n = -1L; n < count; n++)
        {
            var reference = n < 0
                ? initialization
                : media.Replace("$Number$", (start + n).ToString(CultureInfo.InvariantCulture), StringComparison.Ordinal);
            if (reference.Contains('$', StringComparison.Ordinal))
                return Result.Fail(ProviderError.Failed, $"{name}: the manifest's template has an unsupported identifier");
            if (!(baseUri is null ? Uri.TryCreate(reference, UriKind.Absolute, out var uri) : Uri.TryCreate(baseUri, reference, out uri)) ||
                uri.Scheme is not ("http" or "https"))
            {
                return Result.Fail(ProviderError.Failed, $"{name}: the manifest holds a URL that is not http");
            }
            segments.Add(uri);
        }

        var representation = elements.FirstOrDefault(e => e.Name.LocalName == "Representation");
        var type = representation?.Attribute("mimeType")?.Value ??
            elements.FirstOrDefault(e => e.Name.LocalName == "AdaptationSet")?.Attribute("mimeType")?.Value ?? "audio/mp4";
        return new TidalManifest(type, segments);
    }
}
