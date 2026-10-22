namespace Multiroom.Server.Providers.TuneIn;

// providers.tunein, resolved. A class, not a record: a record's ToString would
// print the partner id (rule 7). Locale is TuneIn's `locale` (de-DE); null
// lets TuneIn choose.
public sealed class TuneInSettings(string? partnerId, string? locale)
{
    public string? PartnerId { get; } = partnerId;

    public string? Locale { get; } = locale;
}

// Where TuneIn's OPML API is. Only a test points it anywhere else.
public sealed record TuneInEndpoints(Uri Api)
{
    public static TuneInEndpoints Default { get; } = new(new Uri("https://opml.radiotime.com/"));
}
