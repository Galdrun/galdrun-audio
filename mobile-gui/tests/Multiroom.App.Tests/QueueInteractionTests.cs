using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Headless.XUnit;
using Avalonia.Input;
using Avalonia.Threading;
using Avalonia.VisualTree;
using Multiroom.App.Api;

namespace Multiroom.App.Tests;

public sealed class QueueInteractionTests
{
    private static (PlaybackViewModel Model, List<string> Paths, List<string?> Bodies) Model()
    {
        var paths = new List<string>(); var bodies = new List<string?>();
        var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (r, ct) =>
        { paths.Add(r.Method + " " + r.RequestUri!.PathAndQuery); bodies.Add(r.Content is null ? null : await r.Content.ReadAsStringAsync(ct)); return FakeHttp.Json("{}"); }));
        var model = new PlaybackViewModel(action => action(api, TestContext.Current.CancellationToken));
        model.Apply(PlaybackTests.Snapshot());
        return (model, paths, bodies);
    }

    [AvaloniaFact]
    public void Opening_the_queue_puts_the_playing_track_in_the_middle()
    {
        var view = new MainView(loadPreferences: false); var model = (ShellViewModel)view.DataContext!;
        var items = Enumerable.Range(1, 40).Select(i => new QueueItem(i, new Track("local", $"track:{i}", $"T{i}"))).ToArray();
        model.Playback.Apply(new([new("living", Queue: new("living", items, RepeatMode.Off, false, QueueTransport.Paused, Current: 25))], []));
        var window = new Window { Width = 390, Height = 700, Content = view };
        try
        {
            window.Show(); window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            model.SelectedPage = "Queue"; window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var list = view.FindControl<QueueView>("QueuePage")!.FindControl<ListBox>("QueueList")!;
            var row = (Control)list.ContainerFromIndex(24)!;
            var scroll = (ScrollViewer)list.Scroll!;
            var middle = row.TranslatePoint(new Point(0, row.Bounds.Height / 2), scroll)!.Value.Y;
            Assert.Equal(scroll.Viewport.Height / 2, middle, 1.0);
        }
        finally { window.Close(); }
    }

    [AvaloniaFact]
    public void Rows_show_number_or_bars_light_the_playing_one_and_a_tap_plays_a_row()
    {
        var (model, paths, bodies) = Model();
        var view = new QueueView { DataContext = model };
        var window = new Window { Width = 500, Height = 400, Content = view };
        try
        {
            window.Show(); window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            Assert.Equal("2 tracks", view.FindControl<TextBlock>("QueueCount")!.Text);
            Assert.Equal([(true, "First"), (false, "Second")], model.Items.Select(r => (r.IsCurrent, r.Title)));
            Assert.Equal(2, model.Items[1].Number);
            var rows = view.FindControl<ListBox>("QueueList")!.GetVisualDescendants().OfType<ListBoxItem>().ToArray();
            var lit = rows.Select(r => r.GetVisualDescendants().OfType<Border>().First(b => b.Classes.Contains("queuerow")).Classes.Contains("current"));
            Assert.Equal([true, false], lit);
            var title = rows[1].GetVisualDescendants().OfType<TextBlock>().First(t => t.Text == "Second");
            var at = title.TranslatePoint(new Point(4, title.Bounds.Height / 2), window)!.Value;
            window.MouseDown(at, MouseButton.Left); window.MouseUp(at, MouseButton.Left); Dispatcher.UIThread.RunJobs();
            Assert.Equal(["POST /api/v1/zones/living/play"], paths);
            Assert.Equal("{\"item\":2}", Assert.Single(bodies));
        }
        finally { window.Close(); }
    }

    [AvaloniaFact]
    public void Dragging_the_handle_moves_the_row_and_reorders_the_queue()
    {
        var (model, paths, bodies) = Model();
        var view = new QueueView { DataContext = model };
        var window = new Window { Width = 500, Height = 400, Content = view };
        try
        {
            window.Show(); window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var list = view.FindControl<ListBox>("QueueList")!;
            var rows = list.GetVisualDescendants().OfType<ListBoxItem>().ToArray();
            Assert.Equal(2, rows.Length);
            var grip = rows[0].GetVisualDescendants().OfType<Border>().First(border => border.Classes.Contains("grip"));
            var start = grip.TranslatePoint(new Point(grip.Bounds.Width / 2, grip.Bounds.Height / 2), window)!.Value;
            var over = rows[1].TranslatePoint(new Point(60, rows[1].Bounds.Height * 0.75), window)!.Value;
            window.MouseDown(start, MouseButton.Left);
            window.MouseMove(start + new Vector(0, 20)); Dispatcher.UIThread.RunJobs();
            Assert.Contains("dragging", rows[0].Classes);
            Assert.Contains("reordering", list.Classes);
            Assert.Equal(20, rows[0].RenderTransform!.Value.M32, 1); // The dragged row follows the pointer.
            window.MouseMove(over); Dispatcher.UIThread.RunJobs();
            window.MouseUp(over, MouseButton.Left); Dispatcher.UIThread.RunJobs();
            Assert.Null(model.SelectedItem);
            Assert.DoesNotContain("dragging", rows[0].Classes);
            Assert.DoesNotContain("reordering", list.Classes);
            Assert.All(rows, row => Assert.Null(row.RenderTransform));
            Assert.Equal(["PATCH /api/v1/zones/living/queue/items/1"], paths);
            Assert.Equal("{\"index\":1}", Assert.Single(bodies, body => body is not null));
            Assert.Equal(1, model.Items[1].Item.Id); // Shown where it was dropped before the server answers.
            model.MoveToCommand.Execute(new QueueRowMove(model.Items[0], 0)); // Same position never writes.
            Dispatcher.UIThread.RunJobs(); Assert.Single(paths);
        }
        finally { window.Close(); }
    }

    [AvaloniaFact]
    public void Dropping_a_row_on_itself_leaves_the_queue_unchanged()
    {
        var (model, paths, _) = Model();
        var view = new QueueView { DataContext = model };
        var window = new Window { Width = 500, Height = 400, Content = view };
        try
        {
            window.Show(); window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var rows = view.FindControl<ListBox>("QueueList")!.GetVisualDescendants().OfType<ListBoxItem>().ToArray();
            var grip = rows[0].GetVisualDescendants().OfType<Border>().First(border => border.Classes.Contains("grip"));
            var start = grip.TranslatePoint(new Point(grip.Bounds.Width / 2, grip.Bounds.Height / 2), window)!.Value;
            window.MouseDown(start, MouseButton.Left);
            window.MouseMove(start + new Vector(0, 10)); Dispatcher.UIThread.RunJobs();
            Assert.Contains("dragging", rows[0].Classes);
            window.MouseUp(start + new Vector(0, 10), MouseButton.Left); Dispatcher.UIThread.RunJobs();
            Assert.Empty(paths);
            Assert.Equal(1, model.Items[0].Item.Id);
        }
        finally { window.Close(); }
    }

    [AvaloniaFact]
    public void Swiping_right_slides_in_the_left_delete_button_and_delete_removes_the_item()
    {
        var (model, paths, _) = Model();
        var view = new QueueView { DataContext = model };
        var window = new Window { Width = 500, Height = 400, Content = view };
        try
        {
            window.Show(); window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var rows = view.FindControl<ListBox>("QueueList")!.GetVisualDescendants().OfType<ListBoxItem>().ToArray();
            var delete = rows[0].GetVisualDescendants().OfType<Button>().First(button => button.Classes.Contains("rowdelete"));
            var grip = rows[0].GetVisualDescendants().OfType<Border>().First(border => border.Classes.Contains("grip"));
            Assert.False(delete.IsVisible); Assert.True(grip.IsVisible);
            var press = rows[0].TranslatePoint(new Point(60, rows[0].Bounds.Height / 2), window)!.Value;
            window.MouseDown(press, MouseButton.Left);
            window.MouseMove(press + new Vector(90, 0)); Dispatcher.UIThread.RunJobs();
            Assert.True(delete.IsVisible);
            Assert.Contains("revealed", rows[0].Classes);
            Assert.DoesNotContain("revealed", rows[1].Classes);
            window.MouseUp(press + new Vector(90, 0), MouseButton.Left); Dispatcher.UIThread.RunJobs();
            window.MouseDown(press, MouseButton.Left); // Swipe back left: the delete button slides out again.
            window.MouseMove(press + new Vector(-90, 0)); Dispatcher.UIThread.RunJobs();
            Assert.False(delete.IsVisible);
            Assert.DoesNotContain("revealed", rows[0].Classes);
            window.MouseUp(press + new Vector(-90, 0), MouseButton.Left); Dispatcher.UIThread.RunJobs();
            window.MouseDown(press, MouseButton.Left); // Reveal again to delete the item.
            window.MouseMove(press + new Vector(90, 0)); Dispatcher.UIThread.RunJobs();
            window.MouseUp(press + new Vector(90, 0), MouseButton.Left); Dispatcher.UIThread.RunJobs();
            Assert.True(delete.IsVisible);
            var tap = delete.TranslatePoint(new Point(delete.Bounds.Width / 2, delete.Bounds.Height / 2), window)!.Value;
            // The row content slides off the button over 0.18 s; tap once it has.
            var content = rows[0].GetVisualDescendants().OfType<Grid>().First(grid => grid.Classes.Contains("rowcontent"));
            var until = DateTime.UtcNow.AddSeconds(2);
            while (content.Margin.Left < 52 && DateTime.UtcNow < until) { AvaloniaHeadlessPlatform.ForceRenderTimerTick(); Dispatcher.UIThread.RunJobs(); }
            Assert.Equal(52, content.Margin.Left);
            window.MouseDown(tap, MouseButton.Left); window.MouseUp(tap, MouseButton.Left); Dispatcher.UIThread.RunJobs();
            Assert.Equal(["DELETE /api/v1/zones/living/queue/items/1"], paths);
        }
        finally { window.Close(); }
    }
}
