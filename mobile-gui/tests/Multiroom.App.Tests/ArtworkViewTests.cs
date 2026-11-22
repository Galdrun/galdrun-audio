using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Headless.XUnit;
using Avalonia.Threading;
using Avalonia.VisualTree;
using Multiroom.App.Api;

namespace Multiroom.App.Tests;

public sealed class ArtworkViewTests
{
    // A page's large cover shows nothing while its picture loads: the placeholder record's green centre, drawn that
    // large, looked like a big play button for a moment. A row's small picture keeps the placeholder.
    [AvaloniaFact]
    public void A_cover_plain_while_loading_shows_no_placeholder_until_its_picture_comes()
    {
        var never = new TaskCompletionSource<HttpResponseMessage>();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((_, _) => never.Task));
        using var cache = new ArtworkCache(api);
        var plain = new ArtworkView { Reference = "album:1", Cache = cache, PlainWhileLoading = true, Width = 480, Height = 480 };
        var row = new ArtworkView { Reference = "album:1", Cache = cache };
        var window = new Window { Content = new StackPanel { Children = { plain, row } } };
        try
        {
            window.Show(); Dispatcher.UIThread.RunJobs();
            Assert.Empty(plain.GetVisualDescendants().OfType<Ellipse>());
            Assert.NotEmpty(row.GetVisualDescendants().OfType<Ellipse>());
            // Without a picture to load, the placeholder shows.
            plain.Reference = null; Dispatcher.UIThread.RunJobs();
            Assert.NotEmpty(plain.GetVisualDescendants().OfType<Ellipse>());
        }
        finally { window.Close(); never.TrySetCanceled(); }
    }
}
