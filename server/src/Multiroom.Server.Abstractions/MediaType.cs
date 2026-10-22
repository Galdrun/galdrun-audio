using System.Globalization;
using System.Text;

namespace Multiroom.Server.Abstractions;

// What an item is. Music is any track that is
// not spoken word: a song, a symphony, an instrumental. A station is Radio by
// its kind. No Podcast: nothing serves one yet (-> Open decisions).
public enum MediaType
{
    Music,
    Audiobook,
    Radio,
}

// Which rule of the classifier decided: the owner's (a folder's declared
// type), the provider's own, a tag (MP4 `stik`), the container (`.m4b`) or
// the genre.
public enum MediaTypeSource
{
    Folder,
    Provider,
    Tag,
    Extension,
    Genre,
}

// What a caller already holds about one track: the type the owner declared
// for its folder, the provider's own type, the MP4 media kind (`stik`), the
// file's extension and its genre. Any of them may be unknown.
public sealed record MediaTypeEvidence(
    MediaType? Declared = null,
    MediaType? Provider = null,
    int? MediaKind = null,
    string? Extension = null,
    string? Genre = null);

public sealed record MediaTypeVerdict(MediaType? Type, MediaTypeSource? Source)
{
    public static MediaTypeVerdict None { get; } = new(null, null);
}

// The one place that says what an item is: a pure function over evidence,
// the first rule that answers wins. A provider with types of its own passes
// its hint as Provider, so the order stays here.
public static class MediaTypeClassifier
{
    // MP4's `stik` for an audiobook.
    public const int AudiobookMediaKind = 2;

    // Folded as the song resolver folds (case, accents); one list, not a
    // setting (rule 5).
    private static readonly HashSet<string> SpokenGenres = new(StringComparer.Ordinal)
    {
        "audiobook", "audio book", "horbuch", "horspiel", "spoken word", "speech",
    };

    public static MediaTypeVerdict Classify(MediaTypeEvidence evidence)
    {
        if (evidence.Declared is { } declared)
            return new(declared, MediaTypeSource.Folder);
        if (evidence.Provider is { } provider)
            return new(provider, MediaTypeSource.Provider);
        if (evidence.MediaKind == AudiobookMediaKind)
            return new(MediaType.Audiobook, MediaTypeSource.Tag);
        if (string.Equals(evidence.Extension, ".m4b", StringComparison.OrdinalIgnoreCase))
            return new(MediaType.Audiobook, MediaTypeSource.Extension);
        if (evidence.Genre is { } genre && genre.Split(['/', ';', ',']).Any(g => SpokenGenres.Contains(Fold(g))))
            return new(MediaType.Audiobook, MediaTypeSource.Genre);
        return MediaTypeVerdict.None;
    }

    // The first word of a chapter's title, folded: "Kapitel 3", "Chapter 12".
    private static readonly HashSet<string> ChapterWords = new(StringComparer.Ordinal)
    {
        "kapitel", "chapter", "chapitre", "capitulo", "capitolo", "hoofdstuk", "teil", "folge", "abschnitt",
    };

    // The share of a container's tracks that must read as chapters, from
    // MinChapterTracks tracks on.
    private const double ChapterShare = 0.8;
    private const int MinChapterTracks = 3;

    // A container whose provider tags nothing (a Tidal album): spoken word
    // when most of its track titles are chapters ("Kapitel 1.1 - Der
    // Hobbit"). Its title alone decides nothing (the owner's rule: a title
    // saying "Hörbuch" with tracks that are no chapters is not taken). A
    // heuristic, so a provider passes its answer as its own hint
    // (MediaTypeEvidence.Provider); the rule order stays Classify's.
    public static bool LooksSpoken(IReadOnlyList<string> trackTitles)
    {
        if (trackTitles.Count < MinChapterTracks)
            return false;
        var chapters = trackTitles.Count(t => Fold(t).Split(' ') is [var word, var number, ..] &&
            ChapterWords.Contains(word) && number.All(char.IsAsciiDigit));
        return chapters >= ChapterShare * trackTitles.Count;
    }

    // A track nothing says anything about is music wherever a decision is
    // needed.
    public static bool IsMusic(Track track) => track.MediaType is null or MediaType.Music;

    // Lower case, no accents, runs of anything but letters and digits one space.
    private static string Fold(string text)
    {
        var folded = new StringBuilder(text.Length);
        var space = false;
        foreach (var c in text.Normalize(NormalizationForm.FormD))
        {
            if (CharUnicodeInfo.GetUnicodeCategory(c) == UnicodeCategory.NonSpacingMark)
                continue;
            if (char.IsLetterOrDigit(c))
            {
                if (space && folded.Length > 0)
                    folded.Append(' ');
                folded.Append(char.ToLowerInvariant(c));
                space = false;
            }
            else
            {
                space = true;
            }
        }
        return folded.ToString();
    }
}
