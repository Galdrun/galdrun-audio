using System.Collections.Concurrent;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading.Channels;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Ai;
using Multiroom.Server.Ai.Suggestions;
using Multiroom.Server.Library;
using Multiroom.Server.Playback;
using Multiroom.Server.Providers;

namespace Multiroom.Server.Web.Api;

// What the proposals have cost since the server started, on status.
public sealed record ProposalStats(long Runs, long Failures, DateTimeOffset? LastRunAt, DateTimeOffset? NextRunAt,
    long NotFound, long Tokens);

// AI proposals: what the house (or a user)
// might like next, from what it liked and made favourites. A run is slow and
// costs tokens, so it runs on this service's task, one at a time, and its
// last result is what every UI reads (ProposalStore). It is no add-in: the
// trigger is this server's logic, and the AI only names items (phase 26's
// line). Songs go through SongSuggestions unchanged; albums, artists and
// audiobooks through ProposalSuggester, found by SongResolver's rules, a book
// told from an album by the classifier that groups books (AudiobookLikes).
//
// A manual refresh is queued here and runs whenever an AI is configured.
// Background runs need the setting, and for each scope with marks of its own
// a last run older than intervalHours, a seed that changed since, no mark
// changed for debounceMinutes, and fewer than maxRunsPerDay runs today (every
// scope counted). The task wakes on a mark changed, a setting changed and a
// refresh, otherwise once a minute.
public sealed partial class ProposalRunner : BackgroundService, IProposals
{
    // The seed: the newest of each kind and as many more at random.
    public const int SeedTracks = 25;
    public const int SeedAlbums = 15;
    public const int SeedArtists = 15;
    public const int SeedAudiobooks = 10;
    public const int SeedPlays = 50;

    // Songs among a scope's last plays are never proposed (phase 26's rule).
    public const int AvoidPlays = 100;

    // A kind short of its count after resolving asks again, at most this often.
    public const int MaxRounds = 3;

    public static readonly TimeSpan Tick = TimeSpan.FromMinutes(1);

    private readonly ProposalStore _store;
    private readonly AiProviderRegistry _ai;
    private readonly AiInstructions _instructions;
    private readonly SongSuggestions _songs;
    private readonly ProposalSuggester _suggester;
    private readonly SongResolver _resolver;
    private readonly AudiobookLikes _books;
    private readonly LikeStore _likes;
    private readonly PlaylistStore _playlists;
    private readonly PlayHistoryStore _history;
    private readonly UserStore _users;
    private readonly ApiEvents _events;
    private readonly ILogger _log;
    private readonly TimeProvider _time;
    private readonly Channel<bool> _wake =
        Channel.CreateBounded<bool>(new BoundedChannelOptions(1) { FullMode = BoundedChannelFullMode.DropWrite });
    private readonly ConcurrentQueue<(string User, long Run)> _manual = new();
    private readonly ConcurrentDictionary<string, long> _running = new(StringComparer.Ordinal);
    private readonly Lock _refreshLock = new();
    private long _lastMark = DateTimeOffset.MinValue.UtcTicks;
    private long _runs;
    private long _failures;
    private long _notFound;
    private long _tokens;
    private long _lastRunAt;

    public ProposalRunner(ProposalStore store, AiProviderRegistry ai, AiInstructions instructions, SongSuggestions songs,
        ProposalSuggester suggester, SongResolver resolver, AudiobookLikes books, LikeStore likes, PlaylistStore playlists,
        PlayHistoryStore history, UserStore users, ApiEvents events, ILogger<ProposalRunner> log, TimeProvider? time = null)
    {
        (_store, _ai, _instructions, _songs, _suggester, _resolver, _books) = (store, ai, instructions, songs, suggester, resolver, books);
        (_likes, _playlists, _history, _users, _events, _log) = (likes, playlists, history, users, events, log);
        _time = time ?? TimeProvider.System;
        likes.Added += (_, _) => MarkChanged();
        likes.Removed += (_, _) => MarkChanged();
        playlists.FavoriteAdded += (_, _) => MarkChanged();
        playlists.FavoriteRemoved += (_, _) => MarkChanged();
        ai.SettingsChanged += (_, _) => Wake();
    }

    public ProposalStats Stats
    {
        get
        {
            var last = Interlocked.Read(ref _lastRunAt);
            return new ProposalStats(Interlocked.Read(ref _runs), Interlocked.Read(ref _failures),
                last == 0 ? null : new DateTimeOffset(last, TimeSpan.Zero), NextRunAt(UserStore.Household),
                Interlocked.Read(ref _notFound), Interlocked.Read(ref _tokens));
        }
    }

    // Asks the background check again now, as a mark or a setting does.
    public void Wake() => _wake.Writer.TryWrite(true);

    public Result<ProposalsView, ProposalError> View(string? user)
    {
        var scope = Scope(user);
        if (!scope.IsSuccess)
            return scope.Failure;
        var lists = _store.Lists(scope.Value);
        if (!lists.IsSuccess)
            return lists.Failure;
        var options = _ai.Settings.ProposalOptions;
        var chosen = _ai.Choose(null, null);
        // A proposal liked or made a favourite since leaves the list.
        var marked = Marks(scope.Value);
        List<ProposalItem> Unmarked(IReadOnlyList<ProposalItem> items) =>
            [.. items.Where(i => !marked.Items.Contains((i.Kind, i.ProviderId, i.ProviderItemId)))];
        var l = lists.Value;
        return new ProposalsView(new ProposalBackground(options.Enabled, options.IntervalHours, NextRunAt(scope.Value)),
            chosen.IsSuccess, chosen.IsSuccess ? null : chosen.Failure.Detail, l.Latest,
            Unmarked(l.Tracks), Unmarked(l.Albums), Unmarked(l.Artists), Unmarked(l.Audiobooks));
    }

    public Result<ProposalRun, ProposalError> Refresh(string? user)
    {
        var scope = Scope(user);
        if (!scope.IsSuccess)
            return scope.Failure;
        var chosen = _ai.Choose(null, null);
        if (!chosen.IsSuccess)
            return Result.Fail(ProposalError.NotConfigured, $"no AI to ask: {chosen.Failure.Detail}");
        Result<ProposalRun, ProposalError> run;
        lock (_refreshLock)
        {
            if (_running.TryGetValue(scope.Value, out var running) && _store.GetRun(running) is { IsSuccess: true, Value: { } same })
                return same;
            run = Begin(scope.Value, ProposalTrigger.Manual);
            if (!run.IsSuccess)
                return run;
            _manual.Enqueue((scope.Value, run.Value.Id));
        }
        Wake();
        return run;
    }

    public Result<bool, ProposalError> Dismiss(long proposal)
    {
        var dismissed = _store.Dismiss(proposal, _time.GetUtcNow());
        if (!dismissed.IsSuccess)
            return dismissed.Failure;
        var user = dismissed.Value.User;
        if (_store.Latest(user) is { IsSuccess: true, Value: { } latest })
            _events.Publish("proposals.changed", new { user, run = latest, state = latest.State });
        return true;
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        _store.FailInterrupted(_time.GetUtcNow());
        try
        {
            while (!stoppingToken.IsCancellationRequested)
            {
                while (_manual.TryDequeue(out var manual))
                    await RunAsync(manual.User, manual.Run, stoppingToken).ConfigureAwait(false);
                await BackgroundAsync(stoppingToken).ConfigureAwait(false);

                using var tick = CancellationTokenSource.CreateLinkedTokenSource(stoppingToken);
                var delay = Task.Delay(Tick, _time, tick.Token);
                await Task.WhenAny(_wake.Reader.WaitToReadAsync(stoppingToken).AsTask(), delay).ConfigureAwait(false);
                await tick.CancelAsync().ConfigureAwait(false);
                while (_wake.Reader.TryRead(out _))
                {
                }
            }
        }
        catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
        {
            // Stopping: a run cut short is failed by FailInterrupted at the next start.
        }
    }

    private void MarkChanged()
    {
        Interlocked.Exchange(ref _lastMark, _time.GetUtcNow().UtcTicks);
        Wake();
    }

    private Result<string, ProposalError> Scope(string? user)
    {
        var resolved = _users.Resolve(user);
        return resolved.IsSuccess
            ? resolved.Value
            : Result.Fail(resolved.Failure.Code == LibraryError.InvalidArgument ? ProposalError.InvalidArgument : ProposalError.Unavailable,
                resolved.Failure.Detail);
    }

    private Result<ProposalRun, ProposalError> Begin(string scope, ProposalTrigger trigger)
    {
        var run = _store.Start(scope, trigger, Fingerprint(scope), _time.GetUtcNow());
        if (!run.IsSuccess)
            return run;
        _running[scope] = run.Value.Id;
        _events.Publish("proposals.running", new { user = scope, run = run.Value });
        return run;
    }

    // --- the background check -------------------------------------------------

    private async Task BackgroundAsync(CancellationToken cancellationToken)
    {
        var options = _ai.Settings.ProposalOptions;
        if (!options.Enabled || !_ai.Choose(null, null).IsSuccess)
            return;
        var now = _time.GetUtcNow();
        if (now - new DateTimeOffset(Interlocked.Read(ref _lastMark), TimeSpan.Zero) < TimeSpan.FromMinutes(options.DebounceMinutes))
            return;
        var today = _store.RunsSince(new DateTimeOffset(now.UtcDateTime.Date, TimeSpan.Zero));
        if (!today.IsSuccess)
            return;
        var runs = today.Value;
        foreach (var scope in OwnScopes())
        {
            if (runs >= options.MaxRunsPerDay)
                break;
            if (_running.ContainsKey(scope) || !Due(scope, now, options))
                continue;
            var run = Begin(scope, ProposalTrigger.Background);
            if (!run.IsSuccess)
                continue;
            runs++;
            await RunAsync(scope, run.Value.Id, cancellationToken).ConfigureAwait(false);
        }
    }

    // The last run older than the interval, and the seed changed since.
    private bool Due(string scope, DateTimeOffset now, ProposalSettings options)
    {
        if (_store.Latest(scope) is not { IsSuccess: true } latest)
            return false;
        if (latest.Value is { } last && now - last.Started < TimeSpan.FromHours(options.IntervalHours))
            return false;
        return latest.Value?.Fingerprint != Fingerprint(scope);
    }

    // The household and each user, when they have marks of their own.
    private List<string> OwnScopes()
    {
        var likes = _likes.Likes(null, UserStore.Everyone);
        var favorites = _playlists.Favorites(UserStore.Everyone);
        if (!likes.IsSuccess || !favorites.IsSuccess)
            return [];
        return [.. likes.Value.Select(l => l.User).Concat(favorites.Value.Select(f => f.User))
            .Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal)];
    }

    private DateTimeOffset? NextRunAt(string scope)
    {
        var options = _ai.Settings.ProposalOptions;
        if (!options.Enabled || !_ai.Choose(null, null).IsSuccess || _store.Latest(scope) is not { IsSuccess: true } latest)
            return null;
        if (latest.Value is { } last && last.Fingerprint == Fingerprint(scope))
            return null; // until a mark changes
        var now = _time.GetUtcNow();
        var after = latest.Value is { } run ? run.Started + TimeSpan.FromHours(options.IntervalHours) : now;
        var settled = new DateTimeOffset(Interlocked.Read(ref _lastMark), TimeSpan.Zero) + TimeSpan.FromMinutes(options.DebounceMinutes);
        return new[] { after, settled, now }.Max();
    }

    // --- one run ----------------------------------------------------------------

    private sealed record Seed(List<Track> MarkedTracks, ProposalSeed Names, List<Track> SeedTracks, List<Track> Plays);

    private sealed record Kind(string Name, List<NewProposal> Found, int NotFound, ProposalUsage Usage, string? Error);

    private async Task RunAsync(string scope, long run, CancellationToken cancellationToken)
    {
        using var origin = AiOriginScope.Begin(AiOrigin.Proposals);
        var options = _ai.Settings.ProposalOptions;
        var request = options.Request ?? _instructions.Proposals.DefaultRequest;
        var kinds = new List<Kind>();
        string? error = null;
        var chosen = _ai.Choose(null, null);
        var marks = Marks(scope);
        var avoid = _store.Avoid(scope);
        var seed = SeedOf(scope);
        if (!chosen.IsSuccess)
            error = $"no AI to ask: {chosen.Failure.Detail}";
        else if (!avoid.IsSuccess)
            error = avoid.Failure.Detail;
        else if (!seed.IsSuccess)
            error = seed.Failure.Detail;
        else
        {
            var (provider, model) = chosen.Value;
            var level = _ai.Settings.ReasoningLevels.Suggestions;
            if (options.Tracks > 0)
            {
                kinds.Add(await TracksAsync(request, seed.Value, marks, avoid.Value, options.Tracks, provider, model, level,
                    cancellationToken).ConfigureAwait(false));
            }
            foreach (var (kind, count) in new[]
                     {
                         (ProposalAsk.Albums, options.Albums), (ProposalAsk.Artists, options.Artists),
                         (ProposalAsk.Audiobooks, options.Audiobooks),
                     }.Where(k => k.Item2 > 0))
            {
                kinds.Add(await NamesAsync(kind, request, seed.Value, marks, avoid.Value, count, provider, model, level,
                    cancellationToken).ConfigureAwait(false));
            }
        }

        var failed = kinds.Where(k => k.Error is not null).ToList();
        var state = error is not null || (kinds.Count > 0 && failed.Count == kinds.Count)
            ? ProposalRunState.Failed
            : failed.Count > 0 ? ProposalRunState.Partial : ProposalRunState.Ok;
        error ??= failed.Count == 0 ? null : string.Join("; ", failed.Select(k => $"{k.Name}: {k.Error}"));
        var usage = new ProposalUsage(kinds.Sum(k => k.Usage.InputTokens), kinds.Sum(k => k.Usage.OutputTokens),
            kinds.Sum(k => k.Usage.ReasoningTokens), kinds.Sum(k => k.Usage.WebSearches));
        var notFound = kinds.Sum(k => k.NotFound);
        var finished = _store.Finish(run, state, error, usage, notFound, [.. kinds.SelectMany(k => k.Found)],
            options.Keep, _time.GetUtcNow());
        _running.TryRemove(scope, out _);

        Interlocked.Increment(ref _runs);
        if (state == ProposalRunState.Failed)
            Interlocked.Increment(ref _failures);
        Interlocked.Add(ref _notFound, notFound);
        Interlocked.Add(ref _tokens, usage.InputTokens + usage.OutputTokens + usage.ReasoningTokens);
        Interlocked.Exchange(ref _lastRunAt, _time.GetUtcNow().UtcTicks);
        if (state == ProposalRunState.Failed)
            LogFailed(_log, scope.Length == 0 ? "the household" : "a user", error ?? "");
        if (finished.IsSuccess)
            _events.Publish("proposals.changed", new { user = scope, run = finished.Value, state });
    }

    private async Task<Kind> TracksAsync(string request, Seed seed, Marked marks, ProposalAvoid avoid, int count,
        string provider, string model, AiReasoning level, CancellationToken cancellationToken)
    {
        var answer = await _songs.SuggestAsync(request, [.. seed.SeedTracks, .. seed.Plays.Take(SeedPlays)],
            seed.MarkedTracks, count, provider, model, level, cancellationToken,
            [.. seed.Plays, .. avoid.Tracks], avoid.Keys, MaxRounds).ConfigureAwait(false);
        if (!answer.IsSuccess)
            return new Kind("tracks", [], 0, new ProposalUsage(), Describe(answer.Failure));
        var found = answer.Value.Songs.Where(s => s.Track is { } t && !marks.Items.Contains((ProposalKind.Track, t.ProviderId, t.ProviderItemId)))
            .Select(s => new NewProposal(ProposalKind.Track, s.Track!.ProviderId, s.Track.ProviderItemId,
                SongResolver.SongKey(s.Track.Title, s.Track.Artist), s.Track, null))
            .ToList();
        return new Kind("tracks", found, answer.Value.NotFound, UsageOf(answer.Value.Usage), null);
    }

    private async Task<Kind> NamesAsync(ProposalAsk kind, string request, Seed seed, Marked marks, ProposalAvoid avoid,
        int count, string provider, string model, AiReasoning level, CancellationToken cancellationToken)
    {
        var name = JsonNamingPolicy.CamelCase.ConvertName(kind.ToString());
        var proposalKind = kind switch
        {
            ProposalAsk.Albums => ProposalKind.Album,
            ProposalAsk.Artists => ProposalKind.Artist,
            _ => ProposalKind.Audiobook,
        };
        var found = new List<NewProposal>();
        var named = new List<ProposedName>();
        var notFound = 0;
        long input = 0, output = 0, thought = 0, searched = 0;
        var exclude = marks.Items.Concat(avoid.Items).ToHashSet();
        var keys = new HashSet<string>(marks.Keys.Concat(avoid.Keys), StringComparer.Ordinal);
        string? error = null;
        for (var round = 0; round < MaxRounds && found.Count < count; round++)
        {
            var asked = await _suggester.SuggestAsync(new ProposalRequest(kind, request, seed.Names, named,
                Math.Min(2 * (count - found.Count), ProposalSuggester.MaxCount), provider, model, level),
                cancellationToken).ConfigureAwait(false);
            if (!asked.IsSuccess)
            {
                // A first round's failure is the kind's; a later one's leaves what was found.
                if (round == 0)
                    error = Describe(asked.Failure);
                break;
            }
            (input, output) = (input + asked.Value.Usage.InputTokens, output + asked.Value.Usage.OutputTokens);
            (thought, searched) = (thought + asked.Value.Usage.ReasoningTokens, searched + asked.Value.Usage.WebSearches);
            var names = asked.Value.Items;
            if (names.Count == 0)
                continue;
            named.AddRange(names);
            var resolved = await ResolveAsync(proposalKind, names, exclude, keys, cancellationToken).ConfigureAwait(false);
            foreach (var proposal in resolved)
            {
                if (proposal is null)
                {
                    notFound++;
                    continue;
                }
                if (found.Count == count)
                    continue;
                found.Add(proposal);
                exclude.Add((proposal.Kind, proposal.ProviderId, proposal.ProviderItemId));
                keys.Add(proposal.Key);
            }
        }
        return new Kind(name, found, notFound, new ProposalUsage(input, output, thought, searched), error);
    }

    // Each name as the house can play it, or null; in the names' order.
    private async Task<List<NewProposal?>> ResolveAsync(ProposalKind kind, IReadOnlyList<ProposedName> names,
        HashSet<(ProposalKind, string, string)> exclude, HashSet<string> keys, CancellationToken cancellationToken)
    {
        var excluded = exclude.Where(e => e.Item1 == kind).Select(e => (e.Item2, e.Item3)).ToHashSet();
        if (kind == ProposalKind.Artist)
        {
            var artists = await _resolver.ResolveArtistsAsync([.. names.Select(n => n.Title)], excluded, keys, cancellationToken)
                .ConfigureAwait(false);
            if (!artists.IsSuccess)
                return [.. names.Select(_ => (NewProposal?)null)];
            return [.. artists.Value.Artists.Zip(names).Select(p => p.First.Artist is { } a
                ? new NewProposal(kind, a.ProviderId, a.ProviderItemId, SongResolver.ArtistKey(a.Name), a, p.Second.Reason)
                : null)];
        }
        var albums = await _resolver.ResolveAlbumsAsync([.. names.Select(n => (n.Title, n.Artist))],
            kind == ProposalKind.Album ? excluded : [], keys, cancellationToken).ConfigureAwait(false);
        if (!albums.IsSuccess)
            return [.. names.Select(_ => (NewProposal?)null)];
        var answered = new List<NewProposal?>();
        foreach (var (album, name) in albums.Value.Albums.Zip(names))
        {
            if (album.Album is not { } a)
            {
                answered.Add(null);
            }
            else if (kind == ProposalKind.Album)
            {
                answered.Add(new NewProposal(kind, a.ProviderId, a.ProviderItemId, SongResolver.AlbumKey(a.Title, a.Artist), a,
                    name.Reason));
            }
            else
            {
                // A book is an album the classifier groups as one (phase 37).
                var (book, _) = await _books.ClassifyAsync(a.ProviderId, a.ProviderItemId, cancellationToken).ConfigureAwait(false);
                var item = book is null ? null : LikeStore.BookRef(book.Reference);
                answered.Add(book is null || item!.ProviderId.Length == 0 || excluded.Contains((item.ProviderId, item.ProviderItemId))
                    ? null
                    : new NewProposal(kind, item.ProviderId, item.ProviderItemId, SongResolver.AlbumKey(book.Title, book.Author),
                        book, name.Reason));
            }
        }
        return answered;
    }

    // --- the seed and the marks ---------------------------------------------------

    private sealed record Marked(HashSet<(ProposalKind, string, string)> Items, HashSet<string> Keys);

    // Everything the scope marked: never proposed to it.
    private Marked Marks(string scope)
    {
        var items = new HashSet<(ProposalKind, string, string)>();
        var keys = new HashSet<string>(StringComparer.Ordinal);
        if (_likes.Likes(null, scope) is { IsSuccess: true } likes)
        {
            foreach (var like in likes.Value)
            {
                switch (like)
                {
                    case { Track: { } t }:
                        items.Add((ProposalKind.Track, t.ProviderId, t.ProviderItemId));
                        keys.Add(SongResolver.SongKey(t.Title, t.Artist));
                        break;
                    case { Album: { } a }:
                        items.Add((ProposalKind.Album, a.ProviderId, a.ProviderItemId));
                        keys.Add(SongResolver.AlbumKey(a.Title, a.Artist));
                        break;
                    case { Artist: { } a }:
                        items.Add((ProposalKind.Artist, a.ProviderId, a.ProviderItemId));
                        keys.Add(SongResolver.ArtistKey(a.Name));
                        break;
                    case { Audiobook: { } b }:
                        var book = LikeStore.BookRef(b.Reference);
                        items.Add((ProposalKind.Audiobook, book.ProviderId, book.ProviderItemId));
                        keys.Add(SongResolver.AlbumKey(b.Title, b.Author));
                        break;
                }
            }
        }
        if (_playlists.Favorites(scope) is { IsSuccess: true } favorites)
        {
            foreach (var favorite in favorites.Value)
            {
                if (favorite.Track is { } t)
                {
                    items.Add((ProposalKind.Track, t.ProviderId, t.ProviderItemId));
                    keys.Add(SongResolver.SongKey(t.Title, t.Artist));
                }
                else if (favorite.Group is { Source: { } source } group)
                {
                    var book = LikeStore.BookRef(source);
                    items.Add((ProposalKind.Audiobook, book.ProviderId, book.ProviderItemId));
                    keys.Add(SongResolver.AlbumKey(group.Title, group.Artist));
                }
            }
        }
        return new Marked(items, keys);
    }

    // A hash over the scope's marks: a seed that did not change asks nothing.
    private string Fingerprint(string scope)
    {
        var lines = Marks(scope).Items.Select(i => $"{i.Item1}|{i.Item2}|{i.Item3}").Order(StringComparer.Ordinal);
        return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(string.Join('\n', lines))));
    }

    // Bounded so a large house costs the same tokens as a small one: the
    // newest of each kind and as many more at random, and the last plays.
    private Result<Seed, LibraryError> SeedOf(string scope)
    {
        var likes = _likes.Likes(null, scope);
        if (!likes.IsSuccess)
            return likes.Failure;
        var favorites = _playlists.Favorites(scope);
        if (!favorites.IsSuccess)
            return favorites.Failure;
        var plays = _history.Page(null, 0, AvoidPlays, scope);
        if (!plays.IsSuccess)
            return plays.Failure;

        var tracks = likes.Value.Where(l => l.Track is not null).Select(l => (l.Track!, l.Added))
            .Concat(favorites.Value.Where(f => f.Track is not null).Select(f => (f.Track!, f.Added)))
            .OrderByDescending(t => t.Added).Select(t => t.Item1)
            .DistinctBy(t => (t.ProviderId, t.ProviderItemId)).ToList();
        var albums = likes.Value.Where(l => l.Album is not null).OrderByDescending(l => l.Added).Select(l => l.Album!).ToList();
        var artists = likes.Value.Where(l => l.Artist is not null).OrderByDescending(l => l.Added).Select(l => l.Artist!).ToList();
        var books = likes.Value.Where(l => l.Audiobook is not null).Select(l => (l.Audiobook!, l.Added))
            .Concat(favorites.Value.Where(f => f.Group is { Source: not null }).Select(f => (Audiobook.Of(f.Group!), f.Added)))
            .OrderByDescending(b => b.Added).Select(b => b.Item1).DistinctBy(b => b.Reference).ToList();
        var played = plays.Value.Items.Select(p => p.Track).ToList();

        var seedTracks = Sample(tracks, SeedTracks);
        return new Seed(tracks, new ProposalSeed(
            [.. seedTracks.Select(SongSuggestion.Of)],
            [.. Sample(albums, SeedAlbums).Select(a => new SongSuggestion(a.Title, a.Artist, Year: a.Year))],
            [.. Sample(artists, SeedArtists).Select(a => a.Name)],
            [.. Sample(books, SeedAudiobooks).Select(b => new SongSuggestion(b.Title, b.Author))],
            [.. played.Take(SeedPlays).Select(SongSuggestion.Of)]), seedTracks, played);
    }

    // The newest `count`, then `count` more of the rest at random, in their order.
    private static List<T> Sample<T>(List<T> newestFirst, int count)
    {
        if (newestFirst.Count <= 2 * count)
            return newestFirst;
        var rest = newestFirst.Skip(count).Select((item, index) => (item, index)).ToList();
        var picked = rest.OrderBy(_ => Random.Shared.Next()).Take(count).OrderBy(p => p.index).Select(p => p.item);
        return [.. newestFirst.Take(count), .. picked];
    }

    private static ProposalUsage UsageOf(AiUsage usage) =>
        new(usage.InputTokens, usage.OutputTokens, usage.ReasoningTokens, usage.WebSearches);

    private static string Describe(Failure<AiError> failure) =>
        $"{JsonNamingPolicy.CamelCase.ConvertName(failure.Code.ToString())}: {failure.Detail}";

    // Who, not their name: a user's name is the household's own business.
    [LoggerMessage(Level = LogLevel.Warning, Message = "proposals for {Scope} failed: {Reason}")]
    private static partial void LogFailed(ILogger log, string scope, string reason);
}
