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

// Web lookup (phase 27) through the registry, against the vendors' fakes: the
// flag becomes each vendor's own search, field by field; what the vendor
// cited comes back as sources, and the searches it ran as usage; a provider
// without a search refuses the flag by name; without the flag no request
// carries a search.
public sealed class WebLookupTests : IDisposable
{
    private readonly TempDirectory _dir = new();
    private readonly HttpClient _http = new();

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    public void Dispose()
    {
        _http.Dispose();
        _dir.Dispose();
    }

    private async Task<AiProviderRegistry> RegistryAsync(string id, Func<AiProviderContext, IAiProvider> create, Uri baseUrl,
        string key)
    {
        AiProviderConfig[] config = [new(id, key, baseUrl)];
        AiLoginRegistration[] logins = id switch
        {
            GeminiProvider.ProviderId => [new(id, h => new GoogleLogin(h))],
            OpenRouterProvider.ProviderId => [new(id, h => new OpenRouterLogin(h))],
            OpenAiProvider.ProviderId => [new(id, h => new ChatGptLogin(h, new ChatGptHost(Path.Combine(_dir.Path, ChatGptHost.FileName))))],
            _ => [],
        };
        var registry = new AiProviderRegistry([new AiProviderRegistration(id, create, TimeSpan.FromSeconds(5))], config,
            new AiCredentialStore(Path.Combine(_dir.Path, $"{id}-credentials.json"), config),
            new AiSettingsStore(Path.Combine(_dir.Path, $"{id}-settings.json")), NullLogger<AiProviderRegistry>.Instance, logins);
        await registry.StartAsync(Ct);
        return registry;
    }

    private static AiRequest Ask(bool lookup) => new("model-a", [new AiMessage(AiRole.User, "new albums of 2026")], WebSearch: lookup);

    private const string Cited = """
        {"choices":[{"index":0,"message":{"role":"assistant","content":"Two new albums.","annotations":[
          {"type":"url_citation","url_citation":{"url":"https://example.com/a","title":"A","start_index":0,"end_index":3}},
          {"type":"url_citation","url_citation":{"url":"https://example.com/a","title":"A again","start_index":4,"end_index":7}},
          {"type":"url_citation","url_citation":{"url":"https://example.com/b","title":"B","start_index":8,"end_index":9}}]},
          "finish_reason":"stop"}],"usage":{"prompt_tokens":5,"completion_tokens":7}}
        """;

    [Fact]
    public async Task Openrouter_asks_its_web_plugin_and_keeps_the_cited_pages_once_each()
    {
        await using var fake = await FakeOpenAi.StartAsync();
        using var registry = await RegistryAsync("openrouter", c => new OpenRouterProvider(c), fake.BaseUri, FakeOpenAi.Key);

        Assert.True((await registry.CompleteAsync("openrouter", Ask(false), Ct)).IsSuccess);
        Assert.Null(fake.Requests.Last()["plugins"]);

        fake.Answers.Enqueue(Cited);
        var answer = (await registry.CompleteAsync("openrouter", Ask(true), Ct)).Value;
        Assert.Equal("""[{"id":"web"}]""", fake.Requests.Last()["plugins"]!.ToJsonString());
        Assert.Equal([new AiSource("https://example.com/a", "A"), new AiSource("https://example.com/b", "B")], answer.Sources);
        var usage = Assert.Single(registry.Usage);
        Assert.Equal((1, 0), (usage.WebLookups, usage.WebSearches));
    }

    [Fact]
    public async Task Openai_with_a_key_asks_web_search_options_and_a_vendors_refusal_is_named()
    {
        await using var fake = await FakeOpenAi.StartAsync();
        using var registry = await RegistryAsync("openai", c => new OpenAiProvider(c), fake.BaseUri, FakeOpenAi.Key);

        fake.Answers.Enqueue(Cited);
        var answer = (await registry.CompleteAsync("openai", Ask(true), Ct)).Value;
        Assert.Equal("{}", fake.Requests.Last()["web_search_options"]!.ToJsonString());
        Assert.Equal(2, answer.Sources!.Count);

        // A model that is no search model refuses the field: OpenAI's word.
        fake.Refusal = (400, """{"error":{"message":"Unrecognized request argument supplied: web_search_options","type":"invalid_request_error"}}""");
        fake.RefuseTimes = 1;
        var refused = await registry.CompleteAsync("openai", Ask(true), Ct);
        Assert.Equal((AiError.InvalidArgument, "openai: HTTP 400: Unrecognized request argument supplied: web_search_options"),
            (refused.Failure.Code, refused.Failure.Detail));
    }

    [Theory]
    [InlineData("gemini")]
    [InlineData("groq")]
    [InlineData("mistral")]
    [InlineData("ollama")]
    public async Task A_provider_without_a_search_of_its_own_refuses_the_flag_by_name(string id)
    {
        await using var fake = await FakeOpenAi.StartAsync();
        using var registry = await RegistryAsync(id, c => id switch
        {
            "gemini" => new GeminiProvider(c),
            "groq" => new GroqProvider(c),
            "mistral" => new MistralProvider(c),
            _ => new OllamaProvider(c),
        }, fake.BaseUri, FakeOpenAi.Key);

        var refused = await registry.CompleteAsync(id, Ask(true), Ct);
        Assert.Equal((AiError.InvalidArgument, $"{id} has no web lookup"), (refused.Failure.Code, refused.Failure.Detail));
        Assert.Empty(fake.Requests);
        Assert.False(registry.Supports(id, AiCapability.WebSearch));
    }

    [Fact]
    public async Task Anthropic_asks_its_web_search_tool_and_reads_its_citations_searches_and_blocks()
    {
        await using var fake = await FakeAnthropic.StartAsync();
        using var registry = await RegistryAsync("anthropic", c => new AnthropicProvider(c), fake.BaseUri, FakeAnthropic.Key);

        Assert.True((await registry.CompleteAsync("anthropic", Ask(false), Ct)).IsSuccess);
        Assert.Null(fake.Requests.Last()["tools"]);

        fake.Answers.Enqueue("""
            {"id":"msg_1","type":"message","role":"assistant","content":[
              {"type":"text","text":"I'll search."},
              {"type":"server_tool_use","id":"srvtoolu_1","name":"web_search","input":{"query":"new albums 2026"}},
              {"type":"web_search_tool_result","tool_use_id":"srvtoolu_1","content":[
                {"type":"web_search_result","url":"https://example.com/a","title":"A","encrypted_content":"enc"}]},
              {"type":"text","text":" Two new albums.","citations":[
                {"type":"web_search_result_location","url":"https://example.com/a","title":"A","encrypted_index":"ix","cited_text":"..."}]}],
             "stop_reason":"end_turn","usage":{"input_tokens":30,"output_tokens":9,"server_tool_use":{"web_search_requests":2}}}
            """);
        var answer = (await registry.CompleteAsync("anthropic", Ask(true), Ct)).Value;
        var tool = Assert.Single(fake.Requests.Last()["tools"]!.AsArray())!;
        Assert.Equal(("web_search_20250305", "web_search", AnthropicProvider.MaxSearches),
            (tool["type"]!.GetValue<string>(), tool["name"]!.GetValue<string>(), tool["max_uses"]!.GetValue<int>()));
        Assert.Equal("I'll search. Two new albums.", answer.Text);
        Assert.Equal([new AiSource("https://example.com/a", "A")], answer.Sources);
        Assert.Equal(2, answer.Usage.WebSearches);
        Assert.Equal((1, 2), (registry.Usage[0].WebLookups, registry.Usage[0].WebSearches));
    }

    [Fact]
    public async Task The_chatgpt_plan_asks_the_web_search_tool_beside_its_functions_and_reads_its_citations()
    {
        await using var fake = await FakeOpenAiResponses.StartAsync();
        var provider = new ChatGptPlan(new AiProviderContext(_http, () => "plan-token", fake.BaseUri));
        Assert.Contains(AiCapability.WebSearch, provider.Capabilities);

        fake.Streams.Enqueue(FakeOpenAiResponses.Completed("""
            {"id":"resp_1","status":"completed","output":[
              {"type":"web_search_call","id":"ws_1","status":"completed"},
              {"type":"message","role":"assistant","content":[{"type":"output_text","text":"Two new albums.",
                "annotations":[{"type":"url_citation","url":"https://example.com/b","title":"B","start_index":0,"end_index":3}]}]}],
             "usage":{"input_tokens":4,"output_tokens":6}}
            """));
        var answer = (await provider.CompleteAsync(Ask(true), Ct)).Value;
        Assert.Equal("""[{"type":"web_search"}]""", fake.Requests.Last()["tools"]!.ToJsonString());
        Assert.Equal([new AiSource("https://example.com/b", "B")], answer.Sources);
        Assert.Equal(1, answer.Usage.WebSearches);

        await provider.CompleteAsync(Ask(false), Ct);
        Assert.Null(fake.Requests.Last()["tools"]);
    }
}
