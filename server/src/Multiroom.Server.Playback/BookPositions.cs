using System.Threading.Channels;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;

namespace Multiroom.Server.Playback;

// Keeps where a listener stopped in every book (phase 33), always, whether
// or not the house resumes books: when a zone's book is paused, stopped,
// removed, replaced or left for another item, every Checkpoint while it
// plays, and when this server stops. A book left within FinishedWithin of
// its end was played to it: it is marked finished and its place cleared.
//
// QueueChanged is raised under the zone's gate, so the handler only notes
// the place and queues the write; BookPositionStore is written on this
// service's own task.
public sealed partial class BookPositions : BackgroundService
{
    public static readonly TimeSpan Checkpoint = TimeSpan.FromSeconds(15);
    public static readonly TimeSpan FinishedWithin = TimeSpan.FromSeconds(30);

    // Far more than a house writes between two writes; a burst beyond it
    // drops its oldest writes rather than wait.
    private const int Capacity = 256;

    private readonly QueueEngine _queues;
    private readonly BookPositionStore _store;
    private readonly ILogger _log;
    private readonly TimeProvider _time;
    private readonly Lock _lock = new();
    // Each zone's current book, as last heard.
    private readonly Dictionary<string, Heard> _books = new(StringComparer.Ordinal);
    private readonly Channel<BookPosition> _writes =
        Channel.CreateBounded<BookPosition>(new BoundedChannelOptions(Capacity) { FullMode = BoundedChannelFullMode.DropOldest });

    public BookPositions(QueueEngine queues, BookPositionStore store, ILogger<BookPositions> log,
        TimeProvider? time = null)
    {
        _queues = queues;
        _store = store;
        _log = log;
        _time = time ?? TimeProvider.System;
        queues.QueueChanged += (_, state) => Note(state);
        queues.QueueDeleted += (_, zone) => Leave(zone);
    }

    // A book current in a zone: its item, and its part and the offset in it
    // at the timestamp At, running on from there while Running.
    private sealed record Heard(QueueItemId Item, QueueGroup Book, int Part, long OffsetMs, long At, bool Running,
        QueueTransport Transport);

    // A book's position follows its reference: the node it was queued from,
    // else its first part's track (a book that is one file).
    public static string ReferenceOf(QueueGroup book) => book.Source ?? ReferenceOf(book.Parts[0].Track);

    public static string ReferenceOf(Track track) => $"{track.ProviderId}:{track.ProviderItemId}";

    private void Note(QueueState state)
    {
        lock (_lock)
        {
            var heard = _books.GetValueOrDefault(state.Zone);
            var item = state.Current is { } current ? state.Items.FirstOrDefault(i => i.Id == current) : null;
            if (heard is not null && item?.Id != heard.Item)
            {
                _writes.Writer.TryWrite(Place(heard, leaving: true));
                _books.Remove(state.Zone);
                heard = null;
            }
            if (item?.Group is not { } book)
                return;

            // A place is known once the book has been heard; until then
            // there is nothing to keep, and a saved one is not overwritten.
            var now = _time.GetTimestamp();
            Heard next;
            if (state.Part is { PositionMs: { } offset } part && state.NowPlaying?.Item == item.Id)
                next = new Heard(item.Id, book, part.Index, offset, now, state.Transport == QueueTransport.Playing, state.Transport);
            else if (heard is not null)
                next = heard with { Book = book, OffsetMs = OffsetOf(heard), At = now, Running = false, Transport = state.Transport };
            else
                return;
            _books[state.Zone] = next;

            var stopped = state.Transport is QueueTransport.Paused or QueueTransport.Stopped;
            if (stopped && heard is not null && heard.Transport != state.Transport)
                _writes.Writer.TryWrite(Place(next, leaving: false));
        }
    }

    private void Leave(string zone)
    {
        lock (_lock)
        {
            if (_books.Remove(zone, out var heard))
                _writes.Writer.TryWrite(Place(heard, leaving: true));
        }
    }

    // Every book playing, its place now.
    private void Checkpointed()
    {
        foreach (var state in _queues.Queues.Where(q => q.Transport == QueueTransport.Playing))
        {
            Note(state);
            lock (_lock)
            {
                if (_books.TryGetValue(state.Zone, out var heard))
                    _writes.Writer.TryWrite(Place(heard, leaving: false));
            }
        }
    }

    private long OffsetOf(Heard heard)
    {
        var offset = heard.Running ? heard.OffsetMs + (long)_time.GetElapsedTime(heard.At).TotalMilliseconds : heard.OffsetMs;
        return heard.Book.Parts[heard.Part].Length is { } length ? Math.Min(offset, (long)length.TotalMilliseconds) : offset;
    }

    private BookPosition Place(Heard heard, bool leaving)
    {
        var book = heard.Book;
        var reference = ReferenceOf(book);
        var duration = book.Duration is { } d ? (long)d.TotalMilliseconds : (long?)null;
        var offset = OffsetOf(heard);
        var length = book.Parts[heard.Part].Length;
        var finished = leaving && heard.Part == book.Parts.Count - 1 && length is { } l &&
            offset >= (long)(l - FinishedWithin).TotalMilliseconds;
        if (finished)
            return new BookPosition(reference, book.Title, book.Artist, 0, null, 0, null, duration, true, _time.GetUtcNow());

        long? before = 0;
        foreach (var part in book.Parts.Take(heard.Part))
            before = part.Length is { } p ? before + (long)p.TotalMilliseconds : null;
        return new BookPosition(reference, book.Title, book.Artist, heard.Part, ReferenceOf(book.Parts[heard.Part].Track),
            offset, before + offset, duration, false, _time.GetUtcNow());
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        try
        {
            Task<bool>? read = null;
            var checkpoint = Task.Delay(Checkpoint, _time, stoppingToken);
            while (true)
            {
                read ??= _writes.Reader.WaitToReadAsync(stoppingToken).AsTask();
                var done = await Task.WhenAny(read, checkpoint).ConfigureAwait(false);
                if (done == checkpoint)
                {
                    await checkpoint.ConfigureAwait(false);
                    Checkpointed();
                    checkpoint = Task.Delay(Checkpoint, _time, stoppingToken);
                    continue;
                }
                await read.ConfigureAwait(false);
                read = null;
                Drain();
            }
        }
        catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
        {
            // Stopping: StopAsync writes what is left.
        }
    }

    // At server stop: every book current, where it is.
    public override async Task StopAsync(CancellationToken cancellationToken)
    {
        await base.StopAsync(cancellationToken).ConfigureAwait(false);
        foreach (var state in _queues.Queues)
            Note(state);
        lock (_lock)
        {
            foreach (var heard in _books.Values)
                _writes.Writer.TryWrite(Place(heard, leaving: false));
        }
        Drain();
    }

    private void Drain()
    {
        while (_writes.Reader.TryRead(out var position))
        {
            var saved = _store.Save(position);
            if (!saved.IsSuccess)
                LogUnsaved(_log, position.Title, saved.Failure.Detail);
        }
    }

    [LoggerMessage(Level = LogLevel.Warning, Message = "the position in {Title} was not saved: {Reason}")]
    private static partial void LogUnsaved(ILogger log, string title, string reason);
}
