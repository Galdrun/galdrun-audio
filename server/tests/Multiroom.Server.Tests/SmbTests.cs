using System.Collections.Concurrent;
using System.Diagnostics;
using System.Net;
using System.Net.Http.Headers;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Library.Smb;
using Multiroom.Server.Providers.Local;
using Multiroom.Server.Web.Media;

namespace Multiroom.Server.Tests;

// smb library folders (phase 13) against a NAS in the test process
// (FakeNas: SMBLibrary's server, a user and a password, a free port): the
// indexer over the share, the local provider resolving from it, and the media
// endpoint serving what it resolved, read with FFmpeg's headers.
public sealed class SmbTests : IAsyncDisposable
{
    private readonly TempDirectory _dir = new();
    private readonly FakeNas _nas;
    private readonly LibraryDatabase _database;
    private readonly ConcurrentQueue<string> _logLines = new();
    private readonly ILoggerFactory _loggers;
    private readonly SmbSessions _sessions;
    private readonly List<LibraryIndexer> _indexers = [];
    private readonly MediaHandles _handles = new();
    private readonly List<MediaEndpoint> _endpoints = [];
    private readonly HttpClient _http = new(new SocketsHttpHandler
    {
        AllowAutoRedirect = false,
        AutomaticDecompression = DecompressionMethods.None,
        UseProxy = false,
    });

    public SmbTests()
    {
        _nas = new FakeNas(Directory.CreateDirectory(Path.Combine(_dir.Path, "nas")).FullName);
        _database = new LibraryDatabase(Path.Combine(_dir.Path, "library.db"));
        _loggers = LoggerFactory.Create(b => b.SetMinimumLevel(LogLevel.Trace)
            .AddProvider(new MediaEndpointTests.CapturingProvider(_logLines)));
        _sessions = new SmbSessions(_loggers.CreateLogger<SmbSessions>());
    }

    public async ValueTask DisposeAsync()
    {
        foreach (var indexer in _indexers)
        {
            await indexer.StopAsync(CancellationToken.None);
            indexer.Dispose();
        }
        foreach (var endpoint in _endpoints)
        {
            await endpoint.StopAsync(CancellationToken.None);
            await endpoint.DisposeAsync();
        }
        _http.Dispose();
        _sessions.Dispose();
        _nas.Dispose();
        _loggers.Dispose();
        _dir.Dispose();
    }

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    private LibraryCatalog Catalog => new(_database, LocalFilesProvider.ProviderId);

    private string Uri(string relative) => $"smb://{FakeNas.Host}:{_nas.Port}/{FakeNas.ShareName}/{relative}";

    [Fact]
    public async Task A_folder_on_the_nas_is_indexed_with_its_login_searched_and_browsed()
    {
        WriteTrack("Aurora/Days/one.wav", "Morning Light", "Aurora", "Days");
        WriteTrack("Aurora/Days/two.wav", "Evening Rain", "Aurora", "Days");
        await File.WriteAllTextAsync(Path.Combine(_nas.Root, "notes.txt"), "not audio", Ct);

        var (indexer, _) = await Start(TimeSpan.FromMinutes(5), Share());
        await Until(() => indexer.Status.Passes == 1, "the first pass");

        Assert.Equal((2L, 2L, 0L), (indexer.Status.Scanned, indexer.Status.Indexed, indexer.Status.Failed));
        var folder = Assert.Single(indexer.Folders);
        Assert.Equal((LibraryFolderState.Ready, Uri("")[..^1], true), (folder.State, folder.Path, folder.HasPassword));
        var morning = Assert.Single((await Catalog.SearchAsync("morning", SearchKinds.All, 0, 10, Ct)).Value.Tracks);
        Assert.Equal(Uri("Aurora/Days/one.wav"), Catalog.PathOf(morning.ProviderItemId).Value);

        var folders = (await Catalog.BrowseAsync(new("folders", 0, 10), Ct)).Value.Nodes;
        Assert.Equal([Uri("")[..^1]], folders.Select(n => n.Title));
        var top = (await Catalog.BrowseAsync(new(folders[0].NodeId, 0, 10), Ct)).Value;
        Assert.Equal(["Aurora"], top.Nodes.Select(n => n.Title));
        var aurora = (await Catalog.BrowseAsync(new(top.Nodes[0].NodeId, 0, 10), Ct)).Value;
        var days = (await Catalog.BrowseAsync(new(aurora.Nodes[0].NodeId, 0, 10), Ct)).Value;
        Assert.Equal(["Evening Rain", "Morning Light"], days.Items.Tracks.Select(t => t.Title).Order());
    }

    [Fact]
    public async Task A_file_moved_on_the_share_keeps_its_track_id_and_is_not_probed_again()
    {
        WriteTrack("inbox/song.wav", "Wanderer");
        var (indexer, _) = await Start(TimeSpan.FromMilliseconds(200), Share());
        await Until(() => indexer.Status.Passes >= 1, "the first pass");
        var id = Assert.Single((await Catalog.SearchAsync("wanderer", SearchKinds.All, 0, 10, Ct)).Value.Tracks).ProviderItemId;

        Directory.CreateDirectory(Path.Combine(_nas.Root, "sorted"));
        File.Move(Path.Combine(_nas.Root, "inbox", "song.wav"), Path.Combine(_nas.Root, "sorted", "song.wav"));
        await Until(() => Catalog.PathOf(id) is { IsSuccess: true } found && found.Value == Uri("sorted/song.wav"),
            "the moved file under its old id");

        Assert.Equal(id, Assert.Single((await Catalog.SearchAsync("wanderer", SearchKinds.All, 0, 10, Ct)).Value.Tracks).ProviderItemId);
        Assert.Equal(1, indexer.Status.Indexed);
    }

    [Fact]
    public async Task A_wrong_password_is_authFailed_by_folder_and_the_password_is_in_no_log_line()
    {
        const string wrong = "wr0ng-pa55word-never-logged";
        WriteTrack("a.wav", "Alpha");
        var (indexer, registry) = await Start(TimeSpan.FromMinutes(5), Share(password: wrong));
        await Until(() => indexer.Folders[0].State == LibraryFolderState.AuthFailed, "the refused login");

        Assert.Empty((await Catalog.SearchAsync("alpha", SearchKinds.All, 0, 10, Ct)).Value.Tracks);
        Assert.Contains(_logLines, l => l.Contains("login was refused", StringComparison.Ordinal) &&
            l.Contains(Uri("")[..^1], StringComparison.Ordinal));
        Assert.DoesNotContain(wrong, indexer.Folders[0].Detail ?? "", StringComparison.Ordinal);
        Assert.Equal(LibraryError.InvalidArgument, registry.Update("config:0", null, null, FakeNas.Password, null).Failure.Code);

        Assert.All(_logLines, l =>
        {
            Assert.DoesNotContain(wrong, l, StringComparison.Ordinal);
            Assert.DoesNotContain(FakeNas.Password, l, StringComparison.Ordinal);
            Assert.DoesNotContain(FakeNas.User, l, StringComparison.Ordinal);
        });
    }

    [Fact]
    public async Task A_share_that_drops_keeps_its_rows_and_is_retried_on_the_poll()
    {
        WriteTrack("a.wav", "Alpha");
        var (indexer, _) = await Start(TimeSpan.FromMilliseconds(200), Share());
        await Until(() => indexer.Status.Passes >= 1, "the first pass");

        _nas.Stop();
        await Until(() => indexer.Folders[0].State == LibraryFolderState.Unreachable, "the share to drop");
        Assert.Single((await Catalog.SearchAsync("alpha", SearchKinds.All, 0, 10, Ct)).Value.Tracks);

        WriteTrack("b.wav", "Bravo");
        _nas.Start();
        await Until(async () => (await Catalog.SearchAsync("bravo", SearchKinds.All, 0, 10, Ct)).Value.Tracks.Count == 1,
            "the share, on a later poll");

        Assert.Equal(LibraryFolderState.Ready, indexer.Folders[0].State);
        Assert.Single((await Catalog.SearchAsync("alpha", SearchKinds.All, 0, 10, Ct)).Value.Tracks);
    }

    [Fact]
    public async Task A_track_on_the_share_is_served_206_at_odd_offsets_and_concurrent_GETs_agree()
    {
        var data = new byte[300_001];
        new Random(13).NextBytes(data);
        await File.WriteAllBytesAsync(Path.Combine(_nas.Root, "track.flac"), data, Ct);
        var provider = Provider(Share());

        var resolved = await provider.ResolveAsync(Track(Uri("track.flac")), Ct);
        Assert.True(resolved.IsSuccess, resolved.ToString());
        Assert.Equal((StreamShape.KnownLength, (long)data.Length, "audio/flac"),
            (resolved.Value.Shape, resolved.Value.Length, resolved.Value.ContentType));
        var uri = await Serve(resolved.Value);

        var n = data.Length;
        foreach (var offset in new long[] { 0, n - 128, n / 2, 12345, n - 1, 1 })
        {
            using var response = await Get(uri, offset);
            Assert.Equal(HttpStatusCode.PartialContent, response.StatusCode);
            Assert.Equal($"bytes {offset}-{n - 1}/{n}", response.Content.Headers.ContentRange?.ToString());
            Assert.Equal(data[(int)offset..], await response.Content.ReadAsByteArrayAsync(Ct));
        }

        var offsets = Enumerable.Range(0, 8).Select(i => (long)(i * 37_001 + 7)).ToList();
        var bodies = await Task.WhenAll(offsets.Select(async offset =>
        {
            using var response = await Get(uri, offset);
            return await response.Content.ReadAsByteArrayAsync(Ct);
        }));
        for (var i = 0; i < offsets.Count; i++)
            Assert.Equal(data[(int)offsets[i]..], bodies[i]);

        _handles.Retire(uri.Segments[^1]);
        await Until(() => _sessions.Status.All(s => s.InUse == 0), "the sessions to be let go");
    }

    [Fact]
    public async Task A_file_that_changed_after_it_was_resolved_is_404_and_one_outside_the_folder_is_not_found()
    {
        var file = Path.Combine(_nas.Root, "track.flac");
        await File.WriteAllBytesAsync(file, new byte[5000], Ct);
        var provider = Provider(Share());
        var uri = await Serve((await provider.ResolveAsync(Track(Uri("track.flac")), Ct)).Value);

        File.SetLastWriteTimeUtc(file, DateTime.UtcNow.AddMinutes(-5));
        using (var response = await Get(uri, 0))
            Assert.Equal(HttpStatusCode.NotFound, response.StatusCode);

        foreach (var outside in new[] { Uri("../x.flac"), Uri("nope.flac"), $"smb://other:{_nas.Port}/music/track.flac" })
        {
            var refused = await provider.ResolveAsync(Track(outside), Ct);
            Assert.Equal(ProviderError.NotFound, refused.Failure.Code);
        }
    }

    [Fact]
    public async Task Resolving_wakes_a_sleeping_nas_so_the_core_finds_its_first_bytes_ready()
    {
        var data = new byte[200_000];
        new Random(3).NextBytes(data);
        await File.WriteAllBytesAsync(Path.Combine(_nas.Root, "track.flac"), data, Ct);
        var provider = Provider(Share());

        _nas.Sleep(TimeSpan.FromSeconds(1.5));
        var clock = Stopwatch.StartNew();
        var resolved = await provider.ResolveAsync(Track(Uri("track.flac")), Ct);
        Assert.True(resolved.IsSuccess, resolved.ToString());
        Assert.True(clock.Elapsed >= TimeSpan.FromSeconds(1.4), $"resolved in {clock.Elapsed}, before the disk woke");
        Assert.True(_nas.Reads >= 2, "the head and the tail were read");

        var uri = await Serve(resolved.Value);
        clock.Restart();
        using var response = await Get(uri, 0);
        Assert.Equal(data, await response.Content.ReadAsByteArrayAsync(Ct));
        Assert.True(clock.Elapsed < TimeSpan.FromSeconds(1), $"the first GET took {clock.Elapsed}");
    }

    [Fact]
    public void Sessions_are_bounded_per_server_shared_beyond_it_and_answer_their_use()
    {
        var login = new SmbLogin(FakeNas.Host, _nas.Port, FakeNas.User, FakeNas.Password);
        var leases = Enumerable.Range(0, SmbSessions.SessionsPerServer + 2)
            .Select(_ => _sessions.Lease(login).Value).ToList();

        var status = Assert.Single(_sessions.Status);
        Assert.Equal(($"{FakeNas.Host}:{_nas.Port}", SmbSessions.SessionsPerServer, SmbSessions.SessionsPerServer),
            (status.Server, status.Sessions, status.InUse));
        Assert.Equal(["music", "photos"], leases[^1].ListShares().Value.Where(s => !s.EndsWith('$')));
        Assert.Equal(LibraryError.NotFound, leases[0].List("nothing", "").Failure.Code);

        foreach (var lease in leases)
            lease.Dispose();
        Assert.Equal(0, Assert.Single(_sessions.Status).InUse);

        var refused = _sessions.Lease(new SmbLogin(FakeNas.Host, _nas.Port, FakeNas.User, "wrong"));
        Assert.Equal(LibraryError.AuthFailed, refused.Failure.Code);
        Assert.DoesNotContain("wrong", refused.Failure.Detail, StringComparison.Ordinal);
    }

    private LibraryFolder Share(string? password = FakeNas.Password, string? directory = null) =>
        LibraryFolder.OnShare(new SmbLocation(new SmbLogin(FakeNas.Host, _nas.Port, FakeNas.User, password),
            FakeNas.ShareName, directory));

    private LibraryFolders Registry(TimeSpan pollInterval, params LibraryFolder[] folders) =>
        new(new LibrarySettings(folders, pollInterval), _database, Path.Combine(_dir.Path, "credentials.json"), _sessions);

    private async Task<(LibraryIndexer, LibraryFolders)> Start(TimeSpan pollInterval, params LibraryFolder[] folders)
    {
        var registry = Registry(pollInterval, folders);
        var indexer = new LibraryIndexer(registry, _database, _loggers.CreateLogger<LibraryIndexer>());
        _indexers.Add(indexer);
        await indexer.StartAsync(Ct);
        return (indexer, registry);
    }

    private LocalFilesProvider Provider(params LibraryFolder[] folders)
    {
        var registry = Registry(TimeSpan.FromMinutes(5), folders);
        return new LocalFilesProvider(registry, Catalog,
            new LibraryIndexer(registry, _database, _loggers.CreateLogger<LibraryIndexer>()), Path.Combine(_dir.Path, "artwork"));
    }

    private static Track Track(string uri) => new(LocalFilesProvider.ProviderId, uri, uri);

    // Written beside the share, then moved into it whole.
    private void WriteTrack(string relative, string title, string? artist = null, string? album = null)
    {
        var staging = Path.Combine(_dir.Path, "staging.wav");
        LibraryTests.WriteTrack(staging, title, artist, album);
        var target = Path.Combine(_nas.Root, relative);
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        File.Move(staging, target, overwrite: true);
    }

    private async Task<Uri> Serve(ResolvedStream resolved)
    {
        var endpoint = new MediaEndpoint(new MediaEndpointOptions(IPAddress.Loopback, 0, null), _handles, _loggers);
        _endpoints.Add(endpoint);
        await endpoint.StartAsync(Ct);
        return new Uri(endpoint.BaseUri!, _handles.Register(resolved));
    }

    private async Task<HttpResponseMessage> Get(Uri uri, long offset)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, uri);
        request.Headers.UserAgent.ParseAdd("Lavf/62.3.100");
        request.Headers.Accept.ParseAdd("*/*");
        request.Headers.ConnectionClose = true;
        request.Headers.Add("Icy-MetaData", "1");
        request.Headers.Range = new RangeHeaderValue(offset, null);
        return await _http.SendAsync(request, Ct);
    }

    private static Task Until(Func<bool> condition, string what) => Until(() => Task.FromResult(condition()), what);

    private static async Task Until(Func<Task<bool>> condition, string what)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(20);
        while (!await condition())
        {
            if (DateTime.UtcNow > deadline)
                Assert.Fail($"timed out waiting for {what}");
            await Task.Delay(20, Ct);
        }
    }
}
