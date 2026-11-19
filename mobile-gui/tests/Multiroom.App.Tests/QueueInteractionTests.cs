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

    // The rows shown, in queue order.
    private static Control[] Rows(QueueView view) =>
        view.FindControl<RecyclingRowPanel>("QueueList")!.Rows.Where(r => r.Row.IsVisible).Select(r => r.Row).ToArray();

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
            var queue = view.FindControl<QueueView>("QueuePage")!;
            var row = queue.FindControl<RecyclingRowPanel>("QueueList")!.RowAt(24)!;
            var scroll = queue.FindControl<ScrollViewer>("QueueScroll")!;
            var middle = row.TranslatePoint(new Point(0, row.Bounds.Height / 2), scroll)!.Value.Y;
            Assert.Equal(scroll.Viewport.Height / 2, middle, 1.0);
            // Scrolled away, the queue button brings it back to the middle.
            var centred = scroll.Offset; scroll.Offset = new Vector(0, 0); window.UpdateLayout();
            var button = view.FindControl<Button>("QueueButton")!;
            var at = button.TranslatePoint(new Point(button.Bounds.Width / 2, button.Bounds.Height / 2), window)!.Value;
            using (var tap = window.TouchBegin(at)) window.TouchEnd(tap, at);
            Dispatcher.UIThread.RunJobs(); window.UpdateLayout();
            Assert.Equal("Queue", model.SelectedPage); Assert.Equal(centred, scroll.Offset);
        }
        finally { window.Close(); }
    }

    // A long queue keeps the few rows the screen shows and gives them item after item while it scrolls:
    // the same controls, never added again, each showing its own item at its own place.
    [AvaloniaFact]
    public void A_long_queue_scrolls_through_the_same_few_rows()
    {
        var (model, _, _) = Model();
        var items = Enumerable.Range(1, 200).Select(i => new QueueItem(i, new Track("local", $"track:{i}", $"T{i}"))).ToArray();
        model.Apply(new([new("living", Queue: new("living", items, RepeatMode.Off, false, QueueTransport.Paused, Current: 1))], []));
        var view = new QueueView { DataContext = model };
        var window = new Window { Width = 390, Height = 700, Content = view };
        try
        {
            window.Show(); window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var list = view.FindControl<RecyclingRowPanel>("QueueList")!;
            var scroll = view.FindControl<ScrollViewer>("QueueScroll")!;
            var made = list.Children.ToArray();
            Assert.InRange(made.Length, 2, 20);
            Assert.Equal(200 * list.RowHeight, scroll.Extent.Height, 1);
            foreach (var offset in new[] { 3000.0, 7777, 200 * list.RowHeight, 0 })
            {
                scroll.Offset = new Vector(0, offset); window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
                Assert.Equal(made, list.Children);
                var shown = list.Rows.ToArray();
                Assert.All(shown, r =>
                {
                    Assert.Equal(model.Items[r.Index], r.Row.DataContext);
                    Assert.Equal(r.Index * list.RowHeight, r.Row.Bounds.Top - r.Row.Margin.Top, 1);
                });
                var top = Math.Min(scroll.Offset.Y, scroll.Extent.Height - scroll.Viewport.Height);
                Assert.Contains(shown, r => r.Index == (int)(top / list.RowHeight));
                // Only rows in the viewport count as in view (the playing bars move only there).
                Assert.All(shown, r => Assert.Equal(
                    r.Index * list.RowHeight < scroll.Offset.Y + scroll.Viewport.Height && (r.Index + 1) * list.RowHeight > scroll.Offset.Y,
                    RecyclingRowPanel.GetIsInView(r.Row)));
                Assert.Contains(shown, r => !RecyclingRowPanel.GetIsInView(r.Row)); // The spare rows beyond the ends.
            }
        }
        finally { window.Close(); }
    }

    // A playing row's bars move only while its queue shows: an animation on a hidden queue (another page on top)
    // kept the frame timer running and the UI thread busy.
    [AvaloniaFact]
    public void The_playing_bars_move_only_while_the_queue_is_shown()
    {
        var (model, _, _) = Model();
        var view = new QueueView { DataContext = model };
        var page = new Grid { Children = { view } };
        var window = new Window { Width = 500, Height = 400, Content = page };
        try
        {
            window.Show(); window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            Assert.True(model.IsPlaying);
            var rows = Rows(view).Cast<QueueRowView>().ToArray();
            Assert.True(rows[0].AreBarsMoving);
            Assert.False(rows[1].AreBarsMoving); // Only the playing row shows its bars.
            page.IsVisible = false; Dispatcher.UIThread.RunJobs();
            Assert.False(rows[0].AreBarsMoving);
            page.IsVisible = true; Dispatcher.UIThread.RunJobs();
            Assert.True(rows[0].AreBarsMoving);
            rows[0].SetValue(RecyclingRowPanel.IsInViewProperty, false); // Scrolled out of the viewport.
            Assert.False(rows[0].AreBarsMoving);
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
            var rows = Rows(view);
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
            var list = view.FindControl<RecyclingRowPanel>("QueueList")!;
            var rows = Rows(view);
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
            var rows = Rows(view);
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
            var rows = Rows(view);
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
