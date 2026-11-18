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
    // The next picture fades in over the one shown, which stays until it has loaded (Now Playing's cover);
    // otherwise the placeholder shows while it loads.
    public static readonly StyledProperty<bool> CrossfadeProperty = AvaloniaProperty.Register<ArtworkView, bool>(nameof(Crossfade));
    public bool Crossfade { get => GetValue(CrossfadeProperty); set => SetValue(CrossfadeProperty, value); }
    private static readonly TimeSpan FadeTime = TimeSpan.FromMilliseconds(400);
    private static readonly IBrush Empty = Brush.Parse("#303C36");
    private CancellationTokenSource? _load;
    // What is shown, newest last: during a crossfade, the pictures fading out beneath the new one.
    private readonly Grid _layers = new();
    private bool _attached;
    // A default, not a local value, so a style can square the corners (NowPlayingView's phone layout).
    static ArtworkView() => CornerRadiusProperty.OverrideDefaultValue<ArtworkView>(new CornerRadius(8));
    public ArtworkView()
    {
        Width = Height = 64; ClipToBounds = true;
        Background = Empty; Child = _layers; Show(Placeholder());
        AttachedToVisualTree += (_, _) => { _attached = true; if (Cache is { } cache) cache.Found += OnFound; Load(); };
        DetachedFromVisualTree += (_, _) => { _attached = false; if (Cache is { } cache) cache.Found -= OnFound; Cancel(); Show(Placeholder()); };
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
    // One per view, reused: a list recycling its rows shows it on every scroll, and building it each time stuttered.
    // A crossfade lays a new one over the picture shown, so it builds its own.
    private Control? _placeholder;
    private Control Placeholder() => Crossfade ? CreatePlaceholder() : _placeholder ??= CreatePlaceholder();
    private static Control CreatePlaceholder()
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
        // Opaque, so it covers a picture it fades in over.
        return new Border { Tag = nameof(Placeholder), Background = Empty, Child =new Viewbox { Margin = new Thickness(12), Child = new Border { Padding = new Thickness(24), Child = record } } };
    }
    private void Show(Control content)
    {
        if (content.Tag is nameof(Placeholder) && _layers.Children.Count > 0 && _layers.Children[^1].Tag is nameof(Placeholder)) return;
        if (!Crossfade || !_attached || _layers.Children.Count == 0)
        {
            while (_layers.Children.Count > 0) Remove(_layers.Children[0]);
            _layers.Children.Add(content);
            return;
        }
        content.Opacity = 0;
        content.Transitions = [new Avalonia.Animation.DoubleTransition { Property = OpacityProperty, Duration = FadeTime }];
        _layers.Children.Add(content);
        content.Opacity = 1;
        // Once it covers them, what it faded in over goes (unless something newer replaced it all meanwhile).
        Avalonia.Threading.DispatcherTimer.RunOnce(() => { while (_layers.Children.IndexOf(content) > 0) Remove(_layers.Children[0]); }, FadeTime);
    }
    private void Remove(Control layer)
    {
        _layers.Children.Remove(layer);
        if (layer is Image { Source: Bitmap bitmap }) bitmap.Dispose();
    }
    private void Cancel() { _load?.Cancel(); _load?.Dispose(); _load = null; }
    private async void Load()
    {
        Cancel();
        if (!Crossfade) Show(Placeholder());
        if (!_attached || Reference is not { } reference || Cache is not { } cache) { if (Crossfade) Show(Placeholder()); return; }
        _load = new(); var token = _load.Token;
        try
        {
            // Fetched at the size it is drawn (in device pixels), so a list row never decodes a 1200 px cover.
            var pixels = (double.IsFinite(Width) ? Width : 256) * (TopLevel.GetTopLevel(this)?.RenderScaling ?? 1);
            var bytes = await cache.GetAsync(reference, ArtworkCache.SizeFor(pixels), token);
            var decodeWidth = Math.Clamp((int)Math.Ceiling(pixels), 64, 1024);
            var bitmap = await Task.Run(() => { using var stream = new MemoryStream(bytes); return Bitmap.DecodeToWidth(stream, decodeWidth); }, token);
            if (token.IsCancellationRequested) { bitmap.Dispose(); return; }
            Show(new Image { Source = bitmap, Stretch = Stretch.UniformToFill });
        }
        catch (OperationCanceledException) { }
        catch (Exception e) when (e is IOException or HttpRequestException or ApiException or ArgumentException or InvalidOperationException)
        { if (!token.IsCancellationRequested) Show(Placeholder()); }
    }
}
