using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Headless.XUnit;
using Avalonia.Input;
using Avalonia.Threading;
using Avalonia.VisualTree;
using Multiroom.App.Api;

namespace Multiroom.App.Tests;

public sealed class BrowserViewTests
{
    [AvaloniaFact]
    public async Task A_long_press_on_an_album_or_artist_row_gives_its_play_choices_and_does_not_open_it()
    {
        var artist = new BrowseNode("tidal", "artist:5", "Aurora", Kind: BrowseKind.Artist, Playable: true);
        var album = new BrowseNode("tidal", "album:1", "Days", "album:1", BrowseKind.Album, "Aurora · 2015", Playable: true);
        var similar = new BrowseNode("tidal", "artist:6", "Other", Kind: BrowseKind.Artist, Playable: true);
        var opened = new List<string>();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((r, _) =>
        {
            opened.Add(r.RequestUri!.Query);
            return Task.FromResult(BrowserTests.Reply(new BrowsePage([], BrowserTests.Empty, Node: artist,
                Sections: [new("Albums", [album], BrowserTests.Empty), new("Similar artists", [similar], BrowserTests.Empty)])));
        }));
        var browser = BrowserTests.Browser(api); await browser.OpenNodeAsync(new NodeReference("tidal", "artist:5"), "Aurora");
        var sheets = new List<HomeItem>(); browser.SheetRequested += sheets.Add;
        var view = new BrowserView { DataContext = browser };
        var window = new Window { Width = 390, Height = 1600, Content = view };
        try
        {
            window.Show(); window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            Button Row(string name) => Assert.Single(view.GetVisualDescendants().OfType<Button>(),
                b => b.IsEffectivelyVisible && Avalonia.Automation.AutomationProperties.GetName(b) == name);
            void Click(Visual target, MouseButton button)
            {
                var at = target.TranslatePoint(new Point(target.Bounds.Width / 2, target.Bounds.Height / 2), window)!.Value;
                window.MouseDown(at, button); window.MouseUp(at, button); Dispatcher.UIThread.RunJobs();
            }
            var requests = opened.Count;
            Click(Row("Days"), MouseButton.Right);
            Click(Row("Other"), MouseButton.Right);
            Assert.Equal([("Album", "Days", new NodeReference("tidal", "album:1")), ("Artist", "Other", new NodeReference("tidal", "artist:6"))],
                sheets.Select(s => (s.Kind, s.Title, s.Items.Node!)));
            Assert.Equal(requests, opened.Count);
            // A tap still opens it.
            Click(Row("Days"), MouseButton.Left);
            Assert.Contains("node=album%3A1", opened[^1]);
        }
        finally { window.Close(); }
    }
}
