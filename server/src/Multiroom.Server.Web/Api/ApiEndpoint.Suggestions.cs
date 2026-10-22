using System.Diagnostics;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Ai;
using Multiroom.Server.Ai.Agent;
using Multiroom.Server.Playback;
using Multiroom.Server.Providers;

namespace Multiroom.Server.Web.Api;

// What a suggestion is about: exactly one of a zone's now-playing (zone), a
// zone's queue (its last ten tracks), a stored playlist (at most 50 entries)
// or tracks given (1 to 50). None: the request alone is asked.
public sealed record SuggestionSeed(string? Zone = null, string? Queue = null, long? Playlist = null,
    IReadOnlyList<Track>? Tracks = null);

// Where the found tracks go, in the AI's order; played from the first of them
// when asked.
public sealed record SuggestionTarget(string Zone, QueuePlacement Placement = QueuePlacement.End, bool Play = false);

public sealed record SuggestionsRequest(string Request, SuggestionSeed? Seed = null, int Count = 10,
    string? Provider = null, string? Model = null, SuggestionTarget? Queue = null, AiReasoning? Reasoning = null);

// The answer, and with `queue` how many went in and the queue afterwards;
// sources: what a web lookup cited (phase 27).
public sealed record SuggestionsView(
    IReadOnlyList<ResolvedSong> Songs,
    int Found,
    int NotFound,
    int Rounds,
    AiUsage Usage,
    IReadOnlyList<SearchFailure> Failures,
    int? Queued = null,
    QueueState? Queue = null,
    IReadOnlyList<AiSource>? Sources = null);

public sealed record ChatPatch(string Title);

// User is who speaks (list_users); absent, the zone's listener when a zone is
// named, else the household.
public sealed record CommandRequest(string Text, string? Zone = null, string? Provider = null, string? Model = null,
    string? ConversationId = null, AiReasoning? Reasoning = null, string? User = null);

// Song suggestions (phase 24) and the agent's commands (phase 25). The
// suggestion loop is SongSuggestions'; this reads the seed and queues what
// was found. A command runs the agent over its in-memory MCP connection and
// announces its start and end on the stream, without its text (user content).
public sealed partial class ApiEndpoint
{
    public const int SeedQueueTracks = 10;
    public const int SeedPlaylistEntries = 50;

    // A queue's or a playlist's tracks a seed is picked from: the liked and
    // favourite ones among these come first (LikeStore.MaxLookup).
    public const int SeedCandidates = 100;

    private void MapSuggestions(WebApplication app)
    {
        app.MapPost("/api/v1/ai/suggestions", (Delegate)SuggestAsync);
        app.MapPost("/api/v1/ai/commands", (Delegate)CommandAsync);
        app.MapGet("/api/v1/ai/chats", () => Json(chats.Chats));
        app.MapGet("/api/v1/ai/chats/{id}", (string id) => Answer(chats.Get(id)));
        app.MapMethods("/api/v1/ai/chats/{id}", [HttpMethods.Patch], (Delegate)((HttpContext c, string id) =>
            WithBody<ChatPatch, AgentChatSummary, AiError>(c, p => Task.FromResult(chats.Rename(id, p.Title)))));
        app.MapDelete("/api/v1/ai/chats/{id}", (string id) =>
        {
            var removed = chats.Remove(id);
            return removed.IsSuccess ? Results.NoContent() : Error(removed.Failure);
        });
    }

    private async Task<IResult> SuggestAsync(HttpContext context)
    {
        var body = await ApiJson.ReadAsync<SuggestionsRequest>(context.Request, optional: false, context.RequestAborted)
            .ConfigureAwait(false);
        if (!body.IsSuccess)
            return Error(body.Failure);
        var request = body.Value!;
        var ct = context.RequestAborted;

        var seed = SeedOf(request.Seed);
        if (!seed.IsSuccess)
            return Error(seed.Failure);
        var target = request.Queue;
        List<Track> queued = [];
        if (target is { Placement: not QueuePlacement.Replace } && queues.GetQueue(target.Zone) is { IsSuccess: true } existing)
            queued.AddRange(existing.Value.Items.Select(i => i.Track).OfType<Track>());
        if ((request.Seed?.Zone ?? request.Seed?.Queue) is { } seedZone && queues.GetQueue(seedZone) is { IsSuccess: true } seeded)
            queued.AddRange(seeded.Value.Items.Select(i => i.Track).OfType<Track>());

        using var origin = AiOriginScope.Begin(AiOrigin.Manual);
        var answer = await suggestions.SuggestAsync(request.Request, seed.Value, queued, request.Count, request.Provider,
            request.Model, request.Reasoning, ct).ConfigureAwait(false);
        if (!answer.IsSuccess)
            return Error(answer.Failure);
        var a = answer.Value;
        var view = new SuggestionsView(a.Songs, a.Found, a.NotFound, a.Rounds, a.Usage, a.Failures, Sources: a.Sources);
        if (target is null)
            return Json(view);

        List<Track> found = [.. a.Songs.Select(s => s.Track).OfType<Track>()];
        if (found.Count == 0)
            return Json(view with { Queued = 0, Queue = queues.GetQueue(target.Zone) is { IsSuccess: true } q ? q.Value : null });
        var before = queues.GetQueue(target.Zone) is { IsSuccess: true } b ? b.Value.Items.Select(i => i.Id).ToHashSet() : [];
        var added = target.Placement switch
        {
            QueuePlacement.Next => await queues.AddNextAsync(target.Zone, found, ct).ConfigureAwait(false),
            QueuePlacement.Replace => await queues.ReplaceAsync(target.Zone, found, ct).ConfigureAwait(false),
            _ => await queues.AddAsync(target.Zone, found, null, ct).ConfigureAwait(false),
        };
        if (!added.IsSuccess)
            return Error(added.Failure);
        var state = added.Value;
        if (target.Play)
        {
            var first = state.Items.First(i => !before.Contains(i.Id) || target.Placement == QueuePlacement.Replace).Id;
            var played = await queues.PlayAsync(target.Zone, first, ct).ConfigureAwait(false);
            if (!played.IsSuccess)
                return Error(played.Failure);
            state = played.Value;
        }
        return Json(view with { Queued = found.Count, Queue = state });
    }

    // The seed's tracks; none without a seed.
    private Result<IReadOnlyList<Track>, AiError> SeedOf(SuggestionSeed? seed)
    {
        if (seed is null)
            return Array.Empty<Track>();
        var kinds = (seed.Zone is null ? 0 : 1) + (seed.Queue is null ? 0 : 1) + (seed.Playlist is null ? 0 : 1) +
            (seed.Tracks is null ? 0 : 1);
        if (kinds != 1)
            return Result.Fail(AiError.InvalidArgument, "seed: exactly one of zone, queue, playlist or tracks");
        if (seed.Zone is { } zone)
        {
            var view = house.Zone(zone);
            if (!view.IsSuccess)
                return Result.Fail(AiError.NotFound, view.Failure.Detail);
            var queue = view.Value.Queue;
            var current = queue?.Items.FirstOrDefault(i => i.Id == queue.Current);
            return (current?.Track ?? (current?.Station is not null ? view.Value.RadioTrack : null)) is { } playing
                ? new[] { playing }
                : Result.Fail(AiError.InvalidArgument, $"seed.zone: {zone} has no track playing");
        }
        if (seed.Queue is { } queued)
        {
            var queue = queues.GetQueue(queued);
            if (!queue.IsSuccess)
                return Result.Fail(AiError.NotFound, queue.Failure.Detail);
            Track[] tracks = [.. suggestions.Pick([.. queue.Value.Items.Select(i => i.Track).OfType<Track>().TakeLast(SeedCandidates)],
                SeedQueueTracks, fromEnd: true)];
            return tracks.Length > 0 ? tracks : Result.Fail(AiError.InvalidArgument, $"seed.queue: {queued} holds no track");
        }
        if (seed.Playlist is { } playlist)
        {
            var entries = playlists.Entries(playlist);
            if (!entries.IsSuccess)
                return Result.Fail(AiError.NotFound, entries.Failure.Detail);
            Track[] tracks = [.. suggestions.Pick([.. entries.Value.Take(SeedCandidates).Select(e => e.Track).OfType<Track>()], SeedPlaylistEntries,
                fromEnd: false)];
            return tracks.Length > 0 ? tracks : Result.Fail(AiError.InvalidArgument, $"seed.playlist: {playlist} is empty");
        }
        Track[] given = [.. seed.Tracks!];
        return given.Length is 0 or > SongResolver.MaxSongs
            ? Result.Fail(AiError.InvalidArgument, $"seed.tracks: 1 to {SongResolver.MaxSongs}")
            : given;
    }

    private async Task<IResult> CommandAsync(HttpContext context)
    {
        var body = await ApiJson.ReadAsync<CommandRequest>(context.Request, optional: false, context.RequestAborted)
            .ConfigureAwait(false);
        if (!body.IsSuccess)
            return Error(body.Failure);
        var request = body.Value!;
        // The agent is told the zone by its name, as a person says it; the
        // tools take it back to the id.
        string? zoneName = null;
        var speaker = request.User;
        if (request.Zone is { } zone)
        {
            var known = house.Zone(zone);
            if (!known.IsSuccess)
                return Error(known.Failure);
            zoneName = known.Value.Name;
            speaker ??= known.Value.User;
        }
        var user = users.Resolve(speaker);
        if (!user.IsSuccess)
            return Error(user.Failure);

        // A chat kept in the store goes on where the agent has forgotten it.
        var chat = request.ConversationId is { } chatId && chats.Get(chatId) is { IsSuccess: true } kept ? kept.Value : null;
        var clock = Stopwatch.StartNew();
        var at = DateTimeOffset.UtcNow;
        events.Publish("ai.commandStarted", new { zone = request.Zone, conversationId = request.ConversationId });
        var reply = await agent.RunAsync(new AgentCommand(request.Text, zoneName, request.Provider, request.Model,
            request.ConversationId, request.Reasoning, chat is null ? null : AgentChatStore.History(chat), user.Value), ConnectAgentAsync,
            context.RequestAborted).ConfigureAwait(false);
        Keep(request, at, reply);
        events.Publish("ai.commandEnded", new
        {
            zone = request.Zone,
            conversationId = request.ConversationId,
            ok = reply.IsSuccess,
            toolCalls = reply.IsSuccess ? reply.Value.Actions.Count : 0,
            stoppedBy = reply.IsSuccess ? reply.Value.StoppedBy : ApiError.From(reply.Failure).Error,
            ms = clock.ElapsedMilliseconds,
        });
        return Answer(reply);
    }

    // Every command with a conversation id the agent took is a chat's turn;
    // one the store cannot write is still answered.
    private void Keep(CommandRequest request, DateTimeOffset at, Result<AgentReply, AiError> reply)
    {
        if (request.ConversationId is not { Length: > 0 and <= MultiroomAgent.MaxConversationIdLength } id ||
            string.IsNullOrWhiteSpace(id) || string.IsNullOrWhiteSpace(request.Text) ||
            request.Text.Length > MultiroomAgent.MaxTextLength)
            return;
        var turn = reply.IsSuccess
            ? new AgentChatTurn(request.Text.Trim(), at, reply.Value.Provider, reply.Value.Model, request.Reasoning, reply.Value.Reply,
                reply.Value.Actions, reply.Value.Usage, reply.Value.StoppedBy, reply.Value.Detail)
            : new AgentChatTurn(request.Text.Trim(), at, request.Provider, request.Model, request.Reasoning, null, [], null,
                Detail: reply.Failure.Detail, Error: ApiError.From(reply.Failure).Error);
        chats.Add(id, turn);
    }
}
