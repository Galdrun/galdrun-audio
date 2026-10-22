using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Providers.Local;

namespace Multiroom.Server.Tests;

public sealed class LocalFilesProviderTests : IDisposable
{
    private readonly TempDirectory _dir = new();
    private readonly string _music;
    private readonly LibraryIndexer _indexer;
    private readonly LocalFilesProvider _provider;

    public LocalFilesProviderTests()
    {
        _music = Path.Combine(_dir.Path, "music");
        Directory.CreateDirectory(Path.Combine(_music, "album"));
        var database = new LibraryDatabase(Path.Combine(_dir.Path, "library.db"));
        var folders = LibraryTests.Registry(
            new LibrarySettings([new LibraryFolder(_music, LibraryFolderType.Local)], TimeSpan.FromMinutes(5)), database,
            _dir.Path);
        _indexer = new LibraryIndexer(folders, database, NullLogger<LibraryIndexer>.Instance);
        _provider = new LocalFilesProvider(folders, new LibraryCatalog(database, LocalFilesProvider.ProviderId),
            _indexer, Path.Combine(_dir.Path, "artwork"));
    }

    public void Dispose()
    {
        _indexer.Dispose();
        _dir.Dispose();
    }

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    [Fact]
    public async Task A_file_inside_a_folder_resolves_to_its_length()
    {
        var file = Path.Combine(_music, "album", "one.flac");
        await File.WriteAllBytesAsync(file, new byte[1234], Ct);

        var resolved = await _provider.ResolveAsync(Track(file), Ct);

        Assert.True(resolved.IsSuccess, resolved.ToString());
        Assert.Equal((StreamShape.KnownLength, 1234L, "audio/flac"),
            (resolved.Value.Shape, resolved.Value.Length, resolved.Value.ContentType));
    }

    [Fact]
    public async Task A_path_outside_every_folder_is_not_found()
    {
        var outside = _dir.Write("outside.flac", "x");

        foreach (var path in new[]
                 {
                     outside, Path.Combine(_music, "..", "outside.flac"), "outside.flac", _music,
                     _music + "-sibling" + Path.DirectorySeparatorChar + "x.flac",
                 })
        {
            var resolved = await _provider.ResolveAsync(Track(path), Ct);
            Assert.Equal(ProviderError.NotFound, resolved.Failure.Code);
        }
    }

    [Fact]
    public async Task A_missing_file_is_not_found_and_a_missing_folder_is_unavailable()
    {
        Assert.Equal(ProviderError.NotFound,
            (await _provider.ResolveAsync(Track(Path.Combine(_music, "nope.flac")), Ct)).Failure.Code);
        Assert.Equal(ProviderState.Ready, (await _provider.GetStatusAsync(Ct)).State);

        Directory.Delete(_music, recursive: true);

        Assert.Equal(ProviderState.Unavailable, (await _provider.GetStatusAsync(Ct)).State);
    }

    private static Track Track(string path) => new(LocalFilesProvider.ProviderId, path, Path.GetFileName(path));
}
