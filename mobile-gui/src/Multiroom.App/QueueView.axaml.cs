using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Media;
using Avalonia.Media.Transformation;
using Avalonia.Threading;
using Avalonia.VisualTree;
using Multiroom.App.Api;

namespace Multiroom.App;

public partial class QueueView : UserControl
{
    private const double DragThreshold = 6, SwipeThreshold = 48;
    private ListBoxItem? _pressItem;
    private ListBoxItem? _dragItem;
    // While dragging: the dragged row's index, and the index it lands at when released.
    private int _dragFrom, _dragTo;
    private IPointer? _pointer;
    private Point _pressPoint;
    private bool _dragging;
    // The rows between the dragged one and _dragTo are shifted.
    private bool _shiftsShown;
    private int _swipeApplied;
    // The press moved: its release is a swipe or a drag, not a tap.
    private bool _moved;

    public QueueView()
    {
        InitializeComponent();
        QueueList.AddHandler(PointerPressedEvent, Pressed, RoutingStrategies.Tunnel);
        QueueList.AddHandler(PointerMovedEvent, Moved, RoutingStrategies.Tunnel);
        QueueList.AddHandler(PointerReleasedEvent, Released, RoutingStrategies.Tunnel);
        QueueList.PointerCaptureLost += (_, _) => ResetDrag();
        // A row the list recycles keeps no drag shift: ResetDrag only reaches the rows shown, and a shifted
        // row reused further down lay over its neighbour.
        QueueList.ContainerClearing += (_, e) => e.Container.ClearValue(RenderTransformProperty);
        QueueList.Tapped += RowTapped;
        DetachedFromVisualTree += (_, _) => ResetDrag();
        AttachedToVisualTree += (_, _) => PrefetchArtwork();
        DataContextChanged += (_, _) =>
        {
            if (_model is { } old) { old.Items.CollectionChanged -= ItemsChanged; old.PropertyChanged -= ModelChanged; }
            _model = DataContext as PlaybackViewModel;
            if (_model is { } now) { now.Items.CollectionChanged += ItemsChanged; now.PropertyChanged += ModelChanged; }
            PrefetchArtwork();
        };
    }

    // The queue's pictures are fetched ahead, from the playing track on, at the rows' size: scrolling then
    // finds them in memory. Once per batch of changes, after them.
    private const double RowArtwork = 44;
    private PlaybackViewModel? _model;
    private bool _prefetchQueued;
    private void ItemsChanged(object? sender, System.Collections.Specialized.NotifyCollectionChangedEventArgs e) => PrefetchArtwork();
    private void ModelChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    { if (e.PropertyName == nameof(PlaybackViewModel.Artwork)) PrefetchArtwork(); }
    private void PrefetchArtwork()
    {
        if (_prefetchQueued) return;
        _prefetchQueued = true;
        Dispatcher.UIThread.Post(() =>
        {
            _prefetchQueued = false;
            if (_model is not { Artwork: { } cache } model || TopLevel.GetTopLevel(this) is not { } top) return;
            var rows = model.Items;
            var current = Math.Max(0, rows.ToList().FindIndex(row => row.IsCurrent));
            cache.Prefetch(rows.Skip(current).Concat(rows.Take(current)).Select(row => row.Artwork),
                ArtworkCache.SizeFor(RowArtwork * top.RenderScaling));
        }, DispatcherPriority.Background);
    }

    private static ListBoxItem? ContainerOf(object? source) =>
        source is Visual visual ? visual.GetSelfAndVisualAncestors().OfType<ListBoxItem>().FirstOrDefault() : null;
    private static bool OnGrip(object? source) =>
        source is Visual visual && visual.GetSelfAndVisualAncestors().OfType<Border>().Any(border => border.Classes.Contains("grip"));
    private static bool OnDeleteButton(object? source) =>
        source is Visual visual && visual.GetSelfAndVisualAncestors().OfType<Button>().Any(button => button.Classes.Contains("rowdelete"));

    // The playing row in the middle of the list, or as near as the list's ends allow.
    public void ScrollToCurrent()
    {
        if (DataContext is not PlaybackViewModel model || model.Items.FirstOrDefault(r => r.IsCurrent) is not { } row) return;
        var index = model.Items.IndexOf(row);
        QueueList.ScrollIntoView(index); QueueList.UpdateLayout();
        if (QueueList.ContainerFromIndex(index) is not Control item || QueueList.Scroll is not ScrollViewer scroll ||
            item.TranslatePoint(default, scroll) is not { } at) return;
        var top = at.Y + scroll.Offset.Y;
        scroll.Offset = new Vector(scroll.Offset.X, Math.Max(0, top - (scroll.Viewport.Height - item.Bounds.Height) / 2));
    }

    // A tap plays its row; not one on the row's grip or buttons.
    private void RowTapped(object? sender, TappedEventArgs e)
    {
        if (_moved || OnGrip(e.Source) || e.Source is Visual visual && visual.GetSelfAndVisualAncestors().OfType<Button>().Any()) return;
        if (ContainerOf(e.Source)?.Content is not QueueRow row || DataContext is not PlaybackViewModel model) return;
        model.SelectedItem = row; model.StartItemCommand.Execute(null);
    }

    private void Pressed(object? sender, PointerPressedEventArgs e)
    {
        _pressItem = OnDeleteButton(e.Source) ? null : ContainerOf(e.Source);
        _pointer = e.Pointer;
        _pressPoint = e.GetPosition(QueueList);
        _swipeApplied = 0; _moved = false;
        _dragItem = OnGrip(e.Source) ? _pressItem : null;
        // Grabbing the grip starts a drag, never a selection, and the list must not scroll along with it.
        if (_dragItem is not null) { e.Handled = true; e.PreventGestureRecognition(); }
    }

    private void Moved(object? sender, PointerEventArgs e)
    {
        if (_pointer != e.Pointer || _pressItem is null && _dragItem is null) return;
        var delta = e.GetPosition(QueueList) - _pressPoint;
        if (Math.Abs(delta.X) >= DragThreshold || Math.Abs(delta.Y) >= DragThreshold) _moved = true;
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
            QueueList.Classes.Add("reordering");
            _dragFrom = _dragTo = QueueList.IndexFromContainer(_dragItem);
        }
        UpdateDropTarget(e, delta.Y);
    }

    private void Released(object? sender, PointerReleasedEventArgs e)
    {
        if (_dragging && _dragItem is { } item && item.Content is QueueRow row && DataContext is PlaybackViewModel model)
        {
            var (from, to) = (_dragFrom, _dragTo);
            ResetDrag();
            e.Handled = true; // Reordering must not change the selection.
            if (from >= 0 && to != from) model.MoveToCommand.Execute(new QueueRowMove(row, to));
        }
        else ResetDrag();
        _pressItem = null;
        _pointer = null;
        _swipeApplied = 0;
    }

    // The dragged row follows the finger; it lands before the first row whose middle is below the finger
    // (after the last whose middle is above it), and the rows between slide by its height to make room.
    private void UpdateDropTarget(PointerEventArgs e, double offset)
    {
        if (_dragItem is null || _dragFrom < 0 || QueueList.ItemsPanelRoot is not { } panel) return;
        var y = e.GetPosition(panel).Y;
        var rows = QueueList.GetRealizedContainers().OfType<ListBoxItem>()
            .Select(row => (row, index: QueueList.IndexFromContainer(row))).Where(r => r.index >= 0).ToArray();
        var to = _dragFrom;
        foreach (var (row, index) in rows)
        {
            var middle = row.Bounds.Top + row.Bounds.Height / 2;
            if (index < _dragFrom && y < middle) to = Math.Min(to, index);
            else if (index > _dragFrom && y > middle) to = Math.Max(to, index);
        }
        // The dragged row moves on every pointer move: one transform, moved. The others only change when the
        // landing index does; setting them on every move restarted their slide and made the drag stutter.
        if (_dragItem.RenderTransform is TranslateTransform follow) follow.Y = offset;
        else _dragItem.RenderTransform = new TranslateTransform(0, offset);
        if (to == _dragTo && _shiftsShown) return;
        _dragTo = to; _shiftsShown = true;
        var step = _dragItem.Bounds.Height + _dragItem.Margin.Top + _dragItem.Margin.Bottom;
        foreach (var (row, index) in rows)
        {
            if (ReferenceEquals(row, _dragItem)) continue;
            var shift = index > _dragFrom && index <= to ? -step : index < _dragFrom && index >= to ? step : 0;
            if (shift == 0) row.ClearValue(RenderTransformProperty);
            else row.RenderTransform = TransformOperations.Parse($"translateY({shift.ToString(System.Globalization.CultureInfo.InvariantCulture)}px)");
        }
    }

    private void Reveal(ListBoxItem? item)
    {
        foreach (var row in QueueList.GetVisualDescendants().OfType<ListBoxItem>()) row.Classes.Remove("revealed");
        if (item is not null) item.Classes.Add("revealed");
    }
    private void Hide(ListBoxItem? item) => item?.Classes.Remove("revealed");

    private void ResetDrag()
    {
        // Without the slide, so the rows do not animate back from where they were shown.
        QueueList.Classes.Remove("reordering");
        if (_dragging) foreach (var row in QueueList.GetRealizedContainers()) row.ClearValue(RenderTransformProperty);
        _dragItem?.Classes.Remove("dragging");
        _dragItem = null;
        _dragging = false; _shiftsShown = false;
    }
}
