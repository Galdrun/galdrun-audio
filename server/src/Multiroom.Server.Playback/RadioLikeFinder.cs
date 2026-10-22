using System.Collections.Concurrent;
using System.Threading.Channels;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Providers;

namespace Multiroom.Server.Playback;

// Finds each song liked as a radio played it (RadioTrackMetadata) among the
// providers that stream tracks, the library before a streaming service
// (SongResolver), and keeps the track found beside the like (LikeStore.SetFound),
// so it plays, queues and goes in a playlist as any track. A close match is
// taken too and marked so. The like itself stays as it was.
//
// While PlaybackSettings.FindRadioLikes is on: every radio song's like not
// found yet is looked for shortly after start, a new one when it is liked,
// all of them again when the setting is turned on, and those not found again
// every Retry (a provider may have been away, or the library grown). One song
// at a time, on this service's own task.
// `startDelay` (StartDelay by default) lets the providers come up first.
public sealed partial class RadioLikeFinder(LikeStore likes, SongResolver resolver, PlaybackSettings settings,
    ILogger<RadioLikeFinder> log, TimeSpan? startDelay = null) : BackgroundService
{
    public static readonly TimeSpan StartDelay = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan Retry = TimeSpan.FromHours(1);

    // Like ids looked for and not found, until the next Retry.
    private readonly ConcurrentDictionary<long, byte> _notFound = new();

    private readonly Channel<bool> _wake =
        Channel.CreateBounded<bool>(new BoundedChannelOptions(1) { FullMode = BoundedChannelFullMode.DropWrite });

    public override Task StartAsync(CancellationToken cancellationToken)
    {
        likes.Added += OnLiked;
        settings.Changed += OnSettings;
        return base.StartAsync(cancellationToken);
    }

    public override async Task StopAsync(CancellationToken cancellationToken)
    {
        likes.Added -= OnLiked;
        settings.Changed -= OnSettings;
        await base.StopAsync(cancellationToken).ConfigureAwait(false);
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        try
        {
            await Task.Delay(startDelay ?? StartDelay, stoppingToken).ConfigureAwait(false);
            while (true)
            {
                if (settings.FindRadioLikes)
                    await SweepAsync(stoppingToken).ConfigureAwait(false);
                using var retry = CancellationTokenSource.CreateLinkedTokenSource(stoppingToken);
                retry.CancelAfter(Retry);
                try
                {
                    await _wake.Reader.ReadAsync(retry.Token).ConfigureAwait(false);
                }
                catch (OperationCanceledException) when (!stoppingToken.IsCancellationRequested)
                {
                    _notFound.Clear();
                }
            }
        }
        catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
        {
        }
    }

    private async Task SweepAsync(CancellationToken stoppingToken)
    {
        var unfound = likes.UnfoundRadioSongs();
        if (!unfound.IsSuccess)
        {
            LogUnreadable(log, unfound.Failure.Detail);
            return;
        }
        foreach (var like in unfound.Value)
        {
            if (!settings.FindRadioLikes)
                return;
            if (like.Track is { } song && !_notFound.ContainsKey(like.Id))
                await FindAsync(like.Id, song, stoppingToken).ConfigureAwait(false);
        }
    }

    private async Task FindAsync(long like, Track song, CancellationToken stoppingToken)
    {
        try
        {
            var resolved = await resolver.ResolveAsync([new SongSuggestion(song.Title, song.Artist, song.Album)], [],
                stoppingToken).ConfigureAwait(false);
            if (resolved is { IsSuccess: true, Value.Songs: [{ Track: { } track, Match: var match }] })
            {
                likes.SetFound(like, new FoundTrack(track, match == SongMatchKind.Close));
                LogFound(log, song.Title, track.ProviderId, track.ProviderItemId);
            }
            else
            {
                _notFound.TryAdd(like, 0);
            }
        }
        catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
        {
            throw;
        }
#pragma warning disable CA1031 // One song's failure must not end the loop for every other; it is logged and asked again later.
        catch (Exception e)
#pragma warning restore CA1031
        {
            LogFailed(log, e, song.Title);
            _notFound.TryAdd(like, 0);
        }
    }

    private void OnLiked(object? sender, LikedItem like)
    {
        if (like.Track?.ProviderId == RadioTrackMetadata.ProviderId)
            _wake.Writer.TryWrite(true);
    }

    private void OnSettings(object? sender, PlaybackSettingsView view)
    {
        if (!view.FindRadioLikes)
            return;
        _notFound.Clear();
        _wake.Writer.TryWrite(true);
    }

    [LoggerMessage(Level = LogLevel.Information, Message = "the liked radio song {Title} was found as {Provider} {Item}")]
    private static partial void LogFound(ILogger log, string title, string provider, string item);

    [LoggerMessage(Level = LogLevel.Warning, Message = "looking for the liked radio song {Title} failed")]
    private static partial void LogFailed(ILogger log, Exception e, string title);

    [LoggerMessage(Level = LogLevel.Warning, Message = "the liked radio songs cannot be read: {Reason}")]
    private static partial void LogUnreadable(ILogger log, string reason);
}
