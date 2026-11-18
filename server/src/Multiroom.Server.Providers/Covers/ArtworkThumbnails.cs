using Multiroom.Server.Abstractions;
using SkiaSharp;

namespace Multiroom.Server.Providers.Covers;

// A picture at a named size (GET artwork/{id}?size=): its longest side at most
// that many pixels, so a list row does not fetch and decode a 1200 px cover.
// Made on demand and kept in memory, bounded, least recently used out first;
// what ArtworkService.Changed names goes. The original answers a size it is not
// larger than, and a picture that cannot be decoded.
public sealed class ArtworkThumbnails
{
    public static readonly IReadOnlyDictionary<string, int> Sizes = new Dictionary<string, int>(StringComparer.Ordinal)
    {
        ["tiny"] = 96,
        ["small"] = 192,
        ["medium"] = 384,
        ["large"] = 768,
    };

    public const long Capacity = 32 * 1024 * 1024;
    private const int JpegQuality = 85;

    private readonly Lock _lock = new();
    private readonly Dictionary<(string Id, int Size), LinkedListNode<(string Id, int Size, Artwork Picture)>> _kept = [];
    private readonly LinkedList<(string Id, int Size, Artwork Picture)> _order = []; // Least recently used first.
    private long _bytes;

    public bool TryGet(string id, int size, out Artwork picture)
    {
        lock (_lock)
        {
            if (_kept.TryGetValue((id, size), out var node))
            {
                _order.Remove(node);
                _order.AddLast(node);
                picture = node.Value.Picture;
                return true;
            }
        }
        picture = null!;
        return false;
    }

    public Artwork Make(string id, int size, Artwork original)
    {
        var picture = Scale(original, size) ?? original;
        lock (_lock)
        {
            Remove((id, size));
            if (picture.Bytes.Length > Capacity)
                return picture;
            _kept[(id, size)] = _order.AddLast((id, size, picture));
            _bytes += picture.Bytes.Length;
            while (_bytes > Capacity && _order.First is { } oldest)
                Remove((oldest.Value.Id, oldest.Value.Size));
        }
        return picture;
    }

    // Its picture changed: every size made of the old one goes.
    public void Forget(string id)
    {
        lock (_lock)
        {
            foreach (var size in Sizes.Values)
                Remove((id, size));
        }
    }

    private void Remove((string Id, int Size) key)
    {
        if (_kept.Remove(key, out var node))
        {
            _order.Remove(node);
            _bytes -= node.Value.Picture.Bytes.Length;
        }
    }

    // A JPEG, or a PNG when the picture has transparency; null when it cannot
    // be decoded or is no larger than the size.
    internal static Artwork? Scale(Artwork original, int size)
    {
        using var encoded = SKData.CreateCopy(original.Bytes.Span);
        using var codec = SKCodec.Create(encoded);
        if (codec is null)
            return null;
        var (width, height) = (codec.Info.Width, codec.Info.Height);
        if (width <= 0 || height <= 0 || Math.Max(width, height) <= size)
            return null;
        var scale = (double)size / Math.Max(width, height);
        var target = new SKImageInfo(Math.Max(1, (int)Math.Round(width * scale)), Math.Max(1, (int)Math.Round(height * scale)));
        // Decoded at the smallest size the codec can give that is not below the target (a JPEG at 1/2,
        // 1/4 or 1/8 of its size costs that much less), then scaled down the rest of the way.
        var decodeSize = codec.GetScaledDimensions((float)Math.Max(scale, 1.0 / 8));
        if (decodeSize.Width < target.Width || decodeSize.Height < target.Height)
            decodeSize = new SKSizeI(width, height);
        using var decoded = new SKBitmap(new SKImageInfo(decodeSize.Width, decodeSize.Height, SKColorType.Rgba8888, SKAlphaType.Premul));
        var result = codec.GetPixels(decoded.Info, decoded.GetPixels());
        if (result is not (SKCodecResult.Success or SKCodecResult.IncompleteInput))
            return null;
        using var scaled = decoded.Resize(target with { ColorType = SKColorType.Rgba8888, AlphaType = SKAlphaType.Premul },
            new SKSamplingOptions(SKCubicResampler.Mitchell));
        if (scaled is null)
            return null;
        var opaque = codec.Info.AlphaType == SKAlphaType.Opaque;
        using var image = SKImage.FromBitmap(scaled);
        using var data = image.Encode(opaque ? SKEncodedImageFormat.Jpeg : SKEncodedImageFormat.Png, JpegQuality);
        return data is null ? null : new Artwork(opaque ? "image/jpeg" : "image/png", data.ToArray());
    }
}
