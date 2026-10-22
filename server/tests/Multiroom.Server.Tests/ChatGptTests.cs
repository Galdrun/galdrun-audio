using System.Text.Json;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.WebUtilities;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Ai;
using Multiroom.Server.Ai.Providers;
using Multiroom.Server.Ai.Providers.OpenAi;

namespace Multiroom.Server.Tests;

// Sign in with ChatGPT and the plan's Responses API (phase 28), against
// FakeOAuth taught the dynamic client, the ID token and earliest_refresh_at,
// and FakeOpenAiResponses' recorded streams: the first sign-in registers a
// client and later ones reuse it with the same host id; the stream maps both
// ways field by field; a cut stream, the plan's limit and an ineligible
// account are named; the token is renewed from earliest_refresh_at, once for
// three calls; no token, code, client id or host id leaks into an answer.
public sealed class ChatGptTests : IAsyncLifetime
{
    private static readonly HttpClient Http = new(new SocketsHttpHandler { UseProxy = false });
    private static readonly Uri AppPage = new("http://127.0.0.1:4730/ai-login.html");
    private const string Id = OpenAiProvider.ProviderId;

    private static readonly AiTool SearchTool = new("search", "Searches the music.",
        JsonSerializer.SerializeToElement(new
        {
            type = "object",
            properties = new { query = new { type = "string" } },
            required = new[] { "query" },
        }));

    private readonly TempDirectory _dir = new();
    private readonly Clock _clock = new();
    private FakeOAuth _oauth = null!;
    private FakeOpenAiResponses _vendor = null!;
    private AiCredentialStore _credentials = null!;
    private AiProviderRegistry _registry = null!;

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    private string HostFile => Path.Combine(_dir.Path, ChatGptHost.FileName);

    public async ValueTask InitializeAsync()
    {
        _oauth = await FakeOAuth.StartAsync();
        _oauth.Now = _clock.GetUtcNow;
        _vendor = await FakeOpenAiResponses.StartAsync();
        _vendor.Accepts = token => token == _oauth.CurrentAccess;
        _registry = await StartRegistryAsync();
    }

    public async ValueTask DisposeAsync()
    {
        _registry.Dispose();
        await _vendor.DisposeAsync();
        await _oauth.DisposeAsync();
        _dir.Dispose();
    }

    // The registry as the host makes it, over this test's files: a restart
    // is a new one.
    private async Task<AiProviderRegistry> StartRegistryAsync()
    {
        AiProviderConfig[] config = [new(Id, null, _vendor.BaseUri)];
        _credentials = new AiCredentialStore(Path.Combine(_dir.Path, "credentials.json"), config);
        var registry = new AiProviderRegistry(
            [new AiProviderRegistration(Id, c => new OpenAiProvider(c), TimeSpan.FromSeconds(5))],
            config, _credentials, new AiSettingsStore(Path.Combine(_dir.Path, "settings.json")),
            NullLogger<AiProviderRegistry>.Instance,
            [new AiLoginRegistration(Id, h => new ChatGptLogin(h, new ChatGptHost(HostFile), _oauth.ChatGpt, _clock))],
            _clock);
        await registry.StartAsync(Ct);
        return registry;
    }

    private static AiRequest Hello => new("gpt-plan", [new AiMessage(AiRole.User, "hi")]);

    private async Task<(AiLoginStart Start, string ClientId)> SignInAsync(bool sendClient = true)
    {
        var start = _registry.StartLogin(Id, AppPage).Value;
        var (code, client) = _oauth.ConsentChatGpt(start.AuthorizeUrl);
        var view = await _registry.CompleteLoginAsync(start.State, code, null, sendClient ? client : null, Ct);
        Assert.True(view.IsSuccess, view.ToString());
        return (start, client);
    }

    [Fact]
    public async Task A_first_sign_in_registers_a_client_and_later_ones_reuse_it_with_the_same_host_id()
    {
        var before = Assert.Single(await _registry.ViewsAsync(Ct));
        Assert.Equal([AiAuthMethod.ApiKey, AiAuthMethod.OAuth], before.AuthMethods);
        Assert.Equal([AiCapability.Reasoning, AiCapability.WebSearch, AiCapability.Transcription], before.Capabilities);
        Assert.Equal((false, (bool?)null), (before.Configured, before.HasClient));
        Assert.Contains("POST /api/v1/ai/providers/openai/login", before.Detail, StringComparison.Ordinal);
        Assert.Equal(AiError.NotConfigured, (await _registry.CompleteAsync(Id, Hello, Ct)).Failure.Code);

        var (first, client) = await SignInAsync();
        var query = QueryHelpers.ParseQuery(first.AuthorizeUrl.Query);
        Assert.Equal(_oauth.ChatGpt.Authorize.AbsoluteUri, first.AuthorizeUrl.GetLeftPart(UriPartial.Path));
        Assert.Equal((ChatGptLogin.DynamicClient, "code", AppPage.AbsoluteUri, ChatGptLogin.Scopes, ChatGptLogin.Resource,
                first.State, "S256", "Galdrun"),
            (query["client_id"].ToString(), query["response_type"].ToString(), query["redirect_uri"].ToString(),
                query["scope"].ToString(), query["resource"].ToString(), query["state"].ToString(),
                query["code_challenge_method"].ToString(), query["agent_name_hint"].ToString()));
        Assert.Equal(43, query["nonce"].ToString().Length);
        var hostId = query["ext_agent_host_id"].ToString();
        Assert.StartsWith("urn:uuid:", hostId, StringComparison.Ordinal);
        Assert.True(Guid.TryParse(hostId["urn:uuid:".Length..], out _));
        Assert.Contains(hostId, await File.ReadAllTextAsync(HostFile, Ct), StringComparison.Ordinal);

        Assert.Equal(client, Assert.Single(_oauth.RegisteredClients));
        Assert.Equal(client, _credentials.Client(Id)!.ClientId);
        var token = _credentials.Token(Id)!;
        Assert.Equal((_clock.GetUtcNow().AddSeconds(3600), _clock.GetUtcNow().AddSeconds(_oauth.EarliestRefreshIn)),
            (token.ExpiresAt, token.RefreshAfter));
        var view = Assert.Single(await _registry.ViewsAsync(Ct));
        Assert.Equal((true, true, false, (bool?)null), (view.Configured, view.HasToken, view.HasKey, view.HasClient));
        Assert.True((await _registry.CompleteAsync(Id, Hello, Ct)).IsSuccess);

        // Logout revokes the refresh token and keeps the client.
        var refresh = token.RefreshToken!;
        var loggedOut = (await _registry.RemoveKeyAsync(Id, Ct)).Value;
        Assert.False(loggedOut.HasToken);
        Assert.True(_oauth.RevokedTokens.ContainsKey(refresh));
        Assert.Equal(client, _credentials.Client(Id)!.ClientId);

        // A restart, then a sign-in: the issued client and the same host id,
        // no new registration; the redirect may leave client_id out then.
        _registry.Dispose();
        _registry = await StartRegistryAsync();
        var (second, again) = await SignInAsync(sendClient: false);
        var secondQuery = QueryHelpers.ParseQuery(second.AuthorizeUrl.Query);
        Assert.Equal((client, hostId, false), (secondQuery["client_id"].ToString(),
            secondQuery["ext_agent_host_id"].ToString(), secondQuery.ContainsKey("agent_name_hint")));
        Assert.Equal(client, again);
        Assert.Single(_oauth.RegisteredClients);
        Assert.True((await _registry.CompleteAsync(Id, Hello, Ct)).IsSuccess);

        // Nothing the flow handed out is in an answer.
        var answers = JsonSerializer.Serialize(await _registry.ViewsAsync(Ct)) + JsonSerializer.Serialize(_registry.Usage);
        Assert.All(_oauth.Issued.Append(client).Append(hostId),
            secret => Assert.DoesNotContain(secret, answers, StringComparison.Ordinal));
    }

    [Fact]
    public async Task A_sign_in_is_refused_off_loopback_without_a_client_without_the_plan_or_with_another_nonce()
    {
        var remote = _registry.StartLogin(Id, new Uri("https://gui.example.com/ai-login.html"));
        Assert.Equal((AiError.InvalidArgument, "redirectUri: ChatGPT takes only a loopback http redirect (127.0.0.1 or localhost)"),
            (remote.Failure.Code, remote.Failure.Detail));
        Assert.True(_registry.StartLogin(Id, new Uri("http://localhost:4730/ai-login.html")).IsSuccess);

        async Task<Result<AiProviderView, AiError>> Attempt(Func<(string Code, string Client), string?> client)
        {
            var start = _registry.StartLogin(Id, AppPage).Value;
            var consent = _oauth.ConsentChatGpt(start.AuthorizeUrl);
            return await _registry.CompleteLoginAsync(start.State, consent.Code, null, client(consent), Ct);
        }

        var unregistered = await Attempt(_ => null);
        Assert.Equal(AiError.AuthFailed, unregistered.Failure.Code);
        Assert.Contains("did not complete the app's registration", unregistered.Failure.Detail, StringComparison.Ordinal);
        Assert.Equal(AiError.AuthFailed, (await Attempt(_ => ChatGptLogin.DynamicClient)).Failure.Code);

        _oauth.GrantPlan = false;
        var noPlan = await Attempt(c => c.Client);
        Assert.Equal(AiError.AuthFailed, noPlan.Failure.Code);
        Assert.Contains("did not grant use of the ChatGPT plan", noPlan.Failure.Detail, StringComparison.Ordinal);
        _oauth.GrantPlan = true;

        _oauth.WrongNonce = true;
        var replayed = await Attempt(c => c.Client);
        Assert.Equal((AiError.AuthFailed, "chatgpt sign-in: the ChatGPT identity could not be verified; sign in again"),
            (replayed.Failure.Code, replayed.Failure.Detail));
        _oauth.WrongNonce = false;
        Assert.Null(_credentials.Token(Id));

        var (_, client) = await SignInAsync();
        var other = await Attempt(_ => "app_someone_else");
        Assert.Equal(AiError.AuthFailed, other.Failure.Code);
        Assert.Equal(client, _credentials.Client(Id)!.ClientId);
    }

    [Fact]
    public async Task Dictation_sends_the_audio_as_the_desktop_app_with_the_tokens_account_and_hides_the_token()
    {
        var payload = Convert.ToBase64String(JsonSerializer.SerializeToUtf8Bytes(new Dictionary<string, object>
        {
            ["https://api.openai.com/auth"] = new { chatgpt_account_id = "acct-1", chatgpt_data_residency = "eu" },
        })).TrimEnd('=').Replace('+', '-').Replace('/', '_');
        var token = $"e30.{payload}.sig";
        var seen = new List<(string? Agent, string? Originator, string? Account, string? Residency, string? Name, string? Language)>();
        var status = 200;
        await using var fake = await StartDictationAsync(async c =>
        {
            var form = await c.Request.ReadFormAsync();
            seen.Add((c.Request.Headers.UserAgent, c.Request.Headers["originator"], c.Request.Headers["chatgpt-account-id"],
                c.Request.Headers["x-openai-internal-codex-residency"], form.Files["file"]?.FileName, form["language"]));
            c.Response.StatusCode = status;
            await c.Response.WriteAsync(status == 200 ? """{"text":" Mach lauter. "}""" : $"denied {token}");
        });
        var dictation = new ChatGptDictation(new AiProviderContext(Http, () => token), fake.Uri);
        var request = new AiTranscriptionRequest("", [1, 2, 3], "audio/wav", "de-DE");

        Assert.Equal("Mach lauter.", (await dictation.TranscribeAsync(request, Ct)).Value);
        Assert.Equal([(ChatGptDictation.UserAgent, ChatGptDictation.Originator, "acct-1", "eu", "audio.wav", "de")], seen);

        status = 403;
        var refused = await dictation.TranscribeAsync(request, Ct);
        Assert.Equal(AiError.AuthFailed, refused.Failure.Code);
        Assert.DoesNotContain(token, refused.Failure.Detail, StringComparison.Ordinal);
    }

    private static async Task<DictationFake> StartDictationAsync(RequestDelegate handle)
    {
        var (app, uri) = await FixtureHttp.StartAsync(handle);
        return new DictationFake(app, uri);
    }

    private sealed record DictationFake(WebApplication App, Uri Uri) : IAsyncDisposable
    {
        public ValueTask DisposeAsync() => App.DisposeAsync();
    }

    [Fact]
    public async Task A_turn_with_a_tool_call_and_its_result_maps_both_ways_from_the_stream()
    {
        var provider = new ChatGptPlan(new AiProviderContext(Http, () => "plan-token", _vendor.BaseUri));
        _vendor.Accepts = t => t == "plan-token";
        Assert.Equal([new AiModel("gpt-plan", "GPT Plan")], (await provider.ListModelsAsync(Ct)).Value);

        _vendor.Streams.Enqueue(FakeOpenAiResponses.Completed("""
            {"id":"resp_1","status":"completed","output":[
              {"type":"reasoning","id":"rs_1","summary":[]},
              {"type":"function_call","id":"fc_1","call_id":"call_1","name":"search","arguments":"{\"query\":\"miles\"}"}],
             "usage":{"input_tokens":12,"output_tokens":7,"output_tokens_details":{"reasoning_tokens":3}}}
            """));
        var first = new AiMessage(AiRole.User, "Play some Miles Davis.");
        var asked = await provider.CompleteAsync(new AiRequest("gpt-plan", [first], "You are Galdrun.", [SearchTool], 256), Ct);

        Assert.True(asked.IsSuccess, asked.ToString());
        var sent = Assert.Single(_vendor.Requests);
        Assert.Equal(("gpt-plan", "You are Galdrun.", false, true), (sent["model"]!.GetValue<string>(),
            sent["instructions"]!.GetValue<string>(), sent["store"]!.GetValue<bool>(), sent["stream"]!.GetValue<bool>()));
        Assert.Equal(("user", "Play some Miles Davis."),
            (sent["input"]![0]!["role"]!.GetValue<string>(), sent["input"]![0]!["content"]!.GetValue<string>()));
        var tool = sent["tools"]![0]!;
        Assert.Equal(("function", "search", "Searches the music.", "string"), (tool["type"]!.GetValue<string>(),
            tool["name"]!.GetValue<string>(), tool["description"]!.GetValue<string>(),
            tool["parameters"]!["properties"]!["query"]!["type"]!.GetValue<string>()));
        // What the plan's route documents, and what this server needs; no limit, no reasoning, no tool forced.
        Assert.Null(sent["max_output_tokens"]);
        Assert.Null(sent["reasoning"]);
        Assert.Null(sent["tool_choice"]);

        var answer = asked.Value;
        Assert.Equal((null, AiStopReason.ToolUse, new AiUsage(12, 7, 3)), (answer.Text, answer.StopReason, answer.Usage));
        var call = Assert.Single(answer.ToolCalls);
        Assert.Equal(("call_1", "search", "miles"), (call.Id, call.Name, call.Arguments.GetProperty("query").GetString()));

        _vendor.Streams.Enqueue(FakeOpenAiResponses.Completed("""
            {"id":"resp_2","status":"completed","output":[{"type":"message","role":"assistant",
              "content":[{"type":"output_text","text":"Playing "},{"type":"output_text","text":"Kind of Blue."}]}],
             "usage":{"input_tokens":30,"output_tokens":5}}
            """));
        var replied = await provider.CompleteAsync(new AiRequest("gpt-plan",
        [
            first,
            new AiMessage(AiRole.Assistant, "Looking.", answer.ToolCalls),
            new AiMessage(AiRole.Tool, """{"found":1}""", ToolCallId: "call_1"),
        ], "You are Galdrun.", [SearchTool], RequiredTool: "search"), Ct);

        Assert.True(replied.IsSuccess, replied.ToString());
        var input = _vendor.Requests.Last()["input"]!.AsArray();
        Assert.Equal(("assistant", "Looking."), (input[1]!["role"]!.GetValue<string>(), input[1]!["content"]!.GetValue<string>()));
        Assert.Equal(("function_call", "call_1", "search", "miles"), (input[2]!["type"]!.GetValue<string>(),
            input[2]!["call_id"]!.GetValue<string>(), input[2]!["name"]!.GetValue<string>(),
            JsonElement.Parse(input[2]!["arguments"]!.GetValue<string>()).GetProperty("query").GetString()));
        Assert.Equal(("function_call_output", "call_1", """{"found":1}"""), (input[3]!["type"]!.GetValue<string>(),
            input[3]!["call_id"]!.GetValue<string>(), input[3]!["output"]!.GetValue<string>()));
        var forced = _vendor.Requests.Last()["tool_choice"]!;
        Assert.Equal(("function", "search"), (forced["type"]!.GetValue<string>(), forced["name"]!.GetValue<string>()));
        Assert.Equal(("Playing Kind of Blue.", AiStopReason.EndTurn, new AiUsage(30, 5)),
            (replied.Value.Text, replied.Value.StopReason, replied.Value.Usage));
    }

    [Fact]
    public async Task Items_sent_only_as_output_item_done_are_the_answer_when_the_final_output_is_empty()
    {
        var provider = new ChatGptPlan(new AiProviderContext(Http, () => "plan-token", _vendor.BaseUri));
        _vendor.Accepts = t => t == "plan-token";
        // As the plan's route answers: the items one by one, the final output empty.
        _vendor.Streams.Enqueue(
        [
            """{"type":"response.created","response":{"id":"resp_1","status":"in_progress","output":[]}}""",
            """{"type":"response.output_item.done","output_index":0,"item":{"type":"reasoning","id":"rs_1","summary":[]}}""",
            """{"type":"response.output_item.done","output_index":1,"item":{"type":"function_call","id":"fc_1","call_id":"call_1","name":"search","arguments":"{\"query\":\"miles\"}"}}""",
            """{"type":"response.completed","response":{"id":"resp_1","status":"completed","output":[],"usage":{"input_tokens":3647,"output_tokens":70,"output_tokens_details":{"reasoning_tokens":53}}}}""",
        ]);

        var answer = (await provider.CompleteAsync(new AiRequest("gpt-plan", [new AiMessage(AiRole.User, "more like this")],
            Tools: [SearchTool]), Ct)).Value;

        var call = Assert.Single(answer.ToolCalls);
        Assert.Equal(("call_1", "search", AiStopReason.ToolUse, new AiUsage(3647, 70, 53)),
            (call.Id, call.Name, answer.StopReason, answer.Usage));
    }

    [Fact]
    public async Task A_cut_stream_the_plans_limit_and_an_ineligible_account_are_named()
    {
        var provider = new ChatGptPlan(new AiProviderContext(Http, () => "plan-token", _vendor.BaseUri));
        _vendor.Accepts = t => t == "plan-token";

        _vendor.Cut = true;
        var cut = await provider.CompleteAsync(Hello, Ct);
        Assert.Equal((AiError.InvalidAnswer, "openai: the stream ended before the response was complete"),
            (cut.Failure.Code, cut.Failure.Detail));
        _vendor.Cut = false;

        _vendor.Refusal = (429, """{"error":{"code":"subscription_sharing_usage_limit_exceeded","message":"Usage limit reached."}}""");
        var limit = await provider.CompleteAsync(Hello, Ct);
        Assert.Equal((AiError.RateLimited, "openai: the ChatGPT plan's usage limit is reached: Usage limit reached."),
            (limit.Failure.Code, limit.Failure.Detail));

        _vendor.Refusal = (403, """{"detail":{"code":"subscription_sharing_user_not_eligible"}}""");
        var ineligible = await provider.CompleteAsync(Hello, Ct);
        Assert.Equal(AiError.AuthFailed, ineligible.Failure.Code);
        Assert.Contains("Plus or Pro", ineligible.Failure.Detail, StringComparison.Ordinal);

        _vendor.Streams.Enqueue(
        [
            """{"type":"response.created","response":{"id":"resp_1","status":"in_progress","output":[]}}""",
            """{"type":"response.failed","response":{"id":"resp_1","status":"failed","error":{"code":"server_error","message":"overloaded"}}}""",
        ]);
        Assert.Equal(AiError.Unavailable, (await provider.CompleteAsync(Hello, Ct)).Failure.Code);

        // A refusal that names no code it knows is its HTTP status's.
        _vendor.Refusal = (400, """{"error":{"message":"Unsupported parameter: 'reasoning.effort'","code":"unsupported_value"}}""");
        var other = await provider.CompleteAsync(Hello, Ct);
        Assert.Equal((AiError.InvalidArgument, "openai: HTTP 400: Unsupported parameter: 'reasoning.effort'"),
            (other.Failure.Code, other.Failure.Detail));
    }

    [Fact]
    public async Task The_token_is_renewed_from_earliest_refresh_at_once_for_three_calls_and_a_refused_renewal_says_sign_in()
    {
        await SignInAsync();
        var first = _credentials.Token(Id)!;

        // Before earliest_refresh_at: the token stands.
        _clock.Advance(TimeSpan.FromSeconds(_oauth.EarliestRefreshIn - 1));
        Assert.True((await _registry.CompleteAsync(Id, Hello, Ct)).IsSuccess);
        Assert.Equal(0, _oauth.Refreshes);

        // From it on, well before the end: renewed once for three calls, the refresh token rotated.
        _clock.Advance(TimeSpan.FromSeconds(2));
        var answers = await Task.WhenAll(Enumerable.Range(0, 3).Select(_ => _registry.CompleteAsync(Id, Hello, Ct)));
        Assert.All(answers, a => Assert.True(a.IsSuccess, a.ToString()));
        Assert.Equal(1, _oauth.Refreshes);
        var renewed = _credentials.Token(Id)!;
        Assert.NotEqual(first.AccessToken, renewed.AccessToken);
        Assert.NotEqual(first.RefreshToken, renewed.RefreshToken);
        Assert.Equal(_clock.GetUtcNow().AddSeconds(_oauth.EarliestRefreshIn), renewed.RefreshAfter);

        // The owner disconnected the app in ChatGPT: the renewal is refused.
        _oauth.RevokedTokens[renewed.RefreshToken!] = true;
        _clock.Advance(TimeSpan.FromSeconds(_oauth.EarliestRefreshIn + 1));
        var refused = await _registry.CompleteAsync(Id, Hello, Ct);
        Assert.Equal(AiError.AuthFailed, refused.Failure.Code);
        Assert.Contains("sign in again", refused.Failure.Detail, StringComparison.Ordinal);
        var view = Assert.Single(await _registry.ViewsAsync(Ct));
        Assert.Equal(((bool?)false, true), (view.Authorised, view.HasToken));
        Assert.All(_oauth.Issued, secret => Assert.DoesNotContain(secret, refused.Failure.Detail, StringComparison.Ordinal));
    }

    [Fact]
    public async Task The_host_id_is_made_once_and_kept_owner_only()
    {
        var host = new ChatGptHost(HostFile);
        var id = host.Id().Value;
        Assert.Equal(id, new ChatGptHost(HostFile).Id().Value);
        await File.WriteAllTextAsync(HostFile, "not json", Ct);
        Assert.Equal(AiError.Unavailable, new ChatGptHost(HostFile).Id().Failure.Code);
        Assert.Equal("not json", await File.ReadAllTextAsync(HostFile, Ct));
    }

    // A UTC clock moved by the test alone.
    private sealed class Clock : TimeProvider
    {
        private DateTimeOffset _now = new(2026, 10, 1, 12, 0, 0, TimeSpan.Zero);

        public override DateTimeOffset GetUtcNow() => _now;

        public void Advance(TimeSpan by) => _now += by;
    }
}
