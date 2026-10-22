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
    public void Dragging_the_handle_reorders_the_queue_with_a_visible_drop_line()
    {
        var (model, paths, bodies) = Model();
        var view = new QueueView { DataContext = model };
        var window = new Window { Width = 500, Height = 400, Content = view };
        try
        {
            window.Show(); window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var list = view.FindControl<ListBox>("QueueList")!;
            var line = view.FindControl<Border>("DropLine")!;
            var rows = list.GetVisualDescendants().OfType<ListBoxItem>().ToArray();
            Assert.Equal(2, rows.Length);
            var grip = rows[0].GetVisualDescendants().OfType<Border>().First(border => border.Classes.Contains("grip"));
            var start = grip.TranslatePoint(new Point(grip.Bounds.Width / 2, grip.Bounds.Height / 2), window)!.Value;
            var over = rows[1].TranslatePoint(new Point(60, rows[1].Bounds.Height * 0.75), window)!.Value;
            window.MouseDown(start, MouseButton.Left);
            window.MouseMove(start + new Vector(0, 20)); Dispatcher.UIThread.RunJobs();
            Assert.Contains("dragging", rows[0].Classes);
            window.MouseMove(over); Dispatcher.UIThread.RunJobs();
            var row1Top = rows[1].TranslatePoint(new Point(0, 0), list)!.Value.Y;
            Assert.True(line.IsVisible); // Dropping over the lower half points at the end of row 1.
            Assert.Equal(row1Top + rows[1].Bounds.Height, line.Margin.Top, 1);
            window.MouseUp(over, MouseButton.Left); Dispatcher.UIThread.RunJobs();
            Assert.False(line.IsVisible);
            Assert.Null(model.SelectedItem);
            Assert.DoesNotContain("dragging", rows[0].Classes);
            Assert.Equal(["PATCH /api/v1/zones/living/queue/items/1"], paths);
            Assert.Equal("{\"index\":1}", Assert.Single(bodies, body => body is not null));
            model.MoveToCommand.Execute(new QueueRowMove(model.Items[0], 0)); // Same position never writes.
            Dispatcher.UIThread.RunJobs(); Assert.Single(paths);
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
            window.MouseDown(tap, MouseButton.Left); window.MouseUp(tap, MouseButton.Left); Dispatcher.UIThread.RunJobs();
            Assert.Equal(["DELETE /api/v1/zones/living/queue/items/1"], paths);
        }
        finally { window.Close(); }
    }
}
