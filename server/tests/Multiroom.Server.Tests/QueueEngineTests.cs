using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text.Json.Nodes;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Playback;
using Multiroom.Server.Playback.Core;
using Multiroom.Server.Providers;

namespace Multiroom.Server.Tests;

// The queue engine against FakeCore, with the fake provider's tracks. FakeCore
// does not play: each test scripts the events the core would send, in the
// order it would send them, and reads the commands the engine answered with.
public sealed partial class QueueEngineTests : IAsyncDisposable
{
    private const string Zone = "living";
    private static readonly TimeSpan Patience = TimeSpan.FromSeconds(10);

    private readonly TempDirectory _dir = new();
    private readonly FakeCore _core = new();
    private readonly FakeProvider _fake = new();
    private readonly FakeMedia _media = new();
    private readonly Synchronised _synchronised = new();
    private readonly ManualTime _time = new();
    private readonly ServiceProvider _services = new ServiceCollection().BuildServiceProvider();
    private readonly CoreLink _link;
    private readonly ProviderRegistry _providers;
    private readonly QueueEngine _engine;
    private CoreLink? _restartedLink;

    public QueueEngineTests()
    {
        _link = new CoreLink(new CoreLinkOptions("127.0.0.1", _core.Port), [_synchronised], NullLogger<CoreLink>.Instance);
        _providers = new ProviderRegistry([new ProviderRegistration(FakeProvider.ProviderId, _ => _fake)], _services,
            NullLogger<ProviderRegistry>.Instance);
        _engine = new QueueEngine(_link, _providers, _media, NullLogger<QueueEngine>.Instance, _time);
        // Answered as the core does when it had something staged to clear.
        _core.Answer("source.clearStaged", p => new JsonObject { ["zone"] = p["zone"]!.DeepClone(), ["cleared"] = 1 });
    }

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    public async ValueTask DisposeAsync()
    {
        await _link.StopAsync(CancellationToken.None);
        _link.Dispose();
        if (_restartedLink is not null)
        {
            await _restartedLink.StopAsync(CancellationToken.None);
            _restartedLink.Dispose();
        }
        await _core.DisposeAsync();
        await _providers.DisposeAsync();
        await _services.DisposeAsync();
        _dir.Dispose();
    }

    private async Task<Track[]> StartAsync(params string[] titles)
    {
        await _providers.StartAsync(Ct);
        await _link.StartAsync(Ct);
        await _synchronised.Next.Task.WaitAsync(Patience, Ct);
        return [.. titles.Select(t => _fake.AddTrack(_dir.Write($"{t}.flac", $"the bytes of {t}"), t))];
    }

    // The engine's queue in `living`, once it is not empty and `playing` is
    // playing: A's source staged and played, B's staged behind it at the
    // core's sourceNeeded, and A started.
    private async Task<(Track[] Tracks, string[] SourceIds)> PlayingAsync(params string[] titles)
    {
        var tracks = await StartAsync(titles);
        Ok(await _engine.AddAsync(Zone, tracks, null, Ct));
        Ok(await _engine.SetClientsAsync(Zone, ["roomA"], Ct));
        Ok(await _engine.PlayAsync(Zone, null, Ct));
        var first = Staged()[0].SourceId;
        await EmitAsync($$"""{"event":"zone.sourceNeeded","zone":"living","afterSourceId":"{{first}}"}""");
        await Until(() => Staged().Count == 2);
        await EmitAsync(
            $$"""{"event":"zone.transport","zone":"living","state":"playing","timelineNs":1000}""",
            $$"""{"event":"zone.sourceStarted","zone":"living","sourceId":"{{first}}","timelineNs":1000,"serverTimeNs":5000,"startOffsetNs":0}""");
        await Until(() => Queue().NowPlaying is not null);
        return (tracks, [.. Staged().Select(s => s.SourceId)]);
    }

    [Fact]
    public async Task Play_creates_the_zone_sets_its_rooms_stages_the_first_item_and_plays()
    {
        var tracks = await StartAsync("A", "B");
        var added = Ok(await _engine.AddAsync(Zone, tracks, null, Ct));
        Assert.Equal(["A", "B"], added.Items.Select(i => i.Title));

        Ok(await _engine.SetClientsAsync(Zone, ["roomA"], Ct));
        var playing = Ok(await _engine.PlayAsync(Zone, null, Ct));

        var stage = Staged()[0];
        Assert.Equal(
            [
                ("zone.create", """{"zone":"living"}"""),
                ("zone.setClients", """{"zone":"living","clients":["roomA"]}"""),
                ("source.stage", $$"""{"zone":"living","sourceId":"{{stage.SourceId}}","mediaUri":"{{stage.MediaUri}}","trackId":"fake:t0","title":"A"}"""),
                ("zone.play", """{"zone":"living"}"""),
            ],
            Sent());
        Assert.Equal(added.Items[0].Id, playing.Current);
        Assert.Equal(QueueTransport.Starting, playing.Transport);
        Assert.Equal([new Uri(stage.MediaUri)], _media.Live);
    }

    [Fact]
    public async Task A_staged_track_takes_the_format_its_provider_resolved_it_as()
    {
        var tracks = await StartAsync("A", "B");
        var aac = new AudioFormat("AAC", Channels: 2, Bitrate: 320);
        _fake.Granted[tracks[0].ProviderItemId] = aac;
        Ok(await _engine.AddAsync(Zone, tracks, null, Ct));
        var playing = Ok(await _engine.PlayAsync(Zone, null, Ct));

        Assert.Equal(aac, playing.Items[0].Track!.Format);
        Assert.Equal(tracks[0] with { Format = aac }, playing.Items[0].Track);
        Assert.Null(playing.Items[1].Track!.Format);
    }

    [Fact]
    public async Task The_staging_loop_answers_sourceNeeded_in_time_and_never_stages_two_ahead()
    {
        var tracks = await StartAsync("A", "B", "C");
        Ok(await _engine.AddAsync(Zone, tracks, null, Ct));
        Ok(await _engine.PlayAsync(Zone, null, Ct));
        var a = Staged()[0].SourceId;

        var clock = Stopwatch.StartNew();
        await EmitAsync($$"""{"event":"zone.sourceNeeded","zone":"living","afterSourceId":"{{a}}","remainingNs":180000000000}""");
        await Until(() => Staged().Count == 2);
        Assert.True(clock.Elapsed < TimeSpan.FromSeconds(1), $"answered in {clock.Elapsed}");
        var b = Staged()[1];
        Assert.Equal("B", b.Title);

        // The same question again, and the start of A: B is staged, so
        // nothing more is.
        await EmitAsync(
            $$"""{"event":"zone.sourceNeeded","zone":"living","afterSourceId":"{{a}}"}""",
            $$"""{"event":"zone.sourceStarted","zone":"living","sourceId":"{{a}}","timelineNs":10,"serverTimeNs":20,"startOffsetNs":0}""");
        await Until(() => Queue().NowPlaying is not null);
        Assert.Equal(2, Staged().Count);
        var state = Queue();
        Assert.Equal(new NowPlaying(state.Items[0].Id, a, 10, 20, 0), state.NowPlaying);

        // B opened, then heard: C is staged behind it, and A's URL retired.
        await EmitAsync(
            $$"""{"event":"zone.sourceNeeded","zone":"living","afterSourceId":"{{b.SourceId}}"}""",
            $$"""{"event":"zone.sourceStarted","zone":"living","sourceId":"{{b.SourceId}}","timelineNs":300,"serverTimeNs":400,"startOffsetNs":0}""");
        await Until(() => Queue().Current == state.Items[1].Id);
        Assert.Equal(["A", "B", "C"], Staged().Select(s => s.Title));
        Assert.Equal([.. Staged().Skip(1).Select(s => new Uri(s.MediaUri))], _media.Live);
    }

    [Fact]
    public async Task A_reorder_mid_track_clears_and_restages_exactly_once()
    {
        var (_, ids) = await PlayingAsync("A", "B", "C");
        var queue = Queue();
        var before = _core.All.Count;

        var moved = Ok(await _engine.MoveAsync(Zone, queue.Items[2].Id, 1, Ct));

        Assert.Equal(["A", "C", "B"], moved.Items.Select(i => i.Title));
        var c = Staged()[^1];
        Assert.Equal("C", c.Title);
        Assert.Equal(
            [
                ("source.clearStaged", """{"zone":"living"}"""),
                ("source.stage", $$"""{"zone":"living","sourceId":"{{c.SourceId}}","mediaUri":"{{c.MediaUri}}","trackId":"fake:t2","title":"C"}"""),
            ],
            Sent(before));

        // The core asks again after a clear behind a playing source; C is
        // already its answer.
        await EmitAsync(
            $$"""{"event":"zone.sourceNeeded","zone":"living","afterSourceId":"{{ids[0]}}"}""",
            """{"event":"zone.transport","zone":"living","state":"paused","timelineNs":2000}""");
        await Until(() => Queue().Transport == QueueTransport.Paused);
        Assert.Equal(2, Sent(before).Count);
        // B's URL is retired, A's and C's are live.
        Assert.Equal(2, _media.Live.Count);
    }

    [Fact]
    public async Task Next_is_one_stop_one_clear_of_the_staged_source_one_stage_and_one_play()
    {
        await PlayingAsync("A", "B", "C");
        var queue = Queue();
        var before = _core.All.Count;

        var next = Ok(await _engine.NextAsync(Zone, Ct));

        var b = Staged()[^1];
        Assert.Equal("B", b.Title);
        Assert.Equal(
            [
                ("zone.stop", """{"zone":"living"}"""),
                ("source.clearStaged", """{"zone":"living"}"""),
                ("source.stage", $$"""{"zone":"living","sourceId":"{{b.SourceId}}","mediaUri":"{{b.MediaUri}}","trackId":"fake:t1","title":"B"}"""),
                ("zone.play", """{"zone":"living"}"""),
            ],
            Sent(before));
        Assert.Equal(queue.Items[1].Id, next.Current);
        Assert.Null(next.NowPlaying);
        // Only the new B is served; the old A and B are retired.
        Assert.Equal([new Uri(b.MediaUri)], _media.Live);
    }

    [Fact]
    public async Task Seek_in_the_current_item_is_zone_seek_and_in_another_is_a_start_at_its_offset()
    {
        var (_, ids) = await PlayingAsync("A", "B", "C");
        var queue = Queue();
        _core.Answer("zone.seek", p => new JsonObject
        {
            ["zone"] = Zone,
            ["sourceId"] = ids[0],
            ["positionNs"] = p["positionNs"]!.DeepClone(),
            ["cutTimelineNs"] = 5000,
            ["timelineNs"] = 5150,
        });
        var before = _core.All.Count;

        var sought = Ok(await _engine.SeekAsync(Zone, queue.Items[0].Id, TimeSpan.FromSeconds(90), Ct));

        Assert.Equal([("zone.seek", """{"zone":"living","positionNs":90000000000}""")], Sent(before));
        Assert.Equal(new NowPlaying(queue.Items[0].Id, ids[0], 5150, null, 90_000_000_000), sought.NowPlaying);
        Assert.Equal(90_000, sought.PositionMs);

        before = _core.All.Count;
        var other = Ok(await _engine.SeekAsync(Zone, queue.Items[2].Id, TimeSpan.FromSeconds(30.5), Ct));

        var c = Staged()[^1];
        Assert.Equal(
            [
                ("zone.stop", """{"zone":"living"}"""),
                ("source.clearStaged", """{"zone":"living"}"""),
                ("source.stage", $$"""{"zone":"living","sourceId":"{{c.SourceId}}","mediaUri":"{{c.MediaUri}}","trackId":"fake:t2","title":"C","startOffsetNs":30500000000}"""),
                ("zone.play", """{"zone":"living"}"""),
            ],
            Sent(before));
        Assert.Equal(queue.Items[2].Id, other.Current);
    }

    [Fact]
    public async Task Repeat_one_restages_the_same_track_under_a_new_sourceId()
    {
        var tracks = await StartAsync("A", "B");
        Ok(await _engine.AddAsync(Zone, tracks, null, Ct));
        Ok(await _engine.SetRepeatAsync(Zone, RepeatMode.One, Ct));
        Ok(await _engine.PlayAsync(Zone, null, Ct));
        var first = Staged()[0];

        await EmitAsync($$"""{"event":"zone.sourceNeeded","zone":"living","afterSourceId":"{{first.SourceId}}"}""");
        await Until(() => Staged().Count == 2);

        var again = Staged()[1];
        Assert.Equal(("A", "fake:t0"), (again.Title, again.TrackId));
        Assert.NotEqual(first.SourceId, again.SourceId);
        Assert.NotEqual(first.MediaUri, again.MediaUri);
    }

    [Fact]
    public async Task The_session_survives_events_lost_and_a_reconnect_with_the_model_and_the_desired_state_intact()
    {
        var (_, ids) = await PlayingAsync("A", "B", "C", "D");
        var queue = Queue();

        // Events were lost while B was opened: the core has A audible and
        // nothing staged, so C is staged behind B.
        _core.StatusBody = Status(source: ids[0], staged: []);
        var before = _core.All.Count;
        await EmitAsync("""{"event":"events.lost","count":5}""");
        await Until(() => Staged().Count == 3);
        Assert.Equal(["status", "source.stage"], _core.All.Skip(before).Select(r => r.Command));
        Assert.Equal("C", Staged()[2].Title);
        var c = Staged()[2].SourceId;

        // The connection drops. The core kept everything, and has since
        // started B, with C staged.
        _core.StatusBody = Status(source: ids[1], staged: [c]);
        _core.Drop();
        await Until(() => Queue().Current == queue.Items[1].Id);
        await Until(() => _link.Connections == 2);
        await EmitAsync($$"""{"event":"zone.sourceNeeded","zone":"living","afterSourceId":"{{c}}"}""");
        await Until(() => Staged().Count == 4);

        Assert.Equal(["events.subscribe", "status", "source.stage"], _core.On(2).Select(r => r.Command));
        Assert.Equal("D", Staged()[3].Title);
        Assert.Empty(_link.Desired.Diff(_link.Model.Current));
        Assert.Equal(queue.Items[1].Id, Queue().NowPlaying!.Item);
        // A is done with: B, C and D are served.
        Assert.Equal(3, _media.Live.Count);
    }

    [Fact]
    public async Task A_core_that_restarts_mid_track_gets_its_zone_rooms_source_at_the_playhead_and_play_back()
    {
        await PlayingAsync("A", "B", "C");
        Ok(await _engine.SetVolumeAsync("roomA", 0.5, null, Ct));
        var queue = Queue();
        _time.Advance(TimeSpan.FromSeconds(30));

        // The core exits and comes back empty.
        _core.StatusBody = FakeCore.EmptyStatus;
        _core.Drop();
        await Until(() => _core.On(2).Any(r => r.Command == "zone.play"));

        var a = Staged()[^1];
        Assert.Equal(
            [
                ("events.subscribe", "{}"),
                ("status", "{}"),
                ("zone.create", """{"zone":"living"}"""),
                ("zone.setClients", """{"zone":"living","clients":["roomA"]}"""),
                ("client.setVolume", """{"client":"roomA","value":0.5}"""),
                ("source.stage", $$"""{"zone":"living","sourceId":"{{a.SourceId}}","mediaUri":"{{a.MediaUri}}","trackId":"fake:t0","title":"A","startOffsetNs":30000000000}"""),
                ("zone.play", """{"zone":"living"}"""),
            ],
            _core.On(2).Select(r => (r.Command, r.Params)));
        await Until(() => Queue().Transport == QueueTransport.Starting);
        Assert.Equal(queue.Items[0].Id, Queue().Current);
        // Only the new source is served: the old ones died with the core.
        Assert.Equal([new Uri(a.MediaUri)], _media.Live);
    }

    [Fact]
    public async Task A_paused_zone_whose_core_restarts_stays_stopped_and_plays_on_from_where_it_paused()
    {
        await PlayingAsync("A", "B");
        _time.Advance(TimeSpan.FromSeconds(12));
        Assert.Equal(12_000, Queue().PositionMs);
        Ok(await _engine.PauseAsync(Zone, Ct));
        _time.Advance(TimeSpan.FromMinutes(5));
        // A paused playhead stands still.
        Assert.Equal(12_000, Queue().PositionMs);

        _core.StatusBody = FakeCore.EmptyStatus;
        _core.Drop();
        await Until(() => _core.On(2).Any(r => r.Command == "zone.setClients"));
        await Until(() => Queue().Transport == QueueTransport.Stopped);
        Assert.DoesNotContain(_core.On(2), r => r.Command is "source.stage" or "zone.play");

        Ok(await _engine.PlayAsync(Zone, null, Ct));
        Assert.Equal(("source.stage", "A", 12_000_000_000UL), (_core.All[^2].Command, Staged()[^1].Title,
            JsonNode.Parse(_core.All[^2].Params)!["startOffsetNs"]!.GetValue<ulong>()));
    }

    // Phase 26: a zone's AutoFill is kept in playback.json and comes back.
    [Fact]
    public async Task A_restart_of_this_server_restores_each_zones_autofill()
    {
        await StartAsync("A");
        var path = Path.Combine(_dir.Path, "playback.json");
        var autoFill = new AutoFill(_engine, new NoChoice(), NullLogger<AutoFill>.Instance);
        var store = new PlaybackStore(new PlaybackStoreOptions(path, false), _engine, _link, NullLogger<PlaybackStore>.Instance,
            autoFill);
        await store.StartAsync(Ct);
        var set = autoFill.Set(Zone, new AutoFillPatch(Enabled: true, Count: 7, Source: AutoFillSource.Recommendations,
            Request: " calmer songs ", AvoidRecent: 1000));
        Assert.True(set.IsSuccess);
        await store.StopAsync(Ct);

        var restored = new AutoFill(_engine, new NoChoice(), NullLogger<AutoFill>.Instance);
        var again = new PlaybackStore(new PlaybackStoreOptions(path, false), _engine, _link, NullLogger<PlaybackStore>.Instance,
            restored);
        await again.StartAsync(Ct);
        var view = restored.View(Zone);
        Assert.Equal((true, 3, 7, AutoFillSource.Recommendations, "calmer songs", false, 1000),
            (view.Enabled, view.Threshold, view.Count, view.Source, view.Request, view.Fallback, view.AvoidRecent));
        Assert.False(restored.View("kitchen").Enabled);
        await again.StopAsync(Ct);
    }

    private sealed class NoChoice : IAutoFillChooser
    {
        public Task<AutoFillChoice> ChooseAsync(string zone, IReadOnlyList<Track> queue, AutoFillSettings settings,
            CancellationToken cancellationToken) =>
            Task.FromResult(new AutoFillChoice([], settings.Source, Reason: "none"));
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task A_restart_of_this_server_restores_its_queues_and_desired_state_stopped_unless_auto_resume(
        bool autoResume)
    {
        await PlayingAsync("A", "B");
        Ok(await _engine.SetRepeatAsync(Zone, RepeatMode.All, Ct));
        Ok(await _engine.SetVolumeAsync("roomA", 0.5, null, Ct));
        _time.Advance(TimeSpan.FromSeconds(42));
        var before = Queue();
        var path = Path.Combine(_dir.Path, "playback.json");
        var store = new PlaybackStore(new PlaybackStoreOptions(path, autoResume), _engine, _link,
            NullLogger<PlaybackStore>.Instance);
        await store.StartAsync(Ct);
        await store.StopAsync(Ct);
        await _link.StopAsync(Ct);

        // This server comes back, and so did the core meanwhile: it is empty.
        _core.StatusBody = FakeCore.EmptyStatus;
        var synchronised = new Synchronised();
        _restartedLink = new CoreLink(new CoreLinkOptions("127.0.0.1", _core.Port), [synchronised],
            NullLogger<CoreLink>.Instance);
        var engine = new QueueEngine(_restartedLink, _providers, _media, NullLogger<QueueEngine>.Instance, _time);
        var restored = new PlaybackStore(new PlaybackStoreOptions(path, autoResume), engine, _restartedLink,
            NullLogger<PlaybackStore>.Instance);
        await restored.StartAsync(Ct);
        var queue = engine.GetQueue(Zone).Value;
        Assert.Equal(before.Items, queue.Items);
        Assert.Equal((before.Current, RepeatMode.All, QueueTransport.Stopped), (queue.Current, queue.Repeat, queue.Transport));
        Assert.Null(queue.NowPlaying);

        await _restartedLink.StartAsync(Ct);
        await synchronised.Next.Task.WaitAsync(Patience, Ct);
        var first = _core.On(2).Take(5).Select(r => (r.Command, r.Params));
        Assert.Equal(
            [
                ("events.subscribe", "{}"),
                ("status", "{}"),
                ("zone.create", """{"zone":"living"}"""),
                ("zone.setClients", """{"zone":"living","clients":["roomA"]}"""),
                ("client.setVolume", """{"client":"roomA","value":0.5}"""),
            ],
            first);

        if (!autoResume)
        {
            Assert.Equal(5, _core.On(2).Count);
            Ok(await engine.PlayAsync(Zone, null, Ct));
        }
        await Until(() => _core.On(2).Any(r => r.Command == "zone.play"));
        var stage = JsonNode.Parse(_core.On(2).Single(r => r.Command == "source.stage").Params)!;
        Assert.Equal(("A", 42_000_000_000UL), (stage["title"]!.GetValue<string>(), stage["startOffsetNs"]!.GetValue<ulong>()));

        // Item ids go on after the restored ones.
        var added = Ok(await engine.AddAsync(Zone, [before.Items[0].Track!], null, Ct));
        Assert.True(added.Items[^1].Id.Value > before.Items.Max(i => i.Id.Value));
        await restored.StopAsync(Ct);
    }

    [Fact]
    public async Task A_failed_source_is_marked_with_its_reason_and_the_next_one_staged()
    {
        var (_, ids) = await PlayingAsync("A", "B", "C");

        await EmitAsync($$"""{"event":"zone.sourceFailed","zone":"living","sourceId":"{{ids[1]}}","reason":"HTTP 404"}""");
        await Until(() => Staged().Count == 3);

        Assert.Equal("C", Staged()[2].Title);
        Assert.Equal([null, "HTTP 404", null], Queue().Items.Select(i => i.Failure));
    }

    [Fact]
    public async Task A_station_is_never_staged_behind_and_re_tunes_once_per_failure_before_the_queue_moves_on()
    {
        var tracks = await StartAsync("B");
        var station = _fake.AddStation("Radio", [1, 2, 3]);
        Ok(await _engine.AddAsync(Zone, station, null, Ct));
        Ok(await _engine.AddAsync(Zone, tracks, null, Ct));
        Ok(await _engine.PlayAsync(Zone, null, Ct));
        var first = Staged()[0].SourceId;

        // Opened and heard: nothing is staged behind it.
        await EmitAsync(
            $$"""{"event":"zone.sourceNeeded","zone":"living","afterSourceId":"{{first}}"}""",
            $$"""{"event":"zone.sourceStarted","zone":"living","sourceId":"{{first}}","timelineNs":0,"serverTimeNs":1,"startOffsetNs":0}""");
        await Until(() => Queue().NowPlaying is not null);
        Assert.Single(Staged());

        // The upstream drops: the station is staged again, once, and heard.
        await EmitAsync($$"""{"event":"zone.sourceFailed","zone":"living","sourceId":"{{first}}","reason":"I/O error"}""");
        await Until(() => Staged().Count == 2);
        var second = Staged()[1];
        Assert.Equal(("Radio", "fake:s0"), (second.Title, second.TrackId));
        Assert.NotEqual(first, second.SourceId);
        await EmitAsync(
            $$"""{"event":"zone.sourceNeeded","zone":"living","afterSourceId":"{{second.SourceId}}"}""",
            $$"""{"event":"zone.sourceStarted","zone":"living","sourceId":"{{second.SourceId}}","timelineNs":5,"serverTimeNs":6,"startOffsetNs":0}""");
        await Until(() => Queue().NowPlaying?.SourceId == second.SourceId);
        Assert.Equal(2, Staged().Count);

        // Heard, so a later drop re-tunes again. That one fails before it is
        // heard: the station is marked, and the queue moves on to B.
        await EmitAsync($$"""{"event":"zone.sourceFailed","zone":"living","sourceId":"{{second.SourceId}}","reason":"I/O error"}""");
        await Until(() => Staged().Count == 3);
        var third = Staged()[2].SourceId;
        await EmitAsync($$"""{"event":"zone.sourceFailed","zone":"living","sourceId":"{{third}}","reason":"HTTP 503"}""");
        await Until(() => Staged().Count == 4);

        Assert.Equal(["Radio", "Radio", "Radio", "B"], Staged().Select(s => s.Title));
        Assert.Equal(["HTTP 503", null], Queue().Items.Select(i => i.Failure));
    }

    [Fact]
    public async Task A_track_that_does_not_resolve_is_marked_and_passed()
    {
        var tracks = await StartAsync("A");
        var ghost = new Track(FakeProvider.ProviderId, "nowhere", "Ghost");
        var gone = new Track("tidal", "123", "Unregistered");
        var after = _fake.AddTrack(_dir.Write("Z.flac", "zzz"), "Z");
        Ok(await _engine.AddAsync(Zone, [tracks[0], ghost, gone, after], null, Ct));
        Ok(await _engine.PlayAsync(Zone, null, Ct));

        await EmitAsync($$"""{"event":"zone.sourceNeeded","zone":"living","afterSourceId":"{{Staged()[0].SourceId}}"}""");
        await Until(() => Staged().Count == 2);

        Assert.Equal("Z", Staged()[1].Title);
        var failures = Queue().Items.Select(i => i.Failure).ToList();
        Assert.Equal("NotFound: no track nowhere", failures[1]);
        Assert.Equal("NotFound: no provider tidal", failures[2]);
    }

    [Fact]
    public async Task Zone_ended_with_nothing_after_plays_the_queue_out()
    {
        var tracks = await StartAsync("A");
        Ok(await _engine.AddAsync(Zone, tracks, null, Ct));
        Ok(await _engine.PlayAsync(Zone, null, Ct));
        var a = Staged()[0].SourceId;

        await EmitAsync(
            $$"""{"event":"zone.sourceNeeded","zone":"living","afterSourceId":"{{a}}"}""",
            $$"""{"event":"zone.sourceStarted","zone":"living","sourceId":"{{a}}","timelineNs":0,"serverTimeNs":1,"startOffsetNs":0}""",
            $$"""{"event":"zone.ended","zone":"living","afterSourceId":"{{a}}"}""");
        await Until(() => Queue().Current is null);

        Assert.Single(Staged());
        Assert.Null(Queue().NowPlaying);
        Assert.Empty(_media.Live);
    }

    [Fact]
    public async Task Shuffle_plays_every_item_once_per_cycle_and_previous_walks_back()
    {
        var (_, ids) = await PlayingAsync("A", "B", "C", "D", "E");
        Ok(await _engine.SetShuffleAsync(Zone, true, Ct));
        // Turning shuffle on restaged: B's source was taken back.
        var heard = new List<string> { "A" };
        var last = ids[0];
        for (var i = 0; i < 4; i++)
        {
            var next = Staged()[^1];
            Assert.NotEqual(last, next.SourceId);
            heard.Add(next.Title);
            await EmitAsync(
                $$"""{"event":"zone.sourceNeeded","zone":"living","afterSourceId":"{{next.SourceId}}"}""",
                $$"""{"event":"zone.sourceStarted","zone":"living","sourceId":"{{next.SourceId}}","timelineNs":{{i}},"serverTimeNs":{{i}},"startOffsetNs":0}""");
            await Until(() => Queue().NowPlaying?.SourceId == next.SourceId);
            last = next.SourceId;
        }
        Assert.Equal(["A", "B", "C", "D", "E"], heard.Order());
        // Repeat is off: nothing is staged after the fifth.
        Assert.Equal(last, Staged()[^1].SourceId);

        Ok(await _engine.PreviousAsync(Zone, Ct));
        Assert.Equal(heard[3], Staged()[^1].Title);
    }

    [Fact]
    public async Task Pause_resume_stop_and_what_cannot_be_done_now()
    {
        var (tracks, _) = await PlayingAsync("A", "B");
        var queue = Queue();
        var before = _core.All.Count;

        Assert.Equal(QueueTransport.Paused, Ok(await _engine.PauseAsync(Zone, Ct)).Transport);
        Ok(await _engine.PlayAsync(Zone, null, Ct));
        var stopped = Ok(await _engine.StopAsync(Zone, Ct));

        Assert.Equal(["zone.pause", "zone.play", "zone.stop", "source.clearStaged"], Sent(before).Select(c => c.Command));
        Assert.Equal(queue.Items[0].Id, stopped.Current);
        Assert.Empty(_media.Live);

        var station = _fake.AddStation("Radio", [1, 2, 3]);
        var withRadio = Ok(await _engine.AddAsync(Zone, station, 0, Ct));
        Assert.Equal(QueueError.Unavailable,
            (await _engine.SeekAsync(Zone, withRadio.Items[0].Id, TimeSpan.FromSeconds(1), Ct)).Failure.Code);
        Assert.Equal(QueueError.InvalidArgument, (await _engine.AddAsync(Zone, tracks, 9, Ct)).Failure.Code);
        Assert.Equal(QueueError.NotFound, (await _engine.PlayAsync("attic", null, Ct)).Failure.Code);
        Assert.Equal(QueueError.NotFound, (await _engine.PlayAsync(Zone, new QueueItemId(999), Ct)).Failure.Code);

        _core.Refuse("zone.pause", "unavailable", "zone living's timeline is stopped");
        var refused = await _engine.PauseAsync(Zone, Ct);
        Assert.Equal(QueueError.Unavailable, refused.Failure.Code);
        Assert.Equal("zone.pause: Unavailable: zone living's timeline is stopped", refused.Failure.Detail);
    }

    [Fact]
    public async Task Replacing_the_queue_stops_the_zone_clears_what_is_staged_and_the_next_play_starts_the_new_first_item()
    {
        await PlayingAsync("A", "B", "C");
        Ok(await _engine.SetRepeatAsync(Zone, RepeatMode.All, Ct));
        var before = _core.All.Count;
        var fresh = _fake.AddTrack(_dir.Write("X.flac", "the bytes of X"), "X");

        var replaced = Ok(await _engine.ReplaceAsync(Zone, [fresh], Ct));

        Assert.Equal([("zone.stop", """{"zone":"living"}"""), ("source.clearStaged", """{"zone":"living"}""")], Sent(before));
        Assert.Equal(["X"], replaced.Items.Select(i => i.Title));
        Assert.Equal((null, RepeatMode.All, (NowPlaying?)null), (replaced.Current, replaced.Repeat, replaced.NowPlaying));
        Assert.Empty(_media.Live);

        var played = Ok(await _engine.PlayAsync(Zone, null, Ct));
        Assert.Equal("X", Staged()[^1].Title);
        Assert.Equal(replaced.Items[0].Id, played.Current);
    }

    [Fact]
    public async Task Removing_the_playing_item_moves_on_and_deleting_the_zone_forgets_the_queue()
    {
        await PlayingAsync("A", "B", "C");
        var queue = Queue();

        var removed = Ok(await _engine.RemoveAsync(Zone, queue.Items[0].Id, Ct));

        Assert.Equal(["B", "C"], removed.Items.Select(i => i.Title));
        Assert.Equal(queue.Items[1].Id, removed.Current);
        Assert.Equal("B", Staged()[^1].Title);

        Assert.True((await _engine.DeleteAsync(Zone, Ct)).Value);
        Assert.Equal("zone.delete", _core.All[^1].Command);
        Assert.Equal(QueueError.NotFound, _engine.GetQueue(Zone).Failure.Code);
        Assert.Empty(_engine.Queues);
        Assert.Empty(_media.Live);
        Assert.DoesNotContain(Zone, _link.Desired.Diff(CoreSnapshot.Empty).OfType<ZoneCreate>().Select(z => z.Zone));
    }

    [Fact]
    public async Task Volume_is_per_room()
    {
        await StartAsync();

        Assert.True(Ok(await _engine.SetVolumeAsync("roomA", 0.25, null, Ct)));
        Assert.Equal(("client.setVolume", """{"client":"roomA","value":0.25}"""), Sent()[^1]);
        Assert.Equal(QueueError.InvalidArgument, (await _engine.SetVolumeAsync("roomA", null, null, Ct)).Failure.Code);
        Assert.Equal(QueueError.InvalidArgument, (await _engine.SetVolumeAsync("roomA", 1.5, null, Ct)).Failure.Code);
    }

    // --- a room's buttons and encoders (MATP section 57) ---------------------

    [Fact]
    public async Task A_knob_steps_the_room_s_desired_volume_and_clamps()
    {
        await StartAsync();
        var inputs = new InputActions(_link, _engine, NullLogger<InputActions>.Instance);
        Ok(await _engine.SetVolumeAsync("roomA", 0.5, null, Ct));

        await inputs.RunAsync(new RoomInput("roomA", "knob", "clockwise", InputAction.VolumeUp, 3, Step: 0.02), Ct);
        Assert.Equal(("client.setVolume", """{"client":"roomA","value":0.56}"""), Sent()[^1]);

        // From the desired volume, so a second turn builds on the first.
        await inputs.RunAsync(new RoomInput("roomA", "knob", "counterClockwise", InputAction.VolumeDown, 100,
            Step: 0.02), Ct);
        Assert.Equal(("client.setVolume", """{"client":"roomA","value":0}"""), Sent()[^1]);

        // Already there: nothing to send.
        var count = Sent().Count;
        await inputs.RunAsync(new RoomInput("roomA", "knob", "counterClockwise", InputAction.VolumeDown, 1,
            Step: 0.02), Ct);
        Assert.Equal(count, Sent().Count);

        await inputs.RunAsync(new RoomInput("roomA", "mute", "click", InputAction.ToggleMute, 1), Ct);
        Assert.Equal(("client.setVolume", """{"client":"roomA","muted":true}"""), Sent()[^1]);
    }

    [Fact]
    public async Task A_button_runs_the_zone_s_transport_and_a_zone_scope_changes_the_zone()
    {
        await PlayingAsync("A", "B");
        var inputs = new InputActions(_link, _engine, NullLogger<InputActions>.Instance);
        var changed = new List<string>();
        inputs.ZoneLevelChanged += (_, zone) => changed.Add(zone);

        // The room's zone is found from the desired membership.
        await inputs.RunAsync(new RoomInput("roomA", "play", "click", InputAction.TogglePause, 1), Ct);
        Assert.Equal(("zone.pause", """{"zone":"living"}"""), Sent()[^1]);

        await inputs.RunAsync(new RoomInput("roomA", "knob", "clockwise", InputAction.SetVolume, 1, Value: 0.4,
            Scope: VolumeScope.Zone, Zone: Zone), Ct);
        Assert.Equal(0.4, _link.Desired.ZoneLevels[Zone].Volume);
        Assert.Equal(("client.setVolume", """{"client":"roomA","value":0.4}"""), Sent()[^1]);
        Assert.Equal([Zone], changed);

        // A room in no zone has no transport to run.
        var count = Sent().Count;
        await inputs.RunAsync(new RoomInput("roomB", "play", "click", InputAction.Next, 1), Ct);
        Assert.Equal(count, Sent().Count);
    }

    [Fact]
    public async Task A_room_input_event_from_the_core_is_run()
    {
        await StartAsync();
        var inputs = new InputActions(_link, _engine, NullLogger<InputActions>.Instance);
        await inputs.StartAsync(Ct);
        try
        {
            await EmitAsync(
                """{"event":"room.input","client":"roomA","inputId":"knob","gesture":"clockwise","action":"volumeDown","step":0.25,"scope":"room","count":2}""");
            await Until(() => Sent().Any(s => s.Command == "client.setVolume"));
            Assert.Equal(("client.setVolume", """{"client":"roomA","value":0.5}"""), Sent()[^1]);
        }
        finally
        {
            await inputs.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task A_user_action_is_only_announced()
    {
        await StartAsync();
        var seen = new Captured();
        _link.Subscribe(seen);
        await EmitAsync(
            """{"event":"room.input","client":"roomA","inputId":"play","gesture":"longPress","action":"action7","count":1}""");
        await Until(() => seen.Input is not null);
        Assert.Equal(InputAction.Action7, seen.Input!.Action);

        var inputs = new InputActions(_link, _engine, NullLogger<InputActions>.Instance);
        var count = Sent().Count;
        await inputs.RunAsync(seen.Input, Ct);
        Assert.Equal(count, Sent().Count);
    }

    private sealed class Captured : ICoreEventSubscriber
    {
        public volatile RoomInput? Input;

        public ValueTask HandleAsync(CoreEvent coreEvent, CancellationToken cancellationToken)
        {
            if (coreEvent is RoomInput input)
                Input = input;
            return ValueTask.CompletedTask;
        }
    }

    [Fact]
    public async Task Input_bindings_are_sent_in_the_core_s_spelling()
    {
        await StartAsync();
        var set = await _link.SendAsync(new ClientSetInputBindings("roomA", "play",
            new Dictionary<string, CoreInputBinding>
            {
                ["doubleClick"] = new(InputAction.Next),
                ["longPress"] = new(InputAction.VolumeDown, Step: 0.1, Scope: VolumeScope.Zone),
            }), Ct);
        Assert.True(set.IsSuccess);
        Assert.Equal(("client.setInputBindings",
            """{"client":"roomA","inputId":"play","bindings":{"doubleClick":{"action":"next"},"longPress":{"action":"volumeDown","step":0.1,"scope":"zone"}}}"""),
            Sent()[^1]);
    }

    // --- helpers ------------------------------------------------------------

    private QueueState Queue() => _engine.GetQueue(Zone).Value;

    private static T Ok<T>(Result<T, QueueError> result)
    {
        Assert.True(result.IsSuccess, result.ToString());
        return result.Value;
    }

    private List<(string Command, string Params)> Sent(int from = 0) =>
        [.. _core.All.Skip(from).Where(r => r.Command is not ("events.subscribe" or "status"))
            .Select(r => (r.Command, r.Params))];

    private sealed record Stage(string SourceId, string MediaUri, string? TrackId, string Title);

    // Every source.stage the engine sent, in order.
    private List<Stage> Staged() =>
        [.. _core.All.Where(r => r.Command == "source.stage").Select(r => JsonNode.Parse(r.Params)!).Select(p =>
            new Stage(p["sourceId"]!.GetValue<string>(), p["mediaUri"]!.GetValue<string>(),
                p["trackId"]?.GetValue<string>(), p["title"]!.GetValue<string>()))];

    private static string Status(string source, string[] staged) =>
        $$"""
        {"server":{"version":"fake","uptimeMs":1,"zones":1,"clients":1,"connected":0,"members":0},
         "clients":[{"clientId":"roomA","connected":false,
           "settings":{"audioControl":"bitPerfectFallback","driftCorrection":false,"audioControlIsDefault":true,"driftCorrectionIsDefault":false},
           "member":false,"driftCorrection":{"requested":false} }],
         "zones":[{"zone":"living","state":"playing","transport":"playing","timelineNs":2000,
           "source":{"sourceId":"{{source}}","ended":false,"timelineStartNs":1000},
           "staged":[{{string.Join(",", staged.Select(s => $$"""{"sourceId":"{{s}}","mediaUri":"http://127.0.0.1:4714/media/x"}"""))}}],
           "failedSources":[],"members":[],"clients":["roomA"],"startsSent":1,"rejoins":0}]}
        """;

    private Task EmitAsync(params string[] lines) => _core.EmitAsync(lines);

    private static async Task Until(Func<bool> condition)
    {
        using var timeout = new CancellationTokenSource(Patience);
        while (!condition())
            await Task.Delay(5, timeout.Token);
    }

    // Tells the test when CoreLink is first in step with the core.
    internal sealed class Synchronised : ICoreEventSubscriber
    {
        public TaskCompletionSource Next { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);

        public ValueTask HandleAsync(CoreEvent coreEvent, CancellationToken cancellationToken)
        {
            if (coreEvent is CoreSynchronised)
                Next.TrySetResult();
            return ValueTask.CompletedTask;
        }
    }

    // The playhead's clock, moved by the test alone; its timers fire, once,
    // as it passes their time (the chapter timer, Task.Delay on it).
    internal sealed class ManualTime : TimeProvider
    {
        private readonly Lock _lock = new();
        private readonly List<Timer> _timers = [];
        private long _ticks;

        public override long TimestampFrequency => TimeSpan.TicksPerSecond;

        public override long GetTimestamp() => Interlocked.Read(ref _ticks);

        public void Advance(TimeSpan by)
        {
            var now = Interlocked.Add(ref _ticks, by.Ticks);
            List<Timer> due;
            lock (_lock)
            {
                due = [.. _timers.Where(t => t.Due <= now)];
                _timers.RemoveAll(due.Contains);
            }
            foreach (var timer in due)
                timer.Fire();
        }

        public override ITimer CreateTimer(TimerCallback callback, object? state, TimeSpan dueTime, TimeSpan period)
        {
            var timer = new Timer(this, callback, state);
            timer.Change(dueTime, period);
            return timer;
        }

        private sealed class Timer(ManualTime time, TimerCallback callback, object? state) : ITimer
        {
            public long Due { get; private set; }

            public bool Change(TimeSpan dueTime, TimeSpan period)
            {
                lock (time._lock)
                {
                    time._timers.Remove(this);
                    if (dueTime == Timeout.InfiniteTimeSpan)
                        return true;
                    Due = time.GetTimestamp() + dueTime.Ticks;
                    time._timers.Add(this);
                }
                return true;
            }

            public void Fire() => callback(state);

            public void Dispose() => Change(Timeout.InfiniteTimeSpan, Timeout.InfiniteTimeSpan);

            public ValueTask DisposeAsync()
            {
                Dispose();
                return ValueTask.CompletedTask;
            }
        }
    }

    // What the media endpoint would serve: the URLs published and not retired.
    internal sealed class FakeMedia : IMediaPublisher
    {
        private readonly ConcurrentDictionary<Uri, ResolvedStream> _live = new();
        private int _next;

        public IReadOnlyList<Uri> Live => [.. _live.Keys.OrderBy(u => u.AbsoluteUri, StringComparer.Ordinal)];

        public Uri Publish(ResolvedStream stream)
        {
            var uri = new Uri($"http://127.0.0.1:4714/media/h{Interlocked.Increment(ref _next)}");
            _live[uri] = stream;
            return uri;
        }

        public ResolvedStream this[Uri uri] => _live[uri];

        public void Retire(Uri uri) => _live.TryRemove(uri, out _);
    }
}
