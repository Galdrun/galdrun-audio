using System.Collections;
using System.Collections.Specialized;
using Avalonia;
using Avalonia.Controls;
using Avalonia.VisualTree;

namespace Multiroom.App;

// A list of rows of one height inside a ScrollViewer, for long lists on a phone. It keeps as many row controls
// as the viewport shows, plus a few ahead, and never removes them: scrolling only gives a row that left the
// viewport the next item as its DataContext (item i is always row i mod count, so one row at a time). Avalonia's
// VirtualizingStackPanel takes a recycled row out of the tree and puts it back, styling it anew each time; on
// a phone that was most of a fling's work (profiled), and a scroll stuttered with every row coming in.
public sealed class RecyclingRowPanel : Panel
{
    // Whether a row lies in the viewport, at least partly; a row bound beyond its ends (Spare) does not.
    public static readonly AttachedProperty<bool> IsInViewProperty =
        AvaloniaProperty.RegisterAttached<RecyclingRowPanel, Control, bool>("IsInView");
    public static bool GetIsInView(Control row) => row.GetValue(IsInViewProperty);

    public static readonly StyledProperty<IList?> ItemsSourceProperty =
        AvaloniaProperty.Register<RecyclingRowPanel, IList?>(nameof(ItemsSource));
    public IList? ItemsSource { get => GetValue(ItemsSourceProperty); set => SetValue(ItemsSourceProperty, value); }

    // Makes a row; set before the panel is measured.
    public Func<Control>? CreateRow { get; set; }
    // A row was given another item (or none): what it showed for the last one goes (QueueView).
    public event Action<Control>? RowRebound;

    // Rows bound beyond each end of the viewport, so a row is ready before it shows.
    private const int Spare = 2;
    private readonly List<int> _indexes = []; // Each row's item index, -1 for none.
    private ScrollViewer? _scroll;
    private INotifyCollectionChanged? _observed;
    private int _first = -1;
    private double _measuredWidth = double.NaN;

    public double RowHeight { get; private set; }
    public int Count => ItemsSource?.Count ?? 0;

    // The row showing an item, while it is bound.
    public Control? RowAt(int index) =>
        Children.Count > 0 && index >= 0 && index < Count && _indexes[index % Children.Count] == index ? Children[index % Children.Count] : null;
    public int IndexOf(Control row) => Children.IndexOf(row) is var at and >= 0 ? _indexes[at] : -1;
    // The bound rows by their item index.
    public IEnumerable<(Control Row, int Index)> Rows =>
        Children.Select((row, at) => (row, _indexes[at])).Where(r => r.Item2 >= 0).OrderBy(r => r.Item2);

    protected override void OnPropertyChanged(AvaloniaPropertyChangedEventArgs change)
    {
        base.OnPropertyChanged(change);
        if (change.Property != ItemsSourceProperty) return;
        // Other rows may have another height (the "See all" grid's rows once its column count is known).
        RowHeight = 0;
        if (_observed is { } old) old.CollectionChanged -= ItemsChanged;
        _observed = ItemsSource as INotifyCollectionChanged;
        if (_observed is { } now) now.CollectionChanged += ItemsChanged;
        ItemsChanged(null, null);
    }

    private void ItemsChanged(object? sender, NotifyCollectionChangedEventArgs? e)
    {
        _first = -1; // Every row is checked again.
        Bind();
        InvalidateMeasure();
    }

    protected override void OnAttachedToVisualTree(VisualTreeAttachmentEventArgs e)
    {
        base.OnAttachedToVisualTree(e);
        _scroll = this.FindAncestorOfType<ScrollViewer>();
        if (_scroll is not null) _scroll.ScrollChanged += Scrolled;
    }

    protected override void OnDetachedFromVisualTree(VisualTreeAttachmentEventArgs e)
    {
        base.OnDetachedFromVisualTree(e);
        if (_scroll is not null) _scroll.ScrollChanged -= Scrolled;
        _scroll = null;
    }

    private void Scrolled(object? sender, ScrollChangedEventArgs e)
    {
        if (e.ViewportDelta.Y != 0) InvalidateMeasure(); // Another number of rows fits.
        else Bind();
        MarkInView();
    }

    private void MarkInView()
    {
        var top = _scroll?.Offset.Y ?? 0;
        var bottom = _scroll is { Viewport.Height: > 0 } scroll ? top + scroll.Viewport.Height : double.PositiveInfinity;
        for (var at = 0; at < Children.Count; at++)
        {
            var index = _indexes[at];
            Children[at].SetValue(IsInViewProperty, index >= 0 && index * RowHeight < bottom && (index + 1) * RowHeight > top);
        }
    }

    // Gives each row the item it shows from the first one bound; only rows whose item changed are touched.
    private void Bind()
    {
        var count = Children.Count;
        if (count == 0 || ItemsSource is not { } items) { foreach (var row in Children) Unbind(row); return; }
        var top = RowHeight > 0 && _scroll is not null ? (int)(_scroll.Offset.Y / RowHeight) - Spare : 0;
        var first = Math.Clamp(top, 0, Math.Max(0, items.Count - count));
        if (first == _first) return;
        _first = first;
        for (var index = first; index < first + count; index++)
        {
            var at = index % count;
            var row = Children[at];
            var item = index < items.Count ? items[index] : null;
            var shown = index < items.Count ? index : -1;
            if (_indexes[at] == shown && ReferenceEquals(row.DataContext, item)) continue;
            _indexes[at] = shown;
            row.DataContext = item;
            row.IsVisible = item is not null;
            RowRebound?.Invoke(row);
        }
        InvalidateArrange();
    }

    private void Unbind(Control row)
    {
        if (Children.IndexOf(row) is var at and >= 0) _indexes[at] = -1;
        row.DataContext = null; row.IsVisible = false;
    }

    protected override Size MeasureOverride(Size availableSize)
    {
        var width = double.IsFinite(availableSize.Width) ? availableSize.Width : 0;
        if (Count == 0 || CreateRow is null)
        {
            foreach (var row in Children) row.Measure(new Size(width, 0));
            return new Size(width, 0);
        }
        // A row's height can follow the width (the "See all" grid scales its covers to their columns).
        if (width != _measuredWidth) { RowHeight = 0; _measuredWidth = width; }
        if (RowHeight <= 0)
        {
            // The rows' height, from the first item.
            if (Children.Count == 0) AddRow();
            Children[0].DataContext = ItemsSource![0];
            Children[0].IsVisible = true; // A hidden control measures as nothing.
            Children[0].Measure(new Size(width, double.PositiveInfinity));
            RowHeight = Math.Max(1, Children[0].DesiredSize.Height);
            _first = -1;
        }
        var viewport = _scroll?.Viewport.Height is > 0 and var shown ? shown : _scroll?.Bounds.Height is > 0 and var bounds ? bounds : 800;
        var wanted = Math.Min(Count, (int)Math.Ceiling(viewport / RowHeight) + 1 + 2 * Spare);
        if (wanted > Children.Count)
        {
            while (Children.Count < wanted) AddRow();
            _first = -1; // The ring is another size: every row gets its item again.
        }
        Bind();
        foreach (var row in Children) row.Measure(new Size(width, RowHeight));
        return new Size(width, Count * RowHeight);
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        for (var at = 0; at < Children.Count; at++)
        {
            var index = _indexes[at];
            Children[at].Arrange(index >= 0 ? new Rect(0, index * RowHeight, finalSize.Width, RowHeight) : default);
        }
        MarkInView();
        return finalSize;
    }

    private void AddRow()
    {
        var row = CreateRow!();
        // Its own item or none, never the list's DataContext inherited until it is given one.
        row.DataContext = null;
        row.IsVisible = false;
        _indexes.Add(-1);
        Children.Add(row);
    }
}
