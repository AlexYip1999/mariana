#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "agent/core/chat_model.h"
#include "agent/core/tracer.h"
#include "agent/runtime/agent.h"
#include "agent/runtime/agent_runner.h"
#include "agent/runtime/agent_session.h"
#include "agent/runtime/composition.h"
#include "agent/runtime/service_registry.h"
#include "agent/runtime/tool_descriptor.h"
#include "agent/runtime/tool_registry.h"
#include "support/runtime_fixture.h"

namespace
{

class FakeChatModel : public agent::core::ChatModel
{
  public:
    agent::core::ChatResponse Chat(const agent::core::ChatRequest& request) override
    {
        requests.push_back(request);
        if (!responses.empty())
        {
            agent::core::ChatResponse next = responses.front();
            responses.erase(responses.begin());
            return next;
        }
        return response;
    }

    std::vector<agent::core::ChatRequest> requests;
    std::vector<agent::core::ChatResponse> responses;
    agent::core::ChatResponse response;
};

agent::runtime::AgentSessionConfig MakeConfig()
{
    agent::runtime::AgentSessionConfig config;
    config.model = "deepseek-flash";
    config.system_prompt = "be brief";
    config.temperature = 0.1;
    config.max_tokens = 64;
    return config;
}

agent::runtime::ServiceRegistry MakeServices()
{
    agent::runtime::ServiceRegistry services;
    services.Set<agent::runtime::ToolPolicy>(std::make_shared<agent::runtime::AllowAllToolPolicy>());
    services.Set<agent::runtime::ContextPolicy>(std::make_shared<agent::runtime::KeepRecentTurnsPolicy>());
    services.Set<agent::runtime::ToolOutputPolicy>(std::make_shared<agent::runtime::TruncateToolOutputPolicy>());
    return services;
}

class FakeTracer : public agent::core::NullTracer
{
  public:
    void OnTurnStart() override
    {
        ++m_turns;
    }

    void OnModelRequest(const agent::core::ChatRequest&) override
    {
        ++m_model_requests;
    }

    void OnModelResponse(const agent::core::ChatResponse&) override
    {
        ++m_model_responses;
    }

    void OnToolCall(const agent::core::ToolCall&) override
    {
        ++m_tool_calls;
    }

    void OnToolResult(const agent::core::ToolCall&, const agent::core::ToolResult&) override
    {
        ++m_tool_results;
    }

    void OnTurnEnd(bool ok, const std::string&, int steps) override
    {
        ++m_turn_ends;
        m_last_turn_ok = ok;
        m_last_steps = steps;
    }

    int m_turns = 0;
    int m_model_requests = 0;
    int m_model_responses = 0;
    int m_tool_calls = 0;
    int m_tool_results = 0;
    int m_turn_ends = 0;
    bool m_last_turn_ok = false;
    int m_last_steps = 0;
};

class ThrowingSystem : public agent::core::AgentSystem
{
  public:
    void OnTurnStart() override
    {
        throw std::runtime_error("hook boom");
    }
};

class DenyAllToolPolicy : public agent::runtime::ToolPolicy
{
  public:
    bool Allow(const agent::core::ToolCall&, std::string& reason) const override
    {
        reason = "denied by test";
        return false;
    }
};

class FakeTool : public agent::core::Tool
{
  public:
    explicit FakeTool(std::string name)
        : m_name(std::move(name))
    {}

    std::string Name() const override
    {
        return m_name;
    }

    std::string Description() const override
    {
        return "fake tool";
    }

    std::string ParametersJson() const override
    {
        return R"({"type":"object"})";
    }

    agent::core::ToolResult Execute(const std::string& arguments_json, const agent::core::CallContext&) override
    {
        ++m_calls;
        m_last_arguments = arguments_json;
        agent::core::ToolResult result;
        result.content = m_result_content;
        return result;
    }

    std::string m_name;
    int m_calls = 0;
    std::string m_last_arguments;
    std::string m_result_content = "ok";
};

} // namespace

TEST_CASE("agent facade owns descriptor and session")
{
    FakeChatModel model;
    model.response.ok = true;
    model.response.content = "hello";

    auto services = std::make_shared<agent::runtime::ServiceRegistry>();
    services->Set<agent::runtime::ToolPolicy>(std::make_shared<agent::runtime::AllowAllToolPolicy>());
    services->Set<agent::runtime::ContextPolicy>(std::make_shared<agent::runtime::KeepRecentTurnsPolicy>());
    services->Set<agent::runtime::ToolOutputPolicy>(std::make_shared<agent::runtime::TruncateToolOutputPolicy>());
    const auto tracer = std::make_shared<FakeTracer>();
    auto systems = std::make_shared<agent::core::SystemRegistry>();
    systems->Add(tracer);
    services->Set<agent::core::SystemRegistry>(systems);
    auto executor =
        std::make_shared<agent::runtime::AgentRunner>(agent::tests::MakeRunnerDependencies(model, *services));

    agent::runtime::AgentDescriptor descriptor;
    descriptor.id.value = "npc-1";
    descriptor.name = "merchant";
    descriptor.version = "1";
    descriptor.default_profile = MakeConfig();
    descriptor.capabilities = {"trade"};

    agent::runtime::AgentDependencies agent_services;
    agent_services.services = services;
    agent_services.executor = executor;
    agent::runtime::Agent agent(descriptor, agent_services, "session-1");
    CHECK(agent.Id().value == "npc-1");
    CHECK(agent.SessionId() == "session-1");
    CHECK(agent.SessionId() == agent.Session().Id());
    CHECK(agent.Descriptor().name == "merchant");
    CHECK(agent.Profile().system_prompt == "be brief");
    CHECK(agent.Dependencies().services == services);

    const agent::runtime::AgentRunResult result = agent.Send("hi");
    CHECK(result.Ok());
    CHECK(agent.History().messages.size() == 3);
    CHECK(tracer->m_turns == 1);
    CHECK(tracer->m_turn_ends == 1);

    agent::runtime::AgentProfile profile = agent.Profile();
    profile.tool_names = {"read_file"};
    agent.SetProfile(std::move(profile));
    CHECK(agent.Profile().tool_names == std::vector<std::string>{"read_file"});
    CHECK(agent.Descriptor().default_profile.tool_names.empty());

    agent.ClearHistory();
    CHECK(agent.History().messages.size() == 1);
}

TEST_CASE("agent creates an empty scope when dependencies omit one")
{
    FakeChatModel model;
    model.response.ok = true;
    model.response.content = "hello";
    auto services = MakeServices();
    auto executor =
        std::make_shared<agent::runtime::AgentRunner>(agent::tests::MakeRunnerDependencies(model, services));

    agent::runtime::AgentDescriptor descriptor;
    descriptor.id.value = "worker";
    descriptor.default_profile = MakeConfig();

    agent::runtime::AgentDependencies dependencies;
    dependencies.executor = executor;
    agent::runtime::Agent agent(descriptor, dependencies, "session-worker");
    REQUIRE(agent.Dependencies().services != nullptr);
    CHECK(agent.Send("hi").Ok());
    CHECK(agent.SessionId() == "session-worker");
}

TEST_CASE("agent rejects mismatched scope and executor system registries")
{
    FakeChatModel model;
    auto runner_services = MakeServices();
    auto executor_systems = std::make_shared<agent::core::SystemRegistry>();
    runner_services.Set<agent::core::SystemRegistry>(executor_systems);
    auto dependencies = agent::tests::MakeRunnerDependencies(model, runner_services);

    auto scope = std::make_shared<agent::runtime::ServiceRegistry>();
    auto scope_systems = std::make_shared<agent::core::SystemRegistry>();
    scope->Set<agent::core::SystemRegistry>(scope_systems);

    agent::runtime::AgentDescriptor descriptor;
    descriptor.id.value = "npc-mismatch";
    descriptor.default_profile = MakeConfig();

    agent::runtime::AgentDependencies agent_dependencies;
    agent_dependencies.services = scope;
    agent_dependencies.executor = std::make_shared<agent::runtime::AgentRunner>(dependencies);
    CHECK_THROWS_AS(agent::runtime::Agent(descriptor, agent_dependencies, "session-mismatch"), std::invalid_argument);
}

TEST_CASE("agent adopts executor systems when the scope has none")
{
    FakeChatModel model;
    model.response.ok = true;
    model.response.content = "hello";
    auto runner_services = MakeServices();
    auto systems = std::make_shared<agent::core::SystemRegistry>();
    runner_services.Set<agent::core::SystemRegistry>(systems);
    auto dependencies = agent::tests::MakeRunnerDependencies(model, runner_services);

    auto scope = std::make_shared<agent::runtime::ServiceRegistry>();
    agent::runtime::AgentDescriptor descriptor;
    descriptor.id.value = "npc-scope";
    descriptor.default_profile = MakeConfig();

    agent::runtime::AgentDependencies agent_dependencies;
    agent_dependencies.services = scope;
    agent_dependencies.executor = std::make_shared<agent::runtime::AgentRunner>(dependencies);
    agent::runtime::Agent agent(descriptor, agent_dependencies, "session-scope");
    CHECK(scope->Get<agent::core::SystemRegistry>() == systems);
    CHECK(agent.Send("hi").Ok());
}

TEST_CASE("session keeps a bounded number of turn records")
{
    FakeChatModel model;
    model.response.ok = true;
    model.response.content = "hello";
    auto services = MakeServices();
    auto executor =
        std::make_shared<agent::runtime::AgentRunner>(agent::tests::MakeRunnerDependencies(model, services));
    agent::runtime::AgentSession session(MakeConfig(), executor, {}, nullptr, 2);
    REQUIRE(session.Send("one").Ok());
    REQUIRE(session.Send("two").Ok());
    REQUIRE(session.Send("three").Ok());
    CHECK(session.Records().size() == 2);
}

TEST_CASE("session keeps conversation and system prompt")
{
    FakeChatModel model;
    model.response.ok = true;
    model.response.content = "hello";

    auto services = MakeServices();
    agent::runtime::AgentRunner executor(agent::tests::MakeRunnerDependencies(model, services));
    agent::runtime::AgentSession session(MakeConfig(), agent::tests::BorrowForTest(executor));

    const agent::runtime::AgentRunResult first = session.Send("hi");
    CHECK(first.Ok());
    CHECK(first.content == "hello");
    CHECK(session.History().messages.size() == 3);
    CHECK(model.requests.size() == 1);
    CHECK(model.requests[0].messages.size() == 2);
    CHECK(model.requests[0].messages[0].role == agent::core::ChatRole::System);
    CHECK(model.requests[0].messages[1].role == agent::core::ChatRole::User);

    model.response.content = "again";
    const agent::runtime::AgentRunResult second = session.Send("next");
    CHECK(second.Ok());
    CHECK(session.History().messages.size() == 5);
    CHECK(model.requests[1].messages.size() == 4);
    CHECK(model.requests[1].messages[2].role == agent::core::ChatRole::Assistant);
    CHECK(model.requests[1].messages[2].content == "hello");

    session.ClearHistory();
    CHECK(session.History().messages.size() == 1);
    CHECK(session.History().messages[0].role == agent::core::ChatRole::System);
}

TEST_CASE("session keeps failed turn messages")
{
    FakeChatModel model;
    model.response.ok = false;
    model.response.error = "boom";

    auto services = MakeServices();
    agent::runtime::AgentRunner executor(agent::tests::MakeRunnerDependencies(model, services));
    agent::runtime::AgentSession session(MakeConfig(), agent::tests::BorrowForTest(executor));
    const agent::runtime::AgentRunResult result = session.Send("hi");

    CHECK_FALSE(result.Ok());
    CHECK(result.error == "boom");
    CHECK(result.steps == 1);
    CHECK(session.History().messages.size() == 2);
    CHECK(session.History().messages[0].role == agent::core::ChatRole::System);
    CHECK(session.History().messages[1].role == agent::core::ChatRole::User);
    CHECK(session.History().messages[1].content == "hi");
}

TEST_CASE("session emits turn start through tracer")
{
    FakeChatModel model;
    model.response.ok = true;
    model.response.content = "hello";

    const auto tracer = std::make_shared<FakeTracer>();
    auto systems = std::make_shared<agent::core::SystemRegistry>();
    systems->Add(tracer);

    auto services = MakeServices();
    services.Set<agent::core::SystemRegistry>(systems);
    agent::runtime::AgentRunner executor(agent::tests::MakeRunnerDependencies(model, services));
    agent::runtime::AgentSession session(MakeConfig(), agent::tests::BorrowForTest(executor), {}, systems);

    const agent::runtime::AgentRunResult result = session.Send("hi");

    CHECK(result.Ok());
    CHECK(tracer->m_turns == 1);
}

TEST_CASE("agent runner executes tool calls until final answer")
{
    FakeChatModel model;
    agent::core::ChatResponse tool_response;
    tool_response.ok = true;
    agent::core::ToolCall call;
    call.id = "call_1";
    call.name = "echo";
    call.arguments_json = R"({"x":1})";
    tool_response.tool_calls.push_back(call);
    agent::core::ChatResponse final_response;
    final_response.ok = true;
    final_response.content = "done";
    model.responses = {tool_response, final_response};

    auto tools = std::make_shared<agent::runtime::ToolRegistry>();
    const auto echo = std::make_shared<FakeTool>("echo");
    std::string error;
    REQUIRE(tools->Register(echo, error));

    const auto tracer = std::make_shared<FakeTracer>();
    auto systems = std::make_shared<agent::core::SystemRegistry>();
    systems->Add(tracer);

    auto services = MakeServices();
    services.Set<agent::runtime::ToolRegistry>(tools);
    services.Set<agent::core::SystemRegistry>(systems);
    agent::runtime::AgentRunner runner(agent::tests::MakeRunnerDependencies(model, services));
    agent::runtime::AgentProfile profile;
    profile.max_steps = 4;
    profile.tool_names = {"echo"};

    agent::core::Conversation conversation;
    const agent::runtime::AgentRunResult result =
        agent::tests::RunTurnForTest(runner, profile, conversation, "use echo", systems);

    CHECK(result.Ok());
    CHECK(result.content == "done");
    CHECK(result.steps == 2);
    CHECK(echo->m_calls == 1);
    CHECK(echo->m_last_arguments == R"({"x":1})");
    CHECK(conversation.messages.size() == 4);
    CHECK(conversation.messages[2].role == agent::core::ChatRole::Tool);
    CHECK(conversation.messages[2].tool_call_id == "call_1");
    CHECK(conversation.messages[2].content == "ok");
    CHECK(model.requests[0].tools.size() == 1);
    CHECK(model.requests[1].messages.size() == 3);
    CHECK(tracer->m_turns == 1);
    CHECK(tracer->m_model_requests == 2);
    CHECK(tracer->m_tool_calls == 1);
    CHECK(tracer->m_turn_ends == 1);
    CHECK(tracer->m_last_turn_ok);
    CHECK(tracer->m_last_steps == 2);
}

TEST_CASE("agent runner rejects truncated response")
{
    FakeChatModel model;
    model.response.ok = true;
    model.response.content = "partial";
    model.response.finish_reason = "length";

    auto services = MakeServices();
    agent::runtime::AgentRunner runner(agent::tests::MakeRunnerDependencies(model, services));
    agent::runtime::AgentProfile profile;
    profile.max_steps = 2;

    agent::core::Conversation conversation;
    const agent::runtime::AgentRunResult result = agent::tests::RunTurnForTest(runner, profile, conversation, "hi");

    CHECK_FALSE(result.Ok());
    CHECK(result.error.find("incomplete") != std::string::npos);
    REQUIRE(conversation.messages.size() == 1);
    CHECK(conversation.messages[0].role == agent::core::ChatRole::User);
}

TEST_CASE("agent runner rejects empty response without appending assistant")
{
    FakeChatModel model;
    model.response.ok = true;
    model.response.content = "";

    auto services = MakeServices();
    agent::runtime::AgentRunner runner(agent::tests::MakeRunnerDependencies(model, services));
    agent::runtime::AgentProfile profile;
    profile.max_steps = 2;

    agent::core::Conversation conversation;
    const agent::runtime::AgentRunResult result = agent::tests::RunTurnForTest(runner, profile, conversation, "hi");

    CHECK_FALSE(result.Ok());
    CHECK(result.error.find("empty response") != std::string::npos);
    REQUIRE(conversation.messages.size() == 1);
    CHECK(conversation.messages[0].role == agent::core::ChatRole::User);
}

TEST_CASE("agent runner asks tool policy before execution")
{
    FakeChatModel model;
    agent::core::ChatResponse tool_response;
    tool_response.ok = true;
    agent::core::ToolCall call;
    call.id = "call_1";
    call.name = "echo";
    tool_response.tool_calls.push_back(call);
    agent::core::ChatResponse final_response;
    final_response.ok = true;
    final_response.content = "recovered";
    model.responses = {tool_response, final_response};

    auto tools = std::make_shared<agent::runtime::ToolRegistry>();
    const auto echo = std::make_shared<FakeTool>("echo");
    std::string error;
    REQUIRE(tools->Register(echo, error));

    auto services = MakeServices();
    services.Set<agent::runtime::ToolRegistry>(tools);
    services.Set<agent::runtime::ToolPolicy>(std::make_shared<DenyAllToolPolicy>());
    agent::runtime::AgentRunner runner(agent::tests::MakeRunnerDependencies(model, services));
    agent::runtime::AgentProfile profile;
    profile.max_steps = 3;
    profile.tool_names = {"echo"};

    agent::core::Conversation conversation;
    const agent::runtime::AgentRunResult result =
        agent::tests::RunTurnForTest(runner, profile, conversation, "use echo");

    CHECK(result.Ok());
    CHECK(echo->m_calls == 0);
    REQUIRE(conversation.messages.size() == 4);
    CHECK(conversation.messages[2].content.find("denied by policy") != std::string::npos);
}

TEST_CASE("agent runner limits tool output per turn")
{
    FakeChatModel model;
    agent::core::ChatResponse tool_response;
    tool_response.ok = true;
    agent::core::ToolCall call;
    call.id = "call_1";
    call.name = "echo";
    tool_response.tool_calls.push_back(call);
    agent::core::ChatResponse final_response;
    final_response.ok = true;
    final_response.content = "done";
    model.responses = {tool_response, final_response};

    auto tools = std::make_shared<agent::runtime::ToolRegistry>();
    const auto echo = std::make_shared<FakeTool>("echo");
    echo->m_result_content = std::string(100000, 'x');
    std::string error;
    REQUIRE(tools->Register(echo, error));

    auto services = MakeServices();
    services.Set<agent::runtime::ToolRegistry>(tools);
    agent::runtime::AgentRunner runner(agent::tests::MakeRunnerDependencies(model, services));
    agent::runtime::AgentProfile profile;
    profile.max_steps = 3;
    profile.tool_names = {"echo"};

    agent::core::Conversation conversation;
    const agent::runtime::AgentRunResult result = agent::tests::RunTurnForTest(runner, profile, conversation, "read");

    CHECK(result.Ok());
    REQUIRE(conversation.messages.size() == 4);
    const std::string& tool_content = conversation.messages[2].content;
    CHECK(tool_content.size() < 40000);
    CHECK(tool_content.find("[truncated]") != std::string::npos);
}

TEST_CASE("agent runner isolates observer exceptions")
{
    FakeChatModel model;
    model.response.ok = true;
    model.response.content = "hello";

    const auto throwing = std::make_shared<ThrowingSystem>();
    auto systems = std::make_shared<agent::core::SystemRegistry>();
    systems->Add(throwing);
    auto services = MakeServices();
    services.Set<agent::core::SystemRegistry>(systems);
    agent::runtime::AgentRunner runner(agent::tests::MakeRunnerDependencies(model, services));
    agent::runtime::AgentProfile profile;
    profile.max_steps = 2;

    agent::core::Conversation conversation;
    const agent::runtime::AgentRunResult result = agent::tests::RunTurnForTest(runner, profile, conversation, "hi");

    CHECK(result.Ok());
    CHECK(result.error.empty());
    CHECK(conversation.messages.size() == 2);
}

TEST_CASE("end observers run once and cannot change the turn outcome")
{
    class ThrowingEndSystem final : public FakeTracer
    {
      public:
        void OnTurnEnd(bool ok, const std::string& error, int steps) override
        {
            FakeTracer::OnTurnEnd(ok, error, steps);
            throw std::runtime_error("end boom");
        }
    };

    const bool outcomes[] = {false, true};
    for (const bool model_ok : outcomes)
    {
        FakeChatModel model;
        model.response.ok = model_ok;
        model.response.content = model_ok ? "done" : "";
        model.response.error = model_ok ? "" : "model failed";
        auto throwing = std::make_shared<ThrowingEndSystem>();
        auto recording = std::make_shared<FakeTracer>();
        auto systems = std::make_shared<agent::core::SystemRegistry>();
        systems->Add(throwing);
        systems->Add(recording);
        auto services = MakeServices();
        services.Set<agent::core::SystemRegistry>(systems);
        agent::runtime::AgentRunner runner(agent::tests::MakeRunnerDependencies(model, services));
        agent::core::Conversation conversation;
        const auto result = agent::tests::RunTurnForTest(runner, {}, conversation, "hi", systems);
        CHECK(result.Ok() == model_ok);
        CHECK(result.error == model.response.error);
        CHECK(result.content == model.response.content);
        CHECK(conversation.messages.size() == (model_ok ? 2 : 1));
        CHECK(throwing->m_turn_ends == 1);
        CHECK(recording->m_turn_ends == 1);
        CHECK(recording->m_last_turn_ok == model_ok);
    }
}

TEST_CASE("profile changes replace remove and restore system prompts without losing history")
{
    FakeChatModel model;
    model.response.ok = true;
    model.response.content = "answer";
    auto services = MakeServices();
    agent::runtime::AgentRunner runner(agent::tests::MakeRunnerDependencies(model, services));
    agent::runtime::AgentSession session(MakeConfig(), agent::tests::BorrowForTest(runner));
    REQUIRE(session.Send("first").Ok());
    auto profile = session.Profile();
    profile.system_prompt = "new instruction";
    session.SetProfile(profile);
    REQUIRE(session.Send("second").Ok());
    CHECK(model.requests.back().messages.front().content == "new instruction");
    CHECK(model.requests.back().messages[1].content == "first");

    profile.system_prompt.clear();
    session.SetProfile(profile);
    REQUIRE(session.Send("third").Ok());
    CHECK(model.requests.back().messages.front().role == agent::core::ChatRole::User);
    profile.system_prompt = "restored";
    session.SetProfile(profile);
    REQUIRE(session.Send("fourth").Ok());
    CHECK(model.requests.back().messages.front().content == "restored");
    CHECK(session.History().messages.size() == 9);
}

TEST_CASE("context trimming retains complete tool turns and the newest oversized turn")
{
    agent::core::Conversation conversation;
    conversation.Append(agent::core::ChatRole::System, "sys");
    for (int index = 0; index < 10000; ++index)
    {
        conversation.Append(agent::core::ChatRole::User, "old");
        conversation.Append(agent::core::ChatRole::Assistant, "answer");
    }
    conversation.Append(agent::core::ChatRole::User, "latest");
    agent::core::ChatMessage assistant;
    assistant.role = agent::core::ChatRole::Assistant;
    assistant.tool_calls.push_back({"id", "echo", "{}"});
    conversation.Append(assistant);
    agent::core::ChatMessage tool;
    tool.role = agent::core::ChatRole::Tool;
    tool.tool_call_id = "id";
    tool.content = "result";
    conversation.Append(tool);

    agent::runtime::KeepRecentTurnsPolicy policy;
    const std::size_t budgets[] = {1, 25, 30};
    for (const std::size_t budget : budgets)
    {
        const auto prepared = policy.Prepare(conversation.messages, budget);
        REQUIRE(prepared.size() == 4);
        CHECK(prepared[0].content == "sys");
        CHECK(prepared[1].content == "latest");
        REQUIRE(prepared[2].tool_calls.size() == 1);
        CHECK(prepared[2].tool_calls[0].id == prepared[3].tool_call_id);
    }
    CHECK(policy.Prepare(conversation.messages, 0).size() == conversation.messages.size());
    CHECK(conversation.messages.size() == 20004);
}

TEST_CASE("tool output truncation preserves UTF-8 code points")
{
    agent::runtime::TruncateToolOutputPolicy policy;
    agent::runtime::ToolOutputBudget budget;
    budget.max_bytes = 5;
    const std::string result = policy.Limit({}, "中文", budget);
    CHECK(result == "中\n[truncated]");
    CHECK(budget.used_bytes == budget.max_bytes);
}

TEST_CASE("cancelled turns restore history and propagate cancellation to the model")
{
    class CancellingModel final : public FakeChatModel
    {
      public:
        agent::core::ChatResponse Chat(const agent::core::ChatRequest& request) override
        {
            auto result = FakeChatModel::Chat(request);
            if (requests.size() == 2)
            {
                stop.request_stop();
            }
            return result;
        }

        std::stop_source stop;
    };

    CancellingModel model;
    model.response.ok = true;
    model.response.content = "done";
    agent::core::ChatResponse tool_response;
    tool_response.ok = true;
    tool_response.tool_calls.push_back({"call", "echo", "{}"});
    model.responses = {tool_response, model.response};
    auto services = MakeServices();
    auto tools = std::make_shared<agent::runtime::ToolRegistry>();
    auto echo = std::make_shared<FakeTool>("echo");
    std::string error;
    REQUIRE(tools->Register(echo, error));
    services.Set<agent::runtime::ToolRegistry>(tools);
    auto tracer = std::make_shared<FakeTracer>();
    auto systems = std::make_shared<agent::core::SystemRegistry>();
    systems->Add(tracer);
    services.Set<agent::core::SystemRegistry>(systems);
    agent::runtime::AgentRunner runner(agent::tests::MakeRunnerDependencies(model, services));
    auto profile = MakeConfig();
    profile.tool_names = {"echo"};
    agent::runtime::AgentSession session(profile, agent::tests::BorrowForTest(runner), {}, systems);
    const auto result = session.Send("use echo", model.stop.get_token());
    CHECK_FALSE(result.Ok());
    CHECK(result.error.find("cancelled") != std::string::npos);
    CHECK(echo->m_calls == 1);
    REQUIRE(model.requests.size() == 2);
    CHECK(model.requests[0].call.stop_token == model.stop.get_token());
    CHECK(model.requests[1].call.stop_token == model.stop.get_token());
    REQUIRE(session.History().messages.size() == 1);
    CHECK(session.History().messages.front().content == profile.system_prompt);
    CHECK(tracer->m_turn_ends == 1);
    CHECK_FALSE(tracer->m_last_turn_ok);

    CHECK_FALSE(session.Send("already cancelled", model.stop.get_token()).Ok());
    CHECK(model.requests.size() == 2);
    CHECK(tracer->m_turn_ends == 1);
    CHECK(session.Records().size() == 1);
    CHECK(session.Send("new turn").Ok());
    CHECK(session.History().messages.size() == 3);
}

TEST_CASE("agent runner trims old conversation to context budget")
{
    FakeChatModel model;
    model.response.ok = true;
    model.response.content = "done";

    auto services = MakeServices();
    agent::runtime::AgentRunner runner(agent::tests::MakeRunnerDependencies(model, services));
    agent::runtime::AgentProfile profile;
    profile.max_steps = 1;
    profile.max_context_chars = 2500;

    agent::core::Conversation conversation;
    conversation.Append(agent::core::ChatRole::System, "system");
    for (int index = 0; index < 10; ++index)
    {
        conversation.Append(agent::core::ChatRole::User, std::string(1000, 'u'));
        conversation.Append(agent::core::ChatRole::Assistant, std::string(1000, 'a'));
    }

    const agent::runtime::AgentRunResult result = agent::tests::RunTurnForTest(runner, profile, conversation, "latest");

    CHECK(result.Ok());
    REQUIRE(model.requests.size() == 1);
    const std::vector<agent::core::ChatMessage>& messages = model.requests[0].messages;
    CHECK(messages.front().role == agent::core::ChatRole::System);
    CHECK(messages.back().content == "latest");
    CHECK(messages.size() < conversation.messages.size());
    CHECK(messages.size() >= 2);
}

TEST_CASE("agent runner stops at max steps and keeps messages")
{
    FakeChatModel model;
    agent::core::ChatResponse tool_response;
    tool_response.ok = true;
    agent::core::ToolCall call;
    call.id = "call_1";
    call.name = "echo";
    tool_response.tool_calls.push_back(call);
    model.responses = {tool_response, tool_response};

    auto tools = std::make_shared<agent::runtime::ToolRegistry>();
    const auto echo = std::make_shared<FakeTool>("echo");
    std::string error;
    REQUIRE(tools->Register(echo, error));

    auto services = MakeServices();
    services.Set<agent::runtime::ToolRegistry>(tools);
    agent::runtime::AgentRunner runner(agent::tests::MakeRunnerDependencies(model, services));
    agent::runtime::AgentProfile profile;
    profile.max_steps = 2;
    profile.tool_names = {"echo"};

    agent::core::Conversation conversation;
    const agent::runtime::AgentRunResult result =
        agent::tests::RunTurnForTest(runner, profile, conversation, "use echo");

    CHECK_FALSE(result.Ok());
    CHECK(result.error.find("max steps") != std::string::npos);
    CHECK(result.steps == 2);
    CHECK(conversation.messages.size() == 5);
    CHECK(echo->m_calls == 1);
}

TEST_CASE("agent runner feeds unknown tool errors back to model")
{
    FakeChatModel model;
    agent::core::ChatResponse tool_response;
    tool_response.ok = true;
    agent::core::ToolCall call;
    call.id = "call_1";
    call.name = "missing_tool";
    tool_response.tool_calls.push_back(call);
    agent::core::ChatResponse final_response;
    final_response.ok = true;
    final_response.content = "recovered";
    model.responses = {tool_response, final_response};

    auto services = MakeServices();
    agent::runtime::AgentRunner runner(agent::tests::MakeRunnerDependencies(model, services));
    agent::runtime::AgentProfile profile;
    profile.max_steps = 3;

    agent::core::Conversation conversation;
    const agent::runtime::AgentRunResult result =
        agent::tests::RunTurnForTest(runner, profile, conversation, "try missing");

    CHECK(result.Ok());
    CHECK(result.content == "recovered");
    CHECK(result.steps == 2);
    REQUIRE(conversation.messages.size() == 4);
    CHECK(conversation.messages[2].role == agent::core::ChatRole::Tool);
    CHECK(conversation.messages[2].tool_call_id == "call_1");
    CHECK(conversation.messages[2].content.find("tool not allowed") != std::string::npos);
    CHECK(model.requests[1].messages.size() == 3);
}

TEST_CASE("agent session uses tool registry when present")
{
    FakeChatModel model;
    agent::core::ChatResponse tool_response;
    tool_response.ok = true;
    agent::core::ToolCall call;
    call.id = "call_1";
    call.name = "echo";
    tool_response.tool_calls.push_back(call);
    agent::core::ChatResponse final_response;
    final_response.ok = true;
    final_response.content = "done";
    model.responses = {tool_response, final_response};

    auto tools = std::make_shared<agent::runtime::ToolRegistry>();
    const auto echo = std::make_shared<FakeTool>("echo");
    std::string error;
    REQUIRE(tools->Register(echo, error));

    agent::runtime::AgentSessionConfig config = MakeConfig();
    config.tool_names = {"echo"};
    auto services = MakeServices();
    services.Set<agent::runtime::ToolRegistry>(tools);
    agent::runtime::AgentRunner executor(agent::tests::MakeRunnerDependencies(model, services));
    agent::runtime::AgentSession session(config, agent::tests::BorrowForTest(executor));

    const agent::runtime::AgentRunResult result = session.Send("use echo");

    CHECK(result.Ok());
    CHECK(result.content == "done");
    CHECK(echo->m_calls == 1);
    CHECK(session.History().messages.size() == 5);
    CHECK(model.requests[0].tools.size() == 1);
}

TEST_CASE("component registry creates once and caches")
{
    agent::runtime::ServiceRegistry registry;
    int created = 0;

    const auto factory = [&created]()
    {
        ++created;
        return std::make_shared<int>(42);
    };

    const std::shared_ptr<int> first = registry.CreateOrGet<int>(factory);
    const std::shared_ptr<int> second = registry.CreateOrGet<int>(factory);

    CHECK(first != nullptr);
    CHECK(second == first);
    CHECK(*first == 42);
    CHECK(created == 1);
    CHECK(registry.Get<double>() == nullptr);
}

TEST_CASE("service registry falls back to parent scope")
{
    auto parent = std::make_shared<agent::runtime::ServiceRegistry>();
    const auto parent_policy = std::make_shared<agent::runtime::AllowAllToolPolicy>();
    parent->Set<agent::runtime::ToolPolicy>(parent_policy);

    agent::runtime::ServiceRegistry child(parent);
    CHECK(child.Parent() == parent.get());
    CHECK(child.Get<agent::runtime::ToolPolicy>() == parent_policy);

    const auto child_policy = std::make_shared<agent::runtime::AllowAllToolPolicy>();
    child.Set<agent::runtime::ToolPolicy>(child_policy);
    CHECK(child.Get<agent::runtime::ToolPolicy>() == child_policy);
    CHECK(parent->Get<agent::runtime::ToolPolicy>() == parent_policy);
}

TEST_CASE("tool registry registers by name and executes")
{
    agent::runtime::ToolRegistry registry;
    std::string error;

    const auto read_file = std::make_shared<FakeTool>("read_file");
    const auto search_web = std::make_shared<FakeTool>("search_web");
    REQUIRE(registry.Register(read_file, error));
    REQUIRE(registry.Register(search_web, error));

    CHECK(registry.Size() == 2);
    CHECK(registry.Has("read_file"));
    CHECK(registry.Find("read_file") == read_file.get());
    CHECK(registry.AllSpecs().size() == 2);
    CHECK(registry.AllSpecs()[0].name == "read_file");

    const std::vector<agent::core::ToolSpec> specs = registry.SpecsFor({"search_web", "missing"});
    CHECK(specs.size() == 1);
    CHECK(specs[0].name == "search_web");

    const agent::core::ToolResult result = registry.Execute("read_file", R"({"path":"README.md"})");
    CHECK(result.ok);
    CHECK(result.content == "ok");
    CHECK(read_file->m_calls == 1);
    CHECK(read_file->m_last_arguments == R"({"path":"README.md"})");

    const agent::core::ToolResult missing = registry.Execute("missing", "{}");
    CHECK_FALSE(missing.ok);
    CHECK(missing.error.find("unknown tool") != std::string::npos);
}

TEST_CASE("tool registry stores governance descriptors")
{
    agent::runtime::ToolRegistry registry;
    std::string error;
    auto tool = std::make_shared<FakeTool>("write_file");
    agent::runtime::ToolDescriptor descriptor;
    descriptor.name = "write_file";
    descriptor.side_effects = true;
    descriptor.access_scope = agent::runtime::ToolAccessScope::Write;
    descriptor.approval = agent::runtime::ToolApprovalMode::Required;
    descriptor.thread_affinity = "host";
    REQUIRE(registry.Register(tool, descriptor, error));

    const agent::runtime::ToolDescriptor* stored = registry.Descriptor("write_file");
    REQUIRE(stored != nullptr);
    CHECK(stored->side_effects);
    CHECK(stored->access_scope == agent::runtime::ToolAccessScope::Write);
    CHECK(stored->approval == agent::runtime::ToolApprovalMode::Required);
    CHECK(stored->thread_affinity == "host");
    CHECK(registry.Descriptors().size() == 1);

    agent::runtime::ToolDescriptor mismatched = descriptor;
    mismatched.name = "other";
    CHECK_FALSE(registry.Register(std::make_shared<FakeTool>("next"), mismatched, error));
    CHECK(error.find("descriptor name mismatch") != std::string::npos);
}

TEST_CASE("composition validation rejects side-effecting tools without approval")
{
    FakeChatModel model;
    auto services = MakeServices();
    auto dependencies = agent::tests::MakeRunnerDependencies(model, services);
    auto tools = std::make_shared<agent::runtime::ToolRegistry>();
    std::string error;
    agent::runtime::ToolDescriptor descriptor;
    descriptor.name = "write_file";
    descriptor.side_effects = true;
    descriptor.approval = agent::runtime::ToolApprovalMode::NotRequired;
    REQUIRE(tools->Register(std::make_shared<FakeTool>("write_file"), descriptor, error));
    dependencies.tools = tools;

    agent::runtime::AgentProfile profile;
    profile.tool_names = {"write_file"};
    const agent::runtime::CompositionValidation validation = agent::runtime::ValidateComposition(dependencies, profile);
    CHECK_FALSE(validation.Ok());
    CHECK(validation.Describe().find("approval") != std::string::npos);
}

TEST_CASE("tool registry rejects invalid registrations")
{
    agent::runtime::ToolRegistry registry;
    std::string error;

    CHECK_FALSE(registry.Register(nullptr, error));
    CHECK(error.find("null") != std::string::npos);

    const auto invalid_name = std::make_shared<FakeTool>("bad name");
    error.clear();
    CHECK_FALSE(registry.Register(invalid_name, error));
    CHECK(error.find("invalid tool name") != std::string::npos);

    const auto first = std::make_shared<FakeTool>("read_file");
    const auto duplicate = std::make_shared<FakeTool>("read_file");
    REQUIRE(registry.Register(first, error));
    error.clear();
    CHECK_FALSE(registry.Register(duplicate, error));
    CHECK(error.find("duplicate tool name") != std::string::npos);
    CHECK(registry.Size() == 1);
}
