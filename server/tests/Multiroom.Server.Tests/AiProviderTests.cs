using System.Diagnostics;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Ai;
using Multiroom.Server.Ai.Providers;
using Multiroom.Server.Ai.Providers.Anthropic;
using Multiroom.Server.Ai.Providers.Gemini;
using Multiroom.Server.Ai.Providers.Groq;
using Multiroom.Server.Ai.Providers.Mistral;
using Multiroom.Server.Ai.Providers.Ollama;
using Multiroom.Server.Ai.Providers.OpenAi;
using Multiroom.Server.Ai.Providers.OpenRouter;

namespace Multiroom.Server.Tests;

// The AI providers (phase 21) against FakeOpenAi (Gemini's listing too) and
// FakeAnthropic: models listed, a turn with a tool call and one with its
// result, both directions of the mapping field by field; the vendor's
// refusals mapped; the registry's timeout; the keys kept owner-only.
public sealed class AiProviderTests : IDisposable
{
    private static readonly HttpClient Http = new(new SocketsHttpHandler { UseProxy = false });

    private static readonly AiTool SearchTool = new("search", "Searches the music.",
        JsonSerializer.SerializeToElement(new
        {
            type = "object",
            properties = new { query = new { type = "string" } },
            required = new[] { "query" },
        }));

    private readonly TempDirectory _dir = new();

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    public void Dispose() => _dir.Dispose();

    private static IAiProvider Vendor(string id, AiProviderContext context) => id switch
    {
        OpenAiProvider.ProviderId => new OpenAiProvider(context),
        OpenRouterProvider.ProviderId => new OpenRouterProvider(context),
        GroqProvider.ProviderId => new GroqProvider(context),
        MistralProvider.ProviderId => new MistralProvider(context),
        GeminiProvider.ProviderId => new GeminiProvider(context),
        OllamaProvider.ProviderId => new OllamaProvider(context),
        _ => throw new ArgumentOutOfRangeException(nameof(id)),
    };

    [Theory]
    [InlineData("openai")]
    [InlineData("openrouter")]
    [InlineData("groq")]
    [InlineData("mistral")]
    [InlineData("gemini")]
    [InlineData("ollama")]
    public async Task An_openai_compatible_vendor_lists_models_and_maps_a_tool_call_both_ways(string id)
    {
        await using var fake = await FakeOpenAi.StartAsync(id == "gemini" ? "models/" : "");
        fake.RequireKey = id != "ollama";
        var provider = Vendor(id, new AiProviderContext(Http, () => id == "ollama" ? null : FakeOpenAi.Key, fake.BaseUri));

        Assert.Equal(id, provider.Id);
        Assert.True((await provider.GetStatusAsync(Ct)).Configured);
        var models = (await provider.ListModelsAsync(Ct)).Value;
        Assert.Equal([new AiModel("model-a"), new AiModel("model-b", "Model B")], models);

        // Turn one: the model asks for a tool.
        fake.Answers.Enqueue("""
            {"id":"chatcmpl-1","object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":null,
              "tool_calls":[{"id":"call_1","type":"function","function":{"name":"search","arguments":"{\"query\":\"miles\"}"}}]},
              "finish_reason":"tool_calls"}],"usage":{"prompt_tokens":12,"completion_tokens":7,"total_tokens":19}}
            """);
        var first = new AiMessage(AiRole.User, "Play some Miles Davis.");
        var asked = await provider.CompleteAsync(new AiRequest("model-a", [first], "You are Galdrun.", [SearchTool], 256), Ct);

        Assert.True(asked.IsSuccess, asked.ToString());
        var sent = Assert.Single(fake.Requests);
        Assert.Equal("model-a", sent["model"]!.GetValue<string>());
        Assert.Equal(256, sent[id == "openai" ? "max_completion_tokens" : "max_tokens"]!.GetValue<int>());
        Assert.Null(sent[id == "openai" ? "max_tokens" : "max_completion_tokens"]);
        Assert.Equal(("system", "You are Galdrun."), Message(sent, 0));
        Assert.Equal(("user", "Play some Miles Davis."), Message(sent, 1));
        var tool = sent["tools"]![0]!;
        Assert.Equal("function", tool["type"]!.GetValue<string>());
        Assert.Equal(("search", "Searches the music."),
            (tool["function"]!["name"]!.GetValue<string>(), tool["function"]!["description"]!.GetValue<string>()));
        Assert.Equal("string", tool["function"]!["parameters"]!["properties"]!["query"]!["type"]!.GetValue<string>());

        var answer = asked.Value;
        Assert.Null(answer.Text);
        Assert.Equal(AiStopReason.ToolUse, answer.StopReason);
        Assert.Equal(new AiUsage(12, 7), answer.Usage);
        var call = Assert.Single(answer.ToolCalls);
        Assert.Equal(("call_1", "search", "miles"), (call.Id, call.Name, call.Arguments.GetProperty("query").GetString()));

        // Turn two: the tool's result goes back, and the model answers.
        fake.Answers.Enqueue("""
            {"id":"chatcmpl-2","object":"chat.completion","choices":[{"index":0,
              "message":{"role":"assistant","content":"Playing Kind of Blue."},"finish_reason":"stop"}],
             "usage":{"prompt_tokens":30,"completion_tokens":5,"total_tokens":35}}
            """);
        var replied = await provider.CompleteAsync(new AiRequest("model-a",
        [
            first,
            new AiMessage(AiRole.Assistant, ToolCalls: answer.ToolCalls),
            new AiMessage(AiRole.Tool, """{"found":1}""", ToolCallId: "call_1"),
        ], "You are Galdrun.", [SearchTool]), Ct);

        Assert.True(replied.IsSuccess, replied.ToString());
        var second = fake.Requests.Last();
        var assistant = second["messages"]![2]!;
        Assert.Equal("assistant", assistant["role"]!.GetValue<string>());
        var sentCall = assistant["tool_calls"]![0]!;
        Assert.Equal(("call_1", "function", "search"), (sentCall["id"]!.GetValue<string>(),
            sentCall["type"]!.GetValue<string>(), sentCall["function"]!["name"]!.GetValue<string>()));
        Assert.Equal("miles", JsonNode.Parse(sentCall["function"]!["arguments"]!.GetValue<string>())!["query"]!.GetValue<string>());
        var result = second["messages"]![3]!;
        Assert.Equal(("tool", "call_1", """{"found":1}"""), (result["role"]!.GetValue<string>(),
            result["tool_call_id"]!.GetValue<string>(), result["content"]!.GetValue<string>()));
        Assert.Equal(AiRequest.DefaultMaxTokens, second[id == "openai" ? "max_completion_tokens" : "max_tokens"]!.GetValue<int>());
        Assert.Equal(("Playing Kind of Blue.", AiStopReason.EndTurn, new AiUsage(30, 5)),
            (replied.Value.Text, replied.Value.StopReason, replied.Value.Usage));
        Assert.Empty(replied.Value.ToolCalls);
    }

    [Fact]
    public void A_model_named_for_speech_pictures_or_embeddings_takes_no_tools()
    {
        string[] ids = ["gemini-2.5-flash-preview-tts", "gemini-3-pro-image", "gemini-embedding-001", "gemini-2.5-pro", "gpt-6-sol"];
        Assert.Equal([false, false, false, null, null], ids.Select(id => AiModel.Named(id).Tools));
    }

    [Fact]
    public async Task Anthropic_lists_models_and_maps_a_tool_call_both_ways()
    {
        await using var fake = await FakeAnthropic.StartAsync();
        var provider = new AnthropicProvider(new AiProviderContext(Http, () => FakeAnthropic.Key, fake.BaseUri));

        Assert.Equal([new AiModel("claude-a", "Claude A"), new AiModel("claude-b", "Claude B")],
            (await provider.ListModelsAsync(Ct)).Value);

        fake.Answers.Enqueue("""
            {"id":"msg_1","type":"message","role":"assistant","model":"claude-a","content":[
              {"type":"text","text":"Let me look."},
              {"type":"tool_use","id":"toolu_1","name":"search","input":{"query":"miles"}}],
             "stop_reason":"tool_use","usage":{"input_tokens":20,"output_tokens":9}}
            """);
        var first = new AiMessage(AiRole.User, "Play some Miles Davis.");
        var asked = await provider.CompleteAsync(new AiRequest("claude-a", [first], "You are Galdrun.", [SearchTool], 256), Ct);

        Assert.True(asked.IsSuccess, asked.ToString());
        var sent = Assert.Single(fake.Requests);
        Assert.Equal(("claude-a", 256, "You are Galdrun."), (sent["model"]!.GetValue<string>(),
            sent["max_tokens"]!.GetValue<int>(), sent["system"]!.GetValue<string>()));
        var user = sent["messages"]![0]!;
        Assert.Equal(("user", "text", "Play some Miles Davis."), (user["role"]!.GetValue<string>(),
            user["content"]![0]!["type"]!.GetValue<string>(), user["content"]![0]!["text"]!.GetValue<string>()));
        var tool = sent["tools"]![0]!;
        Assert.Equal(("search", "Searches the music.", "string"), (tool["name"]!.GetValue<string>(),
            tool["description"]!.GetValue<string>(), tool["input_schema"]!["properties"]!["query"]!["type"]!.GetValue<string>()));

        var answer = asked.Value;
        Assert.Equal(("Let me look.", AiStopReason.ToolUse, new AiUsage(20, 9)), (answer.Text, answer.StopReason, answer.Usage));
        var call = Assert.Single(answer.ToolCalls);
        Assert.Equal(("toolu_1", "search", "miles"), (call.Id, call.Name, call.Arguments.GetProperty("query").GetString()));

        fake.Answers.Enqueue("""
            {"id":"msg_2","type":"message","role":"assistant","content":[{"type":"text","text":"Playing Kind of Blue."}],
             "stop_reason":"end_turn","usage":{"input_tokens":40,"output_tokens":6}}
            """);
        var replied = await provider.CompleteAsync(new AiRequest("claude-a",
        [
            first,
            new AiMessage(AiRole.Assistant, answer.Text, answer.ToolCalls),
            new AiMessage(AiRole.Tool, "no such track", ToolCallId: "toolu_1", IsError: true),
        ]), Ct);

        Assert.True(replied.IsSuccess, replied.ToString());
        var second = fake.Requests.Last();
        Assert.Null(second["system"]);
        Assert.Null(second["tools"]);
        var assistant = second["messages"]![1]!;
        Assert.Equal("assistant", assistant["role"]!.GetValue<string>());
        Assert.Equal(("text", "Let me look."), (assistant["content"]![0]!["type"]!.GetValue<string>(),
            assistant["content"]![0]!["text"]!.GetValue<string>()));
        var use = assistant["content"]![1]!;
        Assert.Equal(("tool_use", "toolu_1", "search", "miles"), (use["type"]!.GetValue<string>(),
            use["id"]!.GetValue<string>(), use["name"]!.GetValue<string>(), use["input"]!["query"]!.GetValue<string>()));
        var results = second["messages"]![2]!;
        Assert.Equal("user", results["role"]!.GetValue<string>());
        var block = Assert.Single(results["content"]!.AsArray())!;
        Assert.Equal(("tool_result", "toolu_1", "no such track", true), (block["type"]!.GetValue<string>(),
            block["tool_use_id"]!.GetValue<string>(), block["content"]!.GetValue<string>(), block["is_error"]!.GetValue<bool>()));
        Assert.Equal(("Playing Kind of Blue.", AiStopReason.EndTurn, new AiUsage(40, 6)),
            (replied.Value.Text, replied.Value.StopReason, replied.Value.Usage));
    }

    // Gemini 3 attaches a thought signature to each tool call and refuses the
    // next turn without it: extra_content goes back as it came.
    [Fact]
    public async Task A_tool_calls_extra_content_goes_back_with_it()
    {
        await using var fake = await FakeOpenAi.StartAsync("models/");
        fake.Answers.Enqueue("""
            {"choices":[{"index":0,"message":{"role":"assistant","content":null,"tool_calls":[
              {"id":"c1","type":"function","function":{"name":"search","arguments":"{\"query\":\"miles\"}"},
               "extra_content":{"google":{"thought_signature":"sig-123"}}}]},"finish_reason":"tool_calls"}],
             "usage":{"prompt_tokens":1,"completion_tokens":1}}
            """);
        var provider = new GeminiProvider(new AiProviderContext(Http, () => FakeOpenAi.Key, fake.BaseUri));
        var first = new AiMessage(AiRole.User, "x");
        var asked = (await provider.CompleteAsync(new AiRequest("m", [first], Tools: [SearchTool]), Ct)).Value;
        Assert.Equal("sig-123", asked.ToolCalls[0].Extra!.Value.GetProperty("google").GetProperty("thought_signature").GetString());

        await provider.CompleteAsync(new AiRequest("m",
            [first, new AiMessage(AiRole.Assistant, null, asked.ToolCalls), new AiMessage(AiRole.Tool, "[]", ToolCallId: "c1")],
            Tools: [SearchTool]), Ct);
        var sent = fake.Requests.Last()["messages"]![1]!["tool_calls"]![0]!;
        Assert.Equal("sig-123", sent["extra_content"]!["google"]!["thought_signature"]!.GetValue<string>());
        Assert.Null(fake.Requests.First()["messages"]![0]!["tool_calls"]);
    }

    // Google's OpenAI-compatible endpoint answers an error as a one-item array;
    // its message is kept, as an object's is.
    [Fact]
    public async Task An_error_answered_as_an_array_keeps_the_vendors_message()
    {
        await using var fake = await FakeOpenAi.StartAsync("models/");
        fake.Refusal = (404, """[{"error":{"code":404,"message":"models/x is not found for API version v1beta","status":"NOT_FOUND"}}]""");
        var answer = await new GeminiProvider(new AiProviderContext(Http, () => FakeOpenAi.Key, fake.BaseUri))
            .CompleteAsync(new AiRequest("x", [new AiMessage(AiRole.User, "hi")]), Ct);
        Assert.Equal((AiError.InvalidArgument, "gemini: HTTP 404: models/x is not found for API version v1beta"),
            (answer.Failure.Code, answer.Failure.Detail));
    }

    // Phase 24: a required tool is each wire format's forced tool choice, and
    // absent when none is asked.
    [Fact]
    public async Task A_required_tool_is_the_vendors_forced_tool_choice()
    {
        await using var openAi = await FakeOpenAi.StartAsync();
        await using var anthropic = await FakeAnthropic.StartAsync();
        var request = new AiRequest("m", [new AiMessage(AiRole.User, "x")], Tools: [SearchTool], RequiredTool: "search");

        await new OpenAiProvider(new AiProviderContext(Http, () => FakeOpenAi.Key, openAi.BaseUri)).CompleteAsync(request, Ct);
        var choice = Assert.Single(openAi.Requests)["tool_choice"]!;
        Assert.Equal(("function", "search"), (choice["type"]!.GetValue<string>(), choice["function"]!["name"]!.GetValue<string>()));

        var claude = new AnthropicProvider(new AiProviderContext(Http, () => FakeAnthropic.Key, anthropic.BaseUri));
        await claude.CompleteAsync(request, Ct);
        await claude.CompleteAsync(request with { RequiredTool = null }, Ct);
        var forced = anthropic.Requests.First()["tool_choice"]!;
        Assert.Equal(("tool", "search"), (forced["type"]!.GetValue<string>(), forced["name"]!.GetValue<string>()));
        Assert.Null(anthropic.Requests.Last()["tool_choice"]);
    }

    [Fact]
    public async Task A_wrong_key_is_AuthFailed_with_the_key_in_no_detail_and_a_429_is_RateLimited()
    {
        const string wrong = "sk-wrong-key-never-shown";
        await using var openAi = await FakeOpenAi.StartAsync();
        await using var anthropic = await FakeAnthropic.StartAsync();

        var refused = await new OpenAiProvider(new AiProviderContext(Http, () => wrong, openAi.BaseUri))
            .CompleteAsync(new AiRequest("model-a", [new AiMessage(AiRole.User, "hi")]), Ct);
        var refusedModels = await new AnthropicProvider(new AiProviderContext(Http, () => wrong, anthropic.BaseUri))
            .ListModelsAsync(Ct);
        var noKey = await new GroqProvider(new AiProviderContext(Http, () => null, openAi.BaseUri)).ListModelsAsync(Ct);
        openAi.RateLimited = true;
        var limited = await new OpenAiProvider(new AiProviderContext(Http, () => FakeOpenAi.Key, openAi.BaseUri))
            .ListModelsAsync(Ct);

        Assert.Equal(AiError.AuthFailed, refused.Failure.Code);
        // OpenAI echoes the key it was sent; the summary keeps the message, not the key.
        Assert.Contains("Incorrect API key provided", refused.Failure.Detail, StringComparison.Ordinal);
        Assert.DoesNotContain(wrong, refused.Failure.Detail, StringComparison.Ordinal);
        Assert.Equal(AiError.AuthFailed, refusedModels.Failure.Code);
        Assert.Equal(AiError.NotConfigured, noKey.Failure.Code);
        Assert.Equal(AiError.RateLimited, limited.Failure.Code);
        Assert.Empty(openAi.Requests);
    }

    [Fact]
    public async Task An_overloaded_vendor_is_asked_once_more_and_then_its_failure_stands()
    {
        await using var fake = await FakeOpenAi.StartAsync();
        AiProviderConfig[] config = [new(GroqProvider.ProviderId, FakeOpenAi.Key, fake.BaseUri)];
        using var registry = new AiProviderRegistry(
            [new AiProviderRegistration(GroqProvider.ProviderId, c => new GroqProvider(c), TimeSpan.FromSeconds(5))],
            config, new AiCredentialStore(Path.Combine(_dir.Path, "credentials.json"), config),
            new AiSettingsStore(Path.Combine(_dir.Path, "settings.json")), NullLogger<AiProviderRegistry>.Instance,
            retryDelay: TimeSpan.FromMilliseconds(50));
        await registry.StartAsync(Ct);
        var request = new AiRequest("model-a", [new AiMessage(AiRole.User, "hi")]);
        const string overloaded = """[{"error":{"code":503,"message":"This model is currently experiencing high demand.","status":"UNAVAILABLE"}}]""";

        fake.Refusal = (503, overloaded);
        fake.RefuseTimes = 1;
        Assert.Equal("ok", (await registry.CompleteAsync(GroqProvider.ProviderId, request, Ct)).Value.Text);

        fake.RefuseTimes = 2;
        var failed = await registry.CompleteAsync(GroqProvider.ProviderId, request, Ct);
        Assert.Equal(AiError.Unavailable, failed.Failure.Code);
        Assert.Contains("high demand", failed.Failure.Detail, StringComparison.Ordinal);

        // A refusal that is not a spike is not asked again.
        fake.Refusal = (400, """{"error":{"message":"bad request"}}""");
        fake.RefuseTimes = 1;
        Assert.Equal(AiError.InvalidArgument, (await registry.CompleteAsync(GroqProvider.ProviderId, request, Ct)).Failure.Code);

        var usage = Assert.Single(registry.Usage);
        Assert.Equal((3L, 2L, 2L), (usage.Calls, usage.Failures, usage.Retries));
        Assert.Single(fake.Requests);
    }

    [Fact]
    public async Task A_vendor_that_never_answers_costs_the_registrations_timeout_and_the_registry_remembers_what_it_said()
    {
        await using var fake = await FakeOpenAi.StartAsync();
        var credentials = new AiCredentialStore(Path.Combine(_dir.Path, "credentials.json"),
            [new AiProviderConfig(GroqProvider.ProviderId, FakeOpenAi.Key, fake.BaseUri)]);
        using var registry = new AiProviderRegistry(
            [new AiProviderRegistration(GroqProvider.ProviderId, c => new GroqProvider(c), TimeSpan.FromMilliseconds(300))],
            [new AiProviderConfig(GroqProvider.ProviderId, FakeOpenAi.Key, fake.BaseUri)], credentials,
            new AiSettingsStore(Path.Combine(_dir.Path, "settings.json")), NullLogger<AiProviderRegistry>.Instance);
        await registry.StartAsync(Ct);

        var listed = await registry.ListModelsAsync(GroqProvider.ProviderId, refresh: false, Ct);
        fake.Hang = true;
        var clock = Stopwatch.StartNew();
        var hung = await registry.CompleteAsync(GroqProvider.ProviderId,
            new AiRequest("model-a", [new AiMessage(AiRole.User, "hi")]), Ct);
        clock.Stop();
        // Cached: the vendor is not asked again.
        var cached = await registry.ListModelsAsync(GroqProvider.ProviderId, refresh: false, Ct);

        Assert.Equal(2, listed.Value.Count);
        Assert.Equal(AiError.Timeout, hung.Failure.Code);
        Assert.InRange(clock.Elapsed, TimeSpan.FromMilliseconds(250), TimeSpan.FromSeconds(5));
        Assert.Equal(2, cached.Value.Count);
        var view = Assert.Single(await registry.ViewsAsync(Ct));
        Assert.Equal((true, true, (bool?)true, (bool?)false, AiKeyOrigin.Config),
            (view.Enabled, view.Configured, view.Authorised, view.Reachable, view.Origin));
        var usage = Assert.Single(registry.Usage);
        Assert.Equal((2L, 1L, 1L, 2), (usage.Calls, usage.Failures, usage.Timeouts, usage.CachedModels));
        Assert.Equal(AiError.NotFound, (await registry.ListModelsAsync("nobody", false, Ct)).Failure.Code);
        Assert.Equal(AiError.InvalidArgument, (await registry.CompleteAsync(GroqProvider.ProviderId,
            new AiRequest(" ", [new AiMessage(AiRole.User, "hi")]), Ct)).Failure.Code);
    }

    [Fact]
    public async Task The_registry_refuses_an_id_twice_another_id_and_config_for_no_provider()
    {
        async Task<string> Refusal(AiProviderRegistration[] registrations, AiProviderConfig[] config)
        {
            using var registry = new AiProviderRegistry(registrations, config,
                new AiCredentialStore(Path.Combine(_dir.Path, "credentials.json"), config),
                new AiSettingsStore(Path.Combine(_dir.Path, "settings.json")), NullLogger<AiProviderRegistry>.Instance);
            return (await Assert.ThrowsAsync<InvalidOperationException>(() => registry.StartAsync(Ct))).Message;
        }

        var ollama = new AiProviderRegistration(OllamaProvider.ProviderId, c => new OllamaProvider(c), TimeSpan.FromSeconds(1));

        Assert.Equal("AI provider ollama is registered twice", await Refusal([ollama, ollama], []));
        Assert.Equal("AI provider registered as local reports the id ollama",
            await Refusal([ollama with { Id = "local" }], []));
        Assert.Equal("ai.providers names openai, which is no AI provider",
            await Refusal([ollama], [new AiProviderConfig("openai", "k", null)]));
    }

    [Fact]
    public void Keys_survive_a_restart_in_an_owner_only_file_and_the_configs_are_read_only()
    {
        var path = Path.Combine(_dir.Path, "ai", "credentials.json");
        var config = new[] { new AiProviderConfig("anthropic", "sk-config", null) };
        var store = new AiCredentialStore(path, config);

        Assert.True(store.Set("openai", " sk-set-by-api ").Value);
        Assert.Equal(AiError.InvalidArgument, store.Set("anthropic", "other").Failure.Code);
        Assert.Equal(AiError.InvalidArgument, store.Set("groq", " ").Failure.Code);
        Assert.False(store.Remove("groq").Value);

        var again = new AiCredentialStore(path, config);
        Assert.Equal(("sk-set-by-api", AiKeyOrigin.Api), (again.Key("openai"), again.Origin("openai")));
        Assert.Equal(("sk-config", AiKeyOrigin.Config), (again.Key("anthropic"), again.Origin("anthropic")));
        Assert.Equal((null, AiKeyOrigin.None), (again.Key("groq"), again.Origin("groq")));
        if (OperatingSystem.IsWindows())
        {
            var acl = new FileInfo(path).GetAccessControl();
            Assert.True(acl.AreAccessRulesProtected);
            var rule = Assert.Single(acl.GetAccessRules(true, true, typeof(SecurityIdentifier)).Cast<FileSystemAccessRule>());
            Assert.Equal(WindowsIdentity.GetCurrent().User, rule.IdentityReference);
        }
        else
        {
            Assert.Equal(UnixFileMode.UserRead | UnixFileMode.UserWrite, File.GetUnixFileMode(path));
        }

        Assert.True(again.Remove("openai").Value);
        Assert.Null(new AiCredentialStore(path, config).Key("openai"));
    }

    private static (string Role, string Content) Message(JsonNode request, int index) =>
        (request["messages"]![index]!["role"]!.GetValue<string>(), request["messages"]![index]!["content"]!.GetValue<string>());
}
