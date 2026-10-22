using System.Buffers.Binary;
using System.Text;

namespace Multiroom.Server.Library;

// One chapter mark as a file holds it: its title and where it starts.
internal sealed record ProbedChapter(string Title, long StartMs);

// The chapter marks inside one file (phase 32), read at probe time. MP3:
// ID3v2 CHAP frames, in CTOC order where there is one, through TagLib#.
// MP4: TagLib# reads no chapters, so the boxes are walked here, through the
// folder's own reader (so it works on a NAS) and bounded in what is read:
// the QuickTime chapter text track (tref/chap) first, else Nero's chpl.
//
// A file whose marks cannot be read has no chapters; the probe never fails
// for them. The audio before the first mark belongs to the first chapter,
// starts that do not move forward are dropped, and fewer than two marks are
// no chapters. At most MaxChapters are kept; Cut says more were left out.
internal static class ChapterReader
{
    public const int MaxChapters = 1000;

    // What is read of an MP4: its moov box whole, and one chapter title per
    // sample.
    private const int MaxMoovBytes = 32 * 1024 * 1024;
    private const int MaxTitleBytes = 4096;
    private const int MaxTopLevelBoxes = 64;

    private static readonly string[] Mp4Extensions = [".m4b", ".m4a", ".mp4"];

    public static (IReadOnlyList<ProbedChapter> Chapters, bool Cut) Read(TagLib.File tagged, string path,
        Func<Stream> open)
    {
        List<ProbedChapter> marks;
        try
        {
            if (tagged.GetTag(TagLib.TagTypes.Id3v2) is TagLib.Id3v2.Tag id3)
                marks = FromId3(id3);
            else if (Mp4Extensions.Contains(Path.GetExtension(path), StringComparer.OrdinalIgnoreCase))
                marks = FromMp4(open);
            else
                return ([], false);
        }
#pragma warning disable CA1031 // A file's bytes are untrusted: whatever its chapter boxes throw is "no chapters", never the probe's failure.
        catch (Exception)
#pragma warning restore CA1031
        {
            return ([], false);
        }
        return Tidy(marks);
    }

    private static (IReadOnlyList<ProbedChapter>, bool) Tidy(List<ProbedChapter> marks)
    {
        var kept = new List<ProbedChapter>();
        foreach (var mark in marks)
        {
            if (kept.Count > 0 && mark.StartMs <= kept[^1].StartMs)
                continue;
            var title = string.IsNullOrWhiteSpace(mark.Title)
                ? $"Chapter {kept.Count + 1}"
                : mark.Title.Trim();
            kept.Add(new ProbedChapter(title, kept.Count == 0 ? 0 : mark.StartMs));
        }
        if (kept.Count < 2)
            return ([], false);
        return kept.Count > MaxChapters ? (kept.GetRange(0, MaxChapters), true) : (kept, false);
    }

    // --- ID3v2 --------------------------------------------------------------

    private static List<ProbedChapter> FromId3(TagLib.Id3v2.Tag tag)
    {
        var chapters = tag.GetFrames<TagLib.Id3v2.ChapterFrame>().ToList();
        if (chapters.Count == 0)
            return [];
        var byId = chapters.GroupBy(c => c.Id, StringComparer.Ordinal).ToDictionary(g => g.Key, g => g.First(),
            StringComparer.Ordinal);
        var toc = tag.GetFrames<TagLib.Id3v2.TableOfContentsFrame>().FirstOrDefault(t => t.IsTopLevel);
        IEnumerable<TagLib.Id3v2.ChapterFrame> ordered = toc is { ChapterIds.Count: > 0 }
            ? toc.ChapterIds.Where(byId.ContainsKey).Select(id => byId[id])
            : chapters.OrderBy(c => c.StartMilliseconds);
        return [.. ordered.Select(c => new ProbedChapter(
            c.SubFrames.OfType<TagLib.Id3v2.TextInformationFrame>()
                .FirstOrDefault(f => f.FrameId == "TIT2")?.Text.FirstOrDefault() ?? "",
            c.StartMilliseconds))];
    }

    // --- MP4 ----------------------------------------------------------------

    private static List<ProbedChapter> FromMp4(Func<Stream> open)
    {
        using var stream = open();
        if (!stream.CanSeek || ReadMoov(stream) is not { } moov)
            return [];
        var fromTrack = FromChapterTrack(stream, moov);
        return fromTrack.Count > 0 ? fromTrack : FromChpl(moov);
    }

    // The moov box's body, found among the top-level boxes by their headers.
    private static byte[]? ReadMoov(Stream stream)
    {
        Span<byte> header = stackalloc byte[16];
        long at = 0;
        for (var i = 0; i < MaxTopLevelBoxes && at + 8 <= stream.Length; i++)
        {
            stream.Position = at;
            stream.ReadExactly(header[..8]);
            long size = BinaryPrimitives.ReadUInt32BigEndian(header);
            var type = Encoding.ASCII.GetString(header[4..8]);
            var headerSize = 8;
            if (size == 1)
            {
                stream.ReadExactly(header[8..16]);
                size = checked((long)BinaryPrimitives.ReadUInt64BigEndian(header[8..]));
                headerSize = 16;
            }
            else if (size == 0)
            {
                size = stream.Length - at;
            }
            if (size < headerSize)
                return null;
            if (type == "moov")
            {
                var length = size - headerSize;
                if (length > MaxMoovBytes)
                    return null;
                var body = new byte[length];
                stream.ReadExactly(body);
                return body;
            }
            at += size;
        }
        return null;
    }

    // The children of a box body: their type and body, in order.
    private static IEnumerable<(string Type, ReadOnlyMemory<byte> Body)> Children(ReadOnlyMemory<byte> body)
    {
        var at = 0;
        while (at + 8 <= body.Length)
        {
            var span = body.Span[at..];
            long size = BinaryPrimitives.ReadUInt32BigEndian(span);
            var type = Encoding.ASCII.GetString(span[4..8]);
            var headerSize = 8;
            if (size == 1)
            {
                if (span.Length < 16)
                    yield break;
                size = checked((long)BinaryPrimitives.ReadUInt64BigEndian(span[8..]));
                headerSize = 16;
            }
            else if (size == 0)
            {
                size = span.Length;
            }
            if (size < headerSize || size > span.Length)
                yield break;
            yield return (type, body.Slice(at + headerSize, (int)size - headerSize));
            at += (int)size;
        }
    }

    private static ReadOnlyMemory<byte>? Child(ReadOnlyMemory<byte> body, params string[] path)
    {
        var at = body;
        foreach (var type in path)
        {
            var found = Children(at).FirstOrDefault(c => c.Type == type);
            if (found.Type is null)
                return null;
            at = found.Body;
        }
        return at;
    }

    // Nero's chpl in moov/udta: a version, flags, (with version 1) four
    // reserved bytes, a count, then each start in 100 ns and a title.
    private static List<ProbedChapter> FromChpl(byte[] moov)
    {
        if (Child(moov, "udta", "chpl") is not { } chpl)
            return [];
        var span = chpl.Span;
        var at = span[0] == 0 ? 4 : 8;
        int count = span[at++];
        var marks = new List<ProbedChapter>(count);
        for (var i = 0; i < count; i++)
        {
            var start = BinaryPrimitives.ReadUInt64BigEndian(span[at..]);
            int length = span[at + 8];
            var title = Encoding.UTF8.GetString(span.Slice(at + 9, length));
            marks.Add(new ProbedChapter(title, (long)(start / 10_000)));
            at += 9 + length;
        }
        return marks;
    }

    // The text track an audio track names in its tref/chap: each sample is a
    // title (a 16-bit length, then UTF-8 or BOM-marked UTF-16), its start
    // the samples' durations before it.
    private static List<ProbedChapter> FromChapterTrack(Stream stream, byte[] moov)
    {
        var tracks = Children(moov).Where(c => c.Type == "trak").Select(c => c.Body).ToList();
        uint? chapterId = null;
        foreach (var trak in tracks)
        {
            if (Child(trak, "tref", "chap") is { Length: >= 4 } chap)
            {
                chapterId = BinaryPrimitives.ReadUInt32BigEndian(chap.Span);
                break;
            }
        }
        if (chapterId is not { } id ||
            tracks.FirstOrDefault(t => TrackId(t) == id) is not { Length: > 0 } text ||
            Child(text, "mdia") is not { } mdia ||
            Child(mdia, "mdhd") is not { } mdhd ||
            Child(mdia, "minf", "stbl") is not { } stbl)
        {
            return [];
        }

        var timescale = Timescale(mdhd.Span);
        if (timescale == 0)
            return [];
        var starts = Starts(stbl, timescale);
        var offsets = SampleOffsets(stbl, starts.Count);
        if (offsets.Count == 0)
            return [];

        var marks = new List<ProbedChapter>();
        Span<byte> prefix = stackalloc byte[2];
        for (var i = 0; i < Math.Min(Math.Min(starts.Count, offsets.Count), MaxChapters + 1); i++)
        {
            var (offset, size) = offsets[i];
            var title = "";
            if (size >= 2 && offset + 2 <= stream.Length)
            {
                stream.Position = offset;
                stream.ReadExactly(prefix);
                var length = Math.Min(BinaryPrimitives.ReadUInt16BigEndian(prefix), Math.Min(size - 2, MaxTitleBytes));
                var bytes = new byte[length];
                stream.ReadExactly(bytes);
                title = bytes is [0xFE, 0xFF, ..]
                    ? Encoding.BigEndianUnicode.GetString(bytes, 2, bytes.Length - 2)
                    : bytes is [0xFF, 0xFE, ..]
                        ? Encoding.Unicode.GetString(bytes, 2, bytes.Length - 2)
                        : Encoding.UTF8.GetString(bytes);
            }
            marks.Add(new ProbedChapter(title, starts[i]));
        }
        return marks;
    }

    private static uint? TrackId(ReadOnlyMemory<byte> trak)
    {
        if (Child(trak, "tkhd") is not { } tkhd)
            return null;
        var span = tkhd.Span;
        return BinaryPrimitives.ReadUInt32BigEndian(span[(span[0] == 1 ? 20 : 12)..]);
    }

    private static uint Timescale(ReadOnlySpan<byte> mdhd) =>
        BinaryPrimitives.ReadUInt32BigEndian(mdhd[(mdhd[0] == 1 ? 20 : 12)..]);

    // Each sample's start in ms, from stts (a count and a duration per entry).
    private static List<long> Starts(ReadOnlyMemory<byte> stbl, uint timescale)
    {
        var starts = new List<long>();
        if (Child(stbl, "stts") is not { } stts)
            return starts;
        var span = stts.Span;
        var entries = BinaryPrimitives.ReadUInt32BigEndian(span[4..]);
        ulong time = 0;
        for (var e = 0; e < entries && starts.Count <= MaxChapters; e++)
        {
            var count = BinaryPrimitives.ReadUInt32BigEndian(span[(8 + (e * 8))..]);
            var delta = BinaryPrimitives.ReadUInt32BigEndian(span[(12 + (e * 8))..]);
            for (var s = 0; s < count && starts.Count <= MaxChapters; s++)
            {
                starts.Add((long)(time * 1000 / timescale));
                time += delta;
            }
        }
        return starts;
    }

    // Where each of the first `wanted` samples is in the file, and its size:
    // the chunks' offsets (stco or co64), how many samples each holds (stsc)
    // and each sample's size (stsz).
    private static List<(long Offset, int Size)> SampleOffsets(ReadOnlyMemory<byte> stbl, int wanted)
    {
        var samples = new List<(long, int)>();
        if (Child(stbl, "stsz") is not { } stsz || Child(stbl, "stsc") is not { } stsc)
            return samples;
        var fixedSize = BinaryPrimitives.ReadUInt32BigEndian(stsz.Span[4..]);
        var sampleCount = BinaryPrimitives.ReadUInt32BigEndian(stsz.Span[8..]);
        int SizeOf(int i) => (int)Math.Min(
            fixedSize != 0 ? fixedSize : BinaryPrimitives.ReadUInt32BigEndian(stsz.Span[(12 + (i * 4))..]), int.MaxValue);

        var chunks = new List<long>();
        if (Child(stbl, "stco") is { } stco)
        {
            var span = stco.Span;
            var count = BinaryPrimitives.ReadUInt32BigEndian(span[4..]);
            for (var i = 0; i < count && i <= MaxChapters; i++)
                chunks.Add(BinaryPrimitives.ReadUInt32BigEndian(span[(8 + (i * 4))..]));
        }
        else if (Child(stbl, "co64") is { } co64)
        {
            var span = co64.Span;
            var count = BinaryPrimitives.ReadUInt32BigEndian(span[4..]);
            for (var i = 0; i < count && i <= MaxChapters; i++)
                chunks.Add(checked((long)BinaryPrimitives.ReadUInt64BigEndian(span[(8 + (i * 8))..])));
        }

        var runs = stsc.Span;
        var runCount = (int)BinaryPrimitives.ReadUInt32BigEndian(runs[4..]);
        var limit = (int)Math.Min(Math.Min(sampleCount, (uint)wanted), MaxChapters + 1);
        for (var r = 0; r < runCount && samples.Count < limit; r++)
        {
            var firstChunk = (int)BinaryPrimitives.ReadUInt32BigEndian(runs[(8 + (r * 12))..]);
            var perChunk = BinaryPrimitives.ReadUInt32BigEndian(runs[(12 + (r * 12))..]);
            var lastChunk = r + 1 < runCount
                ? (int)BinaryPrimitives.ReadUInt32BigEndian(runs[(8 + ((r + 1) * 12))..]) - 1
                : chunks.Count;
            for (var c = firstChunk; c <= lastChunk && c - 1 < chunks.Count && samples.Count < limit; c++)
            {
                var offset = chunks[c - 1];
                for (var s = 0; s < perChunk && samples.Count < limit; s++)
                {
                    var size = SizeOf(samples.Count);
                    samples.Add((offset, size));
                    offset += size;
                }
            }
        }
        return samples;
    }
}
