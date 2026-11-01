using System.Buffers;

namespace Multiroom.Server.Providers.Streaming;

// Where a track's held pieces are. A piece is written by one download at a
// time, and only while it is not held; it is read and dropped only while it
// is held. StreamWindow keeps to that, so a store needs no lock of its own.
internal interface IPieceStore : IDisposable
{
    // Downloads are copied in chunks of this size.
    const int ChunkBytes = 128 * 1024;

    // `length` bytes of `body` in as piece `index`. Null when they are all
    // there and the body ends with them; else why not, and nothing is kept.
    Task<string?> WriteAsync(int index, long length, Stream body, CancellationToken cancellationToken);

    ValueTask<int> ReadAsync(int index, long offset, Memory<byte> buffer, CancellationToken cancellationToken);

    void Drop(int index);

    static async Task<string?> EndedAsync(Stream body, Memory<byte> spare, long length,
        CancellationToken cancellationToken) =>
        await body.ReadAsync(spare[..1], cancellationToken).ConfigureAwait(false) == 0
            ? null
            : $"longer than {length} bytes";
}

// One buffer per held piece, rented from the shared pool and returned on
// drop, so steady playback allocates nothing.
internal sealed class MemoryPieceStore(int count) : IPieceStore
{
    private readonly byte[]?[] _pieces = new byte[]?[count];

    public async Task<string?> WriteAsync(int index, long length, Stream body, CancellationToken cancellationToken)
    {
        var buffer = ArrayPool<byte>.Shared.Rent((int)length);
        try
        {
            var read = 0;
            while (read < length)
            {
                var n = await body.ReadAsync(buffer.AsMemory(read, (int)Math.Min(IPieceStore.ChunkBytes, length - read)),
                    cancellationToken).ConfigureAwait(false);
                if (n == 0)
                    return $"{read} of {length} bytes";
                read += n;
            }
            if (await IPieceStore.EndedAsync(body, new byte[1], length, cancellationToken).ConfigureAwait(false) is { } longer)
                return longer;
            _pieces[index] = buffer;
            buffer = null;
            return null;
        }
        finally
        {
            if (buffer is not null)
                ArrayPool<byte>.Shared.Return(buffer);
        }
    }

    public ValueTask<int> ReadAsync(int index, long offset, Memory<byte> buffer, CancellationToken cancellationToken)
    {
        var piece = _pieces[index] ?? throw new IOException($"piece {index} is not held");
        piece.AsSpan((int)offset, buffer.Length).CopyTo(buffer.Span);
        return ValueTask.FromResult(buffer.Length);
    }

    public void Drop(int index)
    {
        if (Interlocked.Exchange(ref _pieces[index], null) is { } piece)
            ArrayPool<byte>.Shared.Return(piece);
    }

    public void Dispose()
    {
        for (var i = 0; i < _pieces.Length; i++)
            Drop(i);
    }
}

// One file per held piece, <directory>/<index>, deleted on drop: space is
// freed piece by piece. The directory goes with the track; what a crash left
// is deleted at the next start (StreamWindow).
internal sealed class DiskPieceStore : IPieceStore
{
    private readonly string _directory;

    public DiskPieceStore(string directory)
    {
        _directory = directory;
        Directory.CreateDirectory(directory);
    }

    public async Task<string?> WriteAsync(int index, long length, Stream body, CancellationToken cancellationToken)
    {
        var path = PathOf(index);
        var chunk = ArrayPool<byte>.Shared.Rent(IPieceStore.ChunkBytes);
        string? reason = null;
        try
        {
            using (var handle = File.OpenHandle(path, FileMode.Create, FileAccess.Write, FileShare.Read | FileShare.Delete,
                FileOptions.Asynchronous))
            {
                long written = 0;
                while (reason is null && written < length)
                {
                    var n = await body.ReadAsync(chunk.AsMemory(0, (int)Math.Min(IPieceStore.ChunkBytes, length - written)),
                        cancellationToken).ConfigureAwait(false);
                    if (n == 0)
                    {
                        reason = $"{written} of {length} bytes";
                        break;
                    }
                    await RandomAccess.WriteAsync(handle, chunk.AsMemory(0, n), written, cancellationToken).ConfigureAwait(false);
                    written += n;
                }
                reason ??= await IPieceStore.EndedAsync(body, chunk, length, cancellationToken).ConfigureAwait(false);
            }
        }
        catch
        {
            Delete(path);
            throw;
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(chunk);
        }
        if (reason is not null)
            Delete(path);
        return reason;
    }

    public async ValueTask<int> ReadAsync(int index, long offset, Memory<byte> buffer, CancellationToken cancellationToken)
    {
        using var handle = File.OpenHandle(PathOf(index), FileMode.Open, FileAccess.Read,
            FileShare.ReadWrite | FileShare.Delete, FileOptions.Asynchronous);
        return await RandomAccess.ReadAsync(handle, buffer, offset, cancellationToken).ConfigureAwait(false);
    }

    public void Drop(int index) => Delete(PathOf(index));

    public void Dispose()
    {
        try
        {
            Directory.Delete(_directory, recursive: true);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            // Deleted at the next start.
        }
    }

    private string PathOf(int index) => Path.Combine(_directory, index.ToString(System.Globalization.CultureInfo.InvariantCulture));

    private static void Delete(string path)
    {
        try
        {
            File.Delete(path);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            // Deleted with the track's directory.
        }
    }
}
