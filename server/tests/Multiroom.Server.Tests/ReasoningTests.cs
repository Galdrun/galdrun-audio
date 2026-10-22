using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Ai;
using Multiroom.Server.Ai.Providers;
using Multiroom.Server.Ai.Providers.Anthropic;
using Multiroom.Server.Ai.Providers.OpenAi;
using Multiroom.Server.Ai.Providers.Gemini;
using Multiroom.Server.Ai.Providers.Groq;
using Multiroom.Server.Ai.Providers.Mistral;
using Multiroom.Server.Ai.Providers.Ollama;
using Multiroom.Server.Ai.Providers.OpenRouter;

namespace Multiroom.Server.Tests;

// Reasoning (phase 29) through the registry, against the vendors' fakes:
// each level becomes the vendor's own field, with MaxTokens raised by the
// level's budget; default sends no field at all; a provider without the
// capability refuses by name; a vendor's 400 for the setting is
// invalidArgument with its message; Anthropic's thinking blocks go back with
// their turn's tool results only, and their text is in no answer.
public sealed class ReasoningTests : IDisposable
{
    private static readonly AiTool SearchTool = new("search", "Searches the music.",
        JsonSerializer.SerializeToElement(new { type = "object", properties = new { query = new { type = "string" } } }));

    private static readonly AiReasoning[] Levels =
        [AiReasoning.Default, AiReasoning.Off, AiReasoning.Low, AiReasoning.Medium, AiReasoning.High];

    private readonly TempDirectory _dir = new();

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    public void Dispose() => _dir.Dispose();

    private async Task<AiProviderRegistry> RegistryAsync(string id, Func<AiProviderContext, IAiProvider> create, Uri baseUrl,
        string? key)
    {
        AiProviderConfig[] config = [new(id, key, baseUrl)];
        // A provider that lists oauth comes with its login; none is used here.
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

    private static AiRequest Ask(AiReasoning level) => new("model-a", [new AiMessage(AiRole.User, "hi")], Reasoning: level);

    private static int Raised(AiReasoning level) => AiRequest.DefaultMaxTokens + AiReasoningBudget.Of(level);

    [Theory]
    [InlineData("openai")]
    [InlineData("gemini")]
    [InlineData("groq")]
    [InlineData("ollama")]
    [InlineData("openrouter")]
    public async Task Each_level_becomes_the_chat_completions_vendors_field_and_raises_the_limit(string id)
    {
        await using var fake = await FakeOpenAi.StartAsync();
        using var registry = await RegistryAsync(id, c => id switch
        {
            "openai" => new OpenAiProvider(c),
            "gemini" => new GeminiProvider(c),
            "groq" => new GroqProvider(c),
            "ollama" => new OllamaProvider(c),
            _ => new OpenRouterProvider(c),
        }, fake.BaseUri, FakeOpenAi.Key);
        var limitField = id == "openai" ? "max_completion_tokens" : "max_tokens";

        foreach (var level in Levels)
        {
            var answer = await registry.CompleteAsync(id, Ask(level), Ct);
            Assert.True(answer.IsSuccess, $"{level}: {answer}");
            var sent = fake.Requests.Last();
            Assert.Equal(Raised(level), sent[limitField]!.GetValue<int>());
            var effort = level switch
            {
                AiReasoning.Default => null,
                AiReasoning.Off => "none",
                _ => Word(level),
            };
            if (id == "openrouter")
            {
                Assert.Null(sent["reasoning_effort"]);
                Assert.Equal(effort, sent["reasoning"]?["effort"]?.GetValue<string>());
                Assert.Equal(effort is null ? null : true, sent["reasoning"]?["exclude"]?.GetValue<bool>());
            }
            else
            {
                Assert.Null(sent["reasoning"]);
                Assert.Equal(effort, sent["reasoning_effort"]?.GetValue<string>());
            }
        }
        // Default is today's request, byte for byte: no field was added.
        Assert.Equal(["model", "messages", limitField], fake.Requests.First().AsObject().Select(p => p.Key));
    }

    [Fact]
    public async Task A_provider_without_the_capability_refuses_a_level_and_a_vendors_400_is_invalid_argument()
    {
        await using var fake = await FakeOpenAi.StartAsync();
        using var mistral = await RegistryAsync("mistral", c => new MistralProvider(c), fake.BaseUri, FakeOpenAi.Key);
        var refused = await mistral.CompleteAsync("mistral", Ask(AiReasoning.Low), Ct);
        Assert.Equal((AiError.InvalidArgument, "mistral takes no reasoning setting"), (refused.Failure.Code, refused.Failure.Detail));
        Assert.Empty(fake.Requests);
        Assert.True((await mistral.CompleteAsync("mistral", Ask(AiReasoning.Default), Ct)).IsSuccess);

        using var openai = await RegistryAsync("openai", c => new OpenAiProvider(c), fake.BaseUri, FakeOpenAi.Key);
        fake.Refusal = (400, """{"error":{"message":"Unsupported value: 'reasoning_effort' does not support 'none' with this model.","type":"invalid_request_error"}}""");
        fake.RefuseTimes = 1;
        var vendor = await openai.CompleteAsync("openai", Ask(AiReasoning.Off), Ct);
        Assert.Equal((AiError.InvalidArgument,
                "openai: HTTP 400: Unsupported value: 'reasoning_effort' does not support 'none' with this model."),
            (vendor.Failure.Code, vendor.Failure.Detail));

        // Reasoning tokens are counted where the vendor reports them.
        fake.Answers.Enqueue("""
            {"choices":[{"index":0,"message":{"role":"assistant","content":"ok","reasoning_content":"thought it over"},
              "finish_reason":"stop"}],"usage":{"prompt_tokens":3,"completion_tokens":9,"completion_tokens_details":{"reasoning_tokens":6}}}
            """);
        var thought = (await openai.CompleteAsync("openai", Ask(AiReasoning.High), Ct)).Value;
        Assert.Equal(("ok", new AiUsage(3, 9, 6)), (thought.Text, thought.Usage));
        Assert.Equal(6, Assert.Single(openai.Usage).ReasoningTokens);
    }

    [Fact]
    public async Task Anthropic_thinks_adaptively_at_an_effort_and_sends_its_thinking_back_with_its_turns_tool_results_only()
    {
        await using var fake = await FakeAnthropic.StartAsync();
        using var registry = await RegistryAsync("anthropic", c => new AnthropicProvider(c), fake.BaseUri, FakeAnthropic.Key);

        foreach (var level in Levels)
        {
            Assert.True((await registry.CompleteAsync("anthropic", Ask(level), Ct)).IsSuccess, level.ToString());
            var sent = fake.Requests.Last();
            Assert.Equal(Raised(level), sent["max_tokens"]!.GetValue<int>());
            Assert.Equal(level switch
            {
                AiReasoning.Default => null,
                AiReasoning.Off => "disabled",
                _ => "adaptive",
            }, sent["thinking"]?["type"]?.GetValue<string>());
            Assert.Equal(level is AiReasoning.Low or AiReasoning.Medium or AiReasoning.High
                ? Word(level)
                : null, sent["output_config"]?["effort"]?.GetValue<string>());
        }
        Assert.Equal(["model", "max_tokens", "messages"], fake.Requests.First().AsObject().Select(p => p.Key));

        // The API refuses a forced tool while the model thinks: said before asking.
        var forced = await registry.CompleteAsync("anthropic", new AiRequest("claude-a", [new AiMessage(AiRole.User, "hi")],
            Tools: [SearchTool], RequiredTool: "search", Reasoning: AiReasoning.High), Ct);
        Assert.Equal(AiError.InvalidArgument, forced.Failure.Code);

        fake.Answers.Enqueue("""
            {"id":"msg_1","type":"message","role":"assistant","content":[
              {"type":"thinking","thinking":"SECRET-THOUGHT","signature":"sig-1"},
              {"type":"redacted_thinking","data":"opaque"},
              {"type":"text","text":"Let me look."},
              {"type":"tool_use","id":"toolu_1","name":"search","input":{"query":"miles"}}],
             "stop_reason":"tool_use","usage":{"input_tokens":20,"output_tokens":9}}
            """);
        var user = new AiMessage(AiRole.User, "Play some Miles Davis.");
        var asked = (await registry.CompleteAsync("anthropic",
            new AiRequest("claude-a", [user], Tools: [SearchTool], Reasoning: AiReasoning.High), Ct)).Value;
        Assert.Equal("Let me look.", asked.Text);
        Assert.DoesNotContain("SECRET-THOUGHT", JsonSerializer.Serialize(asked with { ToolCalls = [] }), StringComparison.Ordinal);

        await registry.CompleteAsync("anthropic", new AiRequest("claude-a",
        [
            user,
            new AiMessage(AiRole.Assistant, asked.Text, asked.ToolCalls),
            new AiMessage(AiRole.Tool, """{"found":1}""", ToolCallId: "toolu_1"),
        ], Tools: [SearchTool], Reasoning: AiReasoning.High), Ct);
        var content = fake.Requests.Last()["messages"]![1]!["content"]!.AsArray();
        Assert.Equal(["thinking", "redacted_thinking", "text", "tool_use"], content.Select(b => b!["type"]!.GetValue<string>()));
        Assert.Equal(("SECRET-THOUGHT", "sig-1"), (content[0]!["thinking"]!.GetValue<string>(), content[0]!["signature"]!.GetValue<string>()));

        // The next user turn: the turn that ended carries no thinking.
        await registry.CompleteAsync("anthropic", new AiRequest("claude-a",
        [
            user,
            new AiMessage(AiRole.Assistant, "Playing Kind of Blue."),
            new AiMessage(AiRole.User, "Another one."),
        ], Reasoning: AiReasoning.High), Ct);
        var next = fake.Requests.Last()["messages"]!.ToJsonString();
        Assert.DoesNotContain("SECRET-THOUGHT", next, StringComparison.Ordinal);
        Assert.DoesNotContain("sig-1", next, StringComparison.Ordinal);
    }

    private static string Word(AiReasoning level) => level switch
    {
        AiReasoning.Low => "low",
        AiReasoning.Medium => "medium",
        _ => "high",
    };

    [Fact]
    public async Task The_chatgpt_plan_takes_an_effort_and_no_limit()
    {
        await using var fake = await FakeOpenAiResponses.StartAsync();
        using var registry = await RegistryAsync(OpenAiProvider.ProviderId, c => new ChatGptPlan(c), fake.BaseUri, "plan-token");

        foreach (var level in Levels)
        {
            Assert.True((await registry.CompleteAsync(OpenAiProvider.ProviderId, Ask(level), Ct)).IsSuccess, level.ToString());
            var sent = fake.Requests.Last();
            Assert.Null(sent["max_output_tokens"]);
            Assert.Equal(level switch
            {
                AiReasoning.Default => null,
                AiReasoning.Off => "none",
                _ => Word(level),
            }, sent["reasoning"]?["effort"]?.GetValue<string>());
        }
        Assert.Equal(["model", "input", "store", "stream"], fake.Requests.First().AsObject().Select(p => p.Key));
    }
}
