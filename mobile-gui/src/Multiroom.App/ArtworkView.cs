using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Multiroom.App.Api;

namespace Multiroom.App;

public sealed class ArtworkView : Border
{
    public static readonly StyledProperty<string?> ReferenceProperty = AvaloniaProperty.Register<ArtworkView, string?>(nameof(Reference));
    public static readonly StyledProperty<ArtworkCache?> CacheProperty = AvaloniaProperty.Register<ArtworkView, ArtworkCache?>(nameof(Cache));
    // The item's artworkId (server phase 40).
    public string? Reference { get => GetValue(ReferenceProperty); set => SetValue(ReferenceProperty, value); }
    public ArtworkCache? Cache { get => GetValue(CacheProperty); set => SetValue(CacheProperty, value); }
    private CancellationTokenSource? _load;
    private Bitmap? _bitmap;
    private bool _attached;
    public ArtworkView()
    {
        Width = Height = 64; CornerRadius = new CornerRadius(8); ClipToBounds = true;
        Background = Brush.Parse("#303C36"); Placeholder();
        AttachedToVisualTree += (_, _) => { _attached = true; if (Cache is { } cache) cache.Found += OnFound; Load(); };
        DetachedFromVisualTree += (_, _) => { _attached = false; if (Cache is { } cache) cache.Found -= OnFound; Cancel(); };
    }
    protected override void OnPropertyChanged(AvaloniaPropertyChangedEventArgs change)
    {
        base.OnPropertyChanged(change);
        if (change.Property == CacheProperty)
        {
            if (change.OldValue is ArtworkCache old) old.Found -= OnFound;
            if (_attached && change.NewValue is ArtworkCache cache) cache.Found += OnFound;
        }
        if (change.Property == ReferenceProperty || change.Property == CacheProperty) Load();
    }
    // The server found this view's picture (artwork.changed): it is asked again.
    private void OnFound(string id)
    {
        if (id == Reference) Avalonia.Threading.Dispatcher.UIThread.Post(() => { if (id == Reference) Load(); });
    }
    private void Placeholder()
    {
        var record = new Grid { Width = 160, Height = 160 };
        record.Children.Add(new Avalonia.Controls.Shapes.Ellipse { Fill = Brush.Parse("#202C27"), Stroke = Brush.Parse("#617568"), StrokeThickness = 2 });
        foreach (var size in new[] { 124, 100 })
            record.Children.Add(new Avalonia.Controls.Shapes.Ellipse { Width = size, Height = size, Stroke = Brush.Parse("#405449"), StrokeThickness = 1,
                HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Center, VerticalAlignment = Avalonia.Layout.VerticalAlignment.Center });
        record.Children.Add(new Avalonia.Controls.Shapes.Ellipse { Width = 64, Height = 64, Fill = Brush.Parse("#CBEC97"),
            HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Center, VerticalAlignment = Avalonia.Layout.VerticalAlignment.Center });
        record.Children.Add(new TextBlock { Text = "♪", FontSize = 42, Foreground = Brush.Parse("#202C27"),
            HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Center, VerticalAlignment = Avalonia.Layout.VerticalAlignment.Center });
        Child = new Viewbox { Margin = new Thickness(12), Child = new Border { Padding = new Thickness(24), Child = record } };
    }
    private void Cancel()
    {
        _load?.Cancel(); _load?.Dispose(); _load = null;
        Child = null; _bitmap?.Dispose(); _bitmap = null; Placeholder();
    }
    private async void Load()
    {
        Cancel();
        if (!_attached || Reference is not { } reference || Cache is not { } cache) return;
        _load = new(); var token = _load.Token;
        try
        {
            var bytes = await cache.GetAsync(reference, token);
            var decodeWidth = Math.Clamp(double.IsFinite(Width) ? (int)Width : 256, 64, 1024);
            var bitmap = await Task.Run(() => { using var stream = new MemoryStream(bytes); return Bitmap.DecodeToWidth(stream, decodeWidth); }, token);
            if (token.IsCancellationRequested) { bitmap.Dispose(); return; }
            _bitmap = bitmap; Child = new Image { Source = bitmap, Stretch = Stretch.UniformToFill };
        }
        catch (OperationCanceledException) { }
        catch (Exception e) when (e is IOException or HttpRequestException or ApiException or ArgumentException or InvalidOperationException)
        { if (!token.IsCancellationRequested) Placeholder(); }
    }
}
