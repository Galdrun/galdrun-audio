using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.VisualTree;

namespace Multiroom.App;

public partial class QueueView : UserControl
{
    private const double DragThreshold = 6, SwipeThreshold = 48;
    private ListBoxItem? _pressItem;
    private ListBoxItem? _dragItem;
    private ListBoxItem? _dropTarget;
    private bool _dropAfter;
    private IPointer? _pointer;
    private Point _pressPoint;
    private bool _dragging;
    private int _swipeApplied;

    public QueueView()
    {
        InitializeComponent();
        QueueList.AddHandler(PointerPressedEvent, Pressed, RoutingStrategies.Tunnel);
        QueueList.AddHandler(PointerMovedEvent, Moved, RoutingStrategies.Tunnel);
        QueueList.AddHandler(PointerReleasedEvent, Released, RoutingStrategies.Tunnel);
        QueueList.PointerCaptureLost += (_, _) => ResetDrag();
        DetachedFromVisualTree += (_, _) => ResetDrag();
    }

    private static ListBoxItem? ContainerOf(object? source) =>
        source is Visual visual ? visual.GetSelfAndVisualAncestors().OfType<ListBoxItem>().FirstOrDefault() : null;
    private static bool OnGrip(object? source) =>
        source is Visual visual && visual.GetSelfAndVisualAncestors().OfType<Border>().Any(border => border.Classes.Contains("grip"));
    private static bool OnDeleteButton(object? source) =>
        source is Visual visual && visual.GetSelfAndVisualAncestors().OfType<Button>().Any(button => button.Classes.Contains("rowdelete"));

    private void Pressed(object? sender, PointerPressedEventArgs e)
    {
        _pressItem = OnDeleteButton(e.Source) ? null : ContainerOf(e.Source);
        _pointer = e.Pointer;
        _pressPoint = e.GetPosition(QueueList);
        _swipeApplied = 0;
        _dragItem = OnGrip(e.Source) ? _pressItem : null;
        if (_dragItem is not null) e.Handled = true; // Grabbing the grip starts a drag, never a selection.
    }

    private void Moved(object? sender, PointerEventArgs e)
    {
        if (_pointer != e.Pointer || _pressItem is null && _dragItem is null) return;
        var delta = e.GetPosition(QueueList) - _pressPoint;
        if (!_dragging)
        {
            if (_dragItem is null)
            {
                // Swiping right reveals the delete button (it slides in from the left);
                // swiping a revealed row back to the left hides it again.
                if (Math.Abs(delta.X) > Math.Abs(delta.Y) * 1.5)
                {
                    if (delta.X > SwipeThreshold && _swipeApplied <= 0)
                    { _swipeApplied = 1; Reveal(_pressItem); }
                    else if (delta.X < -SwipeThreshold && _swipeApplied >= 0)
                    { _swipeApplied = -1; Hide(_pressItem); }
                }
                return;
            }
            if (Math.Abs(delta.X) < DragThreshold && Math.Abs(delta.Y) < DragThreshold) return;
            _dragging = true;
            e.Pointer.Capture(QueueList);
            _dragItem.Classes.Add("dragging");
        }
        UpdateDropTarget(e);
    }

    private void Released(object? sender, PointerReleasedEventArgs e)
    {
        if (_dragging && _dragItem is { } item && item.Content is QueueRow row && DataContext is PlaybackViewModel model)
        {
            var index = TargetIndex();
            ResetDrag();
            e.Handled = true; // Reordering must not change the selection.
            if (index >= 0 && index != model.Items.IndexOf(row)) model.MoveToCommand.Execute(new QueueRowMove(row, index));
        }
        else ResetDrag();
        _pressItem = null;
        _pointer = null;
        _swipeApplied = 0;
    }

    private void UpdateDropTarget(PointerEventArgs e)
    {
        if (_dragItem is null) { HideDropLine(); return; }
        var y = e.GetPosition(QueueList).Y;
        ListBoxItem? hit = null;
        var lowest = 0.0;
        foreach (var row in QueueList.GetVisualDescendants().OfType<ListBoxItem>().Where(row => row.IsEffectivelyVisible))
        {
            var top = row.TranslatePoint(new Point(0, 0), QueueList)!.Value.Y;
            var bottom = top + row.Bounds.Height;
            if (hit is null && y >= top && y < bottom) hit = row;
            if (bottom > lowest) lowest = bottom;
        }
        if (ReferenceEquals(hit, _dragItem)) { _dropTarget = null; HideDropLine(); return; } // No drop onto the dragged row itself.
        if (hit is null)
        {
            _dropTarget = null;
            if (y > lowest) ShowDropLine(lowest); // Below every row: drop at the end.
            else HideDropLine();
            return;
        }
        var top2 = hit.TranslatePoint(new Point(0, 0), QueueList)!.Value.Y;
        _dropTarget = hit;
        _dropAfter = y > top2 + hit.Bounds.Height / 2;
        ShowDropLine(_dropAfter ? top2 + hit.Bounds.Height : top2);
    }

    private int TargetIndex()
    {
        if (DataContext is not PlaybackViewModel model) return -1;
        if (_dropTarget?.Content is not QueueRow target) return model.Items.Count - 1;
        var index = model.Items.IndexOf(target);
        return Math.Clamp(_dropAfter ? index + 1 : index, 0, model.Items.Count - 1);
    }

    private void ShowDropLine(double y)
    {
        DropLine.Margin = new Thickness(0, y, 0, 0);
        DropLine.IsVisible = true;
    }
    private void HideDropLine() => DropLine.IsVisible = false;

    private void Reveal(ListBoxItem? item)
    {
        foreach (var row in QueueList.GetVisualDescendants().OfType<ListBoxItem>()) row.Classes.Remove("revealed");
        if (item is not null) item.Classes.Add("revealed");
    }
    private void Hide(ListBoxItem? item) => item?.Classes.Remove("revealed");

    private void ResetDrag()
    {
        _dragItem?.Classes.Remove("dragging");
        _dragItem = null;
        _dropTarget = null;
        _dragging = false;
        HideDropLine();
    }
}
