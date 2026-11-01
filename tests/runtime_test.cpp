#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agent/core/chat_model.h"
#include "agent/core/tracer.h"
#include "agent/runtime/agent_runner.h"
#include "agent/runtime/agent_session.h"
#include "agent/runtime/service_registry.h"
#include "agent/runtime/tool_registry.h"

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

agent::runtime::AgentSessionDependencies MakeDependencies(FakeChatModel& model)
{
    agent::runtime::AgentSessionDependencies dependencies;
    dependencies.model = &model;
    return dependencies;
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

    agent::core::ToolResult Execute(const std::string& arguments_json) override
    {
        ++m_calls;
        m_last_arguments = arguments_json;
        agent::core::ToolResult result;
        result.content = "ok";
        return result;
    }

    std::string m_name;
    int m_calls = 0;
    std::string m_last_arguments;
};

} // namespace

TEST_CASE("session keeps conversation and system prompt")
{
    FakeChatModel model;
    model.response.ok = true;
    model.response.content = "hello";

    agent::runtime::AgentSession session(MakeConfig(), MakeDependencies(model));

    const agent::runtime::AgentRunResult first = session.RunTurn("hi");
    CHECK(first.ok);
    CHECK(first.content == "hello");
    CHECK(session.History().messages.size() == 3);
    CHECK(model.requests.size() == 1);
    CHECK(model.requests[0].messages.size() == 2);
    CHECK(model.requests[0].messages[0].role == agent::core::ChatRole::System);
    CHECK(model.requests[0].messages[1].role == agent::core::ChatRole::User);

    model.response.content = "again";
    const agent::runtime::AgentRunResult second = session.RunTurn("next");
    CHECK(second.ok);
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

    agent::runtime::AgentSession session(MakeConfig(), MakeDependencies(model));
    const agent::runtime::AgentRunResult result = session.RunTurn("hi");

    CHECK_FALSE(result.ok);
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
    agent::core::SystemRegistry systems;
    systems.Add(tracer);

    agent::runtime::AgentSessionDependencies dependencies = MakeDependencies(model);
    dependencies.systems = &systems;
    agent::runtime::AgentSession session(MakeConfig(), dependencies);

    const agent::runtime::AgentRunResult result = session.RunTurn("hi");

    CHECK(result.ok);
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

    agent::runtime::ToolRegistry tools;
    const auto echo = std::make_shared<FakeTool>("echo");
    std::string error;
    REQUIRE(tools.Register(echo, error));

    const auto tracer = std::make_shared<FakeTracer>();
    agent::core::SystemRegistry systems;
    systems.Add(tracer);
    agent::runtime::AgentRunner runner(model, tools, &systems);
    agent::runtime::AgentProfile profile;
    profile.max_steps = 4;
    profile.tool_names = {"echo"};

    agent::core::Conversation conversation;
    const agent::runtime::AgentRunResult result = runner.Run(profile, conversation, "use echo");

    CHECK(result.ok);
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

    agent::runtime::ToolRegistry tools;
    const auto echo = std::make_shared<FakeTool>("echo");
    std::string error;
    REQUIRE(tools.Register(echo, error));

    agent::runtime::AgentRunner runner(model, tools);
    agent::runtime::AgentProfile profile;
    profile.max_steps = 2;
    profile.tool_names = {"echo"};

    agent::core::Conversation conversation;
    const agent::runtime::AgentRunResult result = runner.Run(profile, conversation, "use echo");

    CHECK_FALSE(result.ok);
    CHECK(result.error.find("max steps") != std::string::npos);
    CHECK(result.steps == 2);
    CHECK(conversation.messages.size() == 5);
    CHECK(echo->m_calls == 2);
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

    agent::runtime::ToolRegistry tools;
    agent::runtime::AgentRunner runner(model, tools);
    agent::runtime::AgentProfile profile;
    profile.max_steps = 3;

    agent::core::Conversation conversation;
    const agent::runtime::AgentRunResult result = runner.Run(profile, conversation, "try missing");

    CHECK(result.ok);
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

    agent::runtime::ToolRegistry tools;
    const auto echo = std::make_shared<FakeTool>("echo");
    std::string error;
    REQUIRE(tools.Register(echo, error));

    agent::runtime::AgentSessionDependencies dependencies = MakeDependencies(model);
    dependencies.tools = &tools;
    agent::runtime::AgentSessionConfig config = MakeConfig();
    config.tool_names = {"echo"};
    agent::runtime::AgentSession session(config, dependencies);

    const agent::runtime::AgentRunResult result = session.RunTurn("use echo");

    CHECK(result.ok);
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
