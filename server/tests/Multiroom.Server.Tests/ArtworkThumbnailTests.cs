using Multiroom.Server.Abstractions;
using Multiroom.Server.Providers.Covers;
using SkiaSharp;

namespace Multiroom.Server.Tests;

public sealed class ArtworkThumbnailTests
{
    private static Artwork Picture(int width, int height, SKEncodedImageFormat format, bool transparent = false)
    {
        using var bitmap = new SKBitmap(width, height);
        bitmap.Erase(transparent ? new SKColor(200, 40, 40, 128) : new SKColor(200, 40, 40));
        using var image = SKImage.FromBitmap(bitmap);
        using var data = image.Encode(format, 90);
        return new Artwork(format == SKEncodedImageFormat.Png ? "image/png" : "image/jpeg", data.ToArray());
    }

    private static SKSizeI SizeOf(Artwork picture)
    {
        using var codec = SKCodec.Create(SKData.CreateCopy(picture.Bytes.Span))!;
        return codec.Info.Size;
    }

    [Fact]
    public void A_cover_is_scaled_to_its_longest_side_and_kept_until_it_changes()
    {
        var thumbnails = new ArtworkThumbnails();
        var cover = Picture(1200, 800, SKEncodedImageFormat.Jpeg);
        var tiny = thumbnails.Make("album:1", 96, cover);
        Assert.Equal("image/jpeg", tiny.ContentType);
        Assert.Equal(new SKSizeI(96, 64), SizeOf(tiny));
        Assert.True(thumbnails.TryGet("album:1", 96, out var kept));
        Assert.Same(tiny, kept);
        Assert.False(thumbnails.TryGet("album:1", 192, out _));
        thumbnails.Forget("album:1");
        Assert.False(thumbnails.TryGet("album:1", 96, out _));
    }

    [Fact]
    public void A_transparent_picture_stays_a_png_and_a_small_or_unreadable_one_is_answered_as_it_is()
    {
        var thumbnails = new ArtworkThumbnails();
        var logo = thumbnails.Make("ref:1", 192, Picture(600, 600, SKEncodedImageFormat.Png, transparent: true));
        Assert.Equal(("image/png", new SKSizeI(192, 192)), (logo.ContentType, SizeOf(logo)));
        var small = Picture(80, 80, SKEncodedImageFormat.Jpeg);
        Assert.Same(small, thumbnails.Make("song:1", 96, small));
        var broken = new Artwork("image/jpeg", new byte[] { 1, 2, 3 });
        Assert.Same(broken, thumbnails.Make("song:2", 96, broken));
    }
}
