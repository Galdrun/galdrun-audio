using System.Buffers.Binary;
using System.Text;

namespace Multiroom.Server.Tests;

// A minimal MP4 audio file, built box by box (no encoder is needed): an
// empty mp4a track of 0.1 s, then tagged by TagLib# with a title, a genre and,
// where asked, the media kind `stik` (2: an audiobook).
internal static class Mp4Fixture
{
    public static void Write(string path, string title, string? album = null, int number = 0, int? mediaKind = null,
        string? genre = null)
    {
        byte[] Matrix() =>
        [
            .. U32(0x00010000), .. U32(0), .. U32(0), .. U32(0), .. U32(0x00010000), .. U32(0), .. U32(0), .. U32(0),
            .. U32(0x40000000),
        ];
        var mvhd = Full("mvhd", 0, [.. U32(0), .. U32(0), .. U32(1000), .. U32(100), .. U32(0x00010000), 1, 0,
            .. new byte[10], .. Matrix(), .. new byte[24], .. U32(2)]);
        var tkhd = Full("tkhd", 7, [.. U32(0), .. U32(0), .. U32(1), .. U32(0), .. U32(100), .. new byte[8], 0, 0, 0, 0,
            1, 0, 0, 0, .. Matrix(), .. U32(0), .. U32(0)]);
        var mdhd = Full("mdhd", 0, [.. U32(0), .. U32(0), .. U32(8000), .. U32(800), 0x55, 0xC4, 0, 0]);
        var hdlr = Full("hdlr", 0, [.. U32(0), .. "soun"u8, .. new byte[12], 0]);
        var mp4a = Box("mp4a", [.. new byte[6], 0, 1, .. new byte[8], 0, 1, 0, 16, 0, 0, 0, 0, .. U32(8000u << 16)]);
        var stbl = Box("stbl", Full("stsd", 0, [.. U32(1), .. mp4a]), Full("stts", 0, U32(0)), Full("stsc", 0, U32(0)),
            Full("stsz", 0, [.. U32(0), .. U32(0)]), Full("stco", 0, U32(0)));
        var minf = Box("minf", Full("smhd", 0, [0, 0, 0, 0]),
            Box("dinf", Full("dref", 0, [.. U32(1), .. Full("url ", 1, [])])), stbl);
        var moov = Box("moov", mvhd, Box("trak", tkhd, Box("mdia", mdhd, hdlr, minf)));
        File.WriteAllBytes(path, [.. Box("ftyp", [.. "M4A "u8, .. U32(0), .. "M4A mp42isom"u8]), .. moov, .. Box("mdat")]);

        using var file = TagLib.File.Create(path);
        var apple = (TagLib.Mpeg4.AppleTag)file.GetTag(TagLib.TagTypes.Apple, create: true);
        apple.Title = title;
        if (album is not null)
            apple.Album = album;
        apple.Track = (uint)number;
        if (genre is not null)
            apple.Genres = [genre];
        if (mediaKind is { } kind)
            apple.SetData("stik", new TagLib.ByteVector((byte)kind), (uint)TagLib.Mpeg4.AppleDataBox.FlagType.ContainsData);
        file.Save();
    }

    // How a chaptered file holds its marks.
    public enum ChapterStyle
    {
        // Nero's chpl in moov/udta.
        Chpl,

        // A QuickTime text track the audio track names in tref/chap.
        Track,

        // A chpl that announces more chapters than it holds.
        BrokenChpl,
    }

    // An untagged MP4 of `durationMs` with chapter marks, its samples before
    // its moov: an empty mp4a track (id 1) and, for Track, a text track (id 2).
    public static void WriteWithChapters(string path, long durationMs, (string Title, long StartMs)[] chapters,
        ChapterStyle style)
    {
        byte[] Matrix() =>
        [
            .. U32(0x00010000), .. U32(0), .. U32(0), .. U32(0), .. U32(0x00010000), .. U32(0), .. U32(0), .. U32(0),
            .. U32(0x40000000),
        ];
        var duration = (uint)durationMs;
        byte[] Tkhd(uint id) => Full("tkhd", 7, [.. U32(0), .. U32(0), .. U32(id), .. U32(0), .. U32(duration),
            .. new byte[8], 0, 0, 0, 0, 1, 0, 0, 0, .. Matrix(), .. U32(0), .. U32(0)]);
        byte[] Mdhd(uint timescale, uint length) => Full("mdhd", 0, [.. U32(0), .. U32(0), .. U32(timescale), .. U32(length),
            0x55, 0xC4, 0, 0]);
        byte[] Hdlr(string type) => Full("hdlr", 0, [.. U32(0), .. Encoding.ASCII.GetBytes(type), .. new byte[12], 0]);
        byte[] Minf(byte[] header, byte[] stbl) => Box("minf", header,
            Box("dinf", Full("dref", 0, [.. U32(1), .. Full("url ", 1, [])])), stbl);

        var ftyp = Box("ftyp", [.. "M4B "u8, .. U32(0), .. "M4B mp42isom"u8]);
        byte[][] samples = [.. chapters.Select(c => (byte[])[.. U16((ushort)Encoding.UTF8.GetByteCount(c.Title)),
            .. Encoding.UTF8.GetBytes(c.Title)])];
        var mdat = Box("mdat", samples);
        var dataStart = (uint)(ftyp.Length + 8);

        var mvhd = Full("mvhd", 0, [.. U32(0), .. U32(0), .. U32(1000), .. U32(duration), .. U32(0x00010000), 1, 0,
            .. new byte[10], .. Matrix(), .. new byte[24], .. U32(3)]);
        var mp4a = Box("mp4a", [.. new byte[6], 0, 1, .. new byte[8], 0, 1, 0, 16, 0, 0, 0, 0, .. U32(8000u << 16)]);
        var audioStbl = Box("stbl", Full("stsd", 0, [.. U32(1), .. mp4a]), Full("stts", 0, U32(0)),
            Full("stsc", 0, U32(0)), Full("stsz", 0, [.. U32(0), .. U32(0)]), Full("stco", 0, U32(0)));
        List<byte[]> audio = [Tkhd(1)];
        if (style == ChapterStyle.Track)
            audio.Add(Box("tref", Box("chap", U32(2))));
        audio.Add(Box("mdia", Mdhd(8000, duration * 8), Hdlr("soun"), Minf(Full("smhd", 0, [0, 0, 0, 0]), audioStbl)));
        List<byte[]> moov = [mvhd, Box("trak", [.. audio])];

        if (style == ChapterStyle.Track)
        {
            var offsets = new List<uint>();
            var at = dataStart;
            foreach (var sample in samples)
            {
                offsets.Add(at);
                at += (uint)sample.Length;
            }
            var stts = chapters.Select((c, i) => (i + 1 < chapters.Length ? chapters[i + 1].StartMs : durationMs) - c.StartMs)
                .SelectMany(d => (byte[])[.. U32(1), .. U32((uint)d)]).ToArray();
            var textStbl = Box("stbl", Full("stsd", 0, U32(0)), Full("stts", 0, [.. U32((uint)chapters.Length), .. stts]),
                Full("stsc", 0, [.. U32(1), .. U32(1), .. U32(1), .. U32(1)]),
                Full("stsz", 0, [.. U32(0), .. U32((uint)samples.Length), .. samples.SelectMany(s => U32((uint)s.Length))]),
                Full("stco", 0, [.. U32((uint)offsets.Count), .. offsets.SelectMany(U32)]));
            moov.Add(Box("trak", Tkhd(2), Box("mdia", Mdhd(1000, duration), Hdlr("text"),
                Minf(Full("nmhd", 0, []), textStbl))));
        }
        else
        {
            var count = style == ChapterStyle.BrokenChpl ? chapters.Length + 2 : chapters.Length;
            byte[] marks = [.. chapters.SelectMany(c => (byte[])[.. U64((ulong)c.StartMs * 10_000),
                (byte)Encoding.UTF8.GetByteCount(c.Title), .. Encoding.UTF8.GetBytes(c.Title)])];
            // Version 1, no flags, four reserved bytes, then the count.
            moov.Add(Box("udta", Box("chpl", [1, 0, 0, 0, 0, 0, 0, 0, (byte)count, .. marks])));
        }
        File.WriteAllBytes(path, [.. ftyp, .. mdat, .. Box("moov", [.. moov])]);
    }

    private static byte[] U16(ushort value)
    {
        var bytes = new byte[2];
        BinaryPrimitives.WriteUInt16BigEndian(bytes, value);
        return bytes;
    }

    private static byte[] U64(ulong value)
    {
        var bytes = new byte[8];
        BinaryPrimitives.WriteUInt64BigEndian(bytes, value);
        return bytes;
    }

    private static byte[] U32(uint value)
    {
        var bytes = new byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(bytes, value);
        return bytes;
    }

    private static byte[] Box(string type, params byte[][] content)
    {
        byte[] body = [.. content.SelectMany(c => c)];
        return [.. U32((uint)(8 + body.Length)), .. Encoding.ASCII.GetBytes(type), .. body];
    }

    // A full box of version 0.
    private static byte[] Full(string type, uint flags, byte[] payload) =>
        Box(type, [0, (byte)(flags >> 16), (byte)(flags >> 8), (byte)flags, .. payload]);
}
