using System.Globalization;
using System.Text;

namespace Multiroom.Server.Abstractions;

// How a provider that holds a whole list orders it by name, filters it by
// text and indexes it by letter, so every such list behaves alike. Case and
// diacritics do not count: "édith" sorts, filters and indexes as "Edith".
public static class BrowseIndex
{
    private static readonly CompareInfo Compare = CultureInfo.InvariantCulture.CompareInfo;
    private const CompareOptions Loose = CompareOptions.IgnoreCase | CompareOptions.IgnoreNonSpace;

    // Names in order, without case or diacritics.
    public static StringComparer Order { get; } = StringComparer.Create(CultureInfo.InvariantCulture, Loose);

    // "A" to "Z" (any letter, upper case, its diacritics gone), or "#" for a
    // name that starts with anything else.
    public static string LetterOf(string name)
    {
        var first = name.AsSpan().TrimStart();
        if (first.IsEmpty || !char.IsLetter(first[0]))
            return "#";
        var plain = first[0].ToString().Normalize(NormalizationForm.FormD)[0];
        return char.ToUpperInvariant(plain).ToString();
    }

    // What a request may name as a letter.
    public static bool IsLetter(string text) => text == "#" || (text.Length == 1 && char.IsLetter(text[0]) &&
        LetterOf(text) == text);

    // Where each letter first appears in names already in Order.
    public static IReadOnlyList<BrowseLetter> Of(IEnumerable<string> names)
    {
        var letters = new List<BrowseLetter>();
        var index = 0;
        foreach (var name in names)
        {
            var letter = LetterOf(name);
            if (!letters.Exists(l => l.Letter == letter))
                letters.Add(new BrowseLetter(letter, index));
            index++;
        }
        return letters;
    }

    // Where a letter jumps to: its first entry, or the first of the next
    // letter there is, or the end.
    public static int OffsetOf(IReadOnlyList<BrowseLetter> letters, string letter, int total) =>
        letters.FirstOrDefault(l => l.Letter == letter)?.Offset ??
        letters.FirstOrDefault(l => Order.Compare(l.Letter, letter) > 0)?.Offset ??
        total;

    // Whether any of the texts holds the filter.
    public static bool Matches(string filter, params string?[] texts) =>
        texts.Any(t => t is not null && Compare.IndexOf(t, filter, Loose) >= 0);
}
