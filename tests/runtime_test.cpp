#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "agent/core/chat_model.h"
#include "agent/runtime/agent_session.h"
#include "agent/runtime/service_registry.h"

namespace
{

class FakeChatModel : public agent::core::ChatModel
{
  public:
    agent::core::ChatResponse Chat(const agent::core::ChatRequest& request) override
    {
        requests.push_back(request);
        return response;
    }

    std::vector<agent::core::ChatRequest> requests;
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

    int m_turns = 0;
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

TEST_CASE("session rolls back failed turn")
{
    FakeChatModel model;
    model.response.ok = false;
    model.response.error = "boom";

    agent::runtime::AgentSession session(MakeConfig(), MakeDependencies(model));
    const agent::runtime::AgentRunResult result = session.RunTurn("hi");

    CHECK_FALSE(result.ok);
    CHECK(result.error == "boom");
    CHECK(session.History().messages.size() == 1);
    CHECK(session.History().messages[0].role == agent::core::ChatRole::System);
}

TEST_CASE("session emits turn start through tracer")
{
    FakeChatModel model;
    model.response.ok = true;
    model.response.content = "hello";

    const auto tracer = std::make_shared<FakeTracer>();
    agent::runtime::ServiceRegistry services;
    services.Set<agent::core::Tracer>(tracer);

    agent::runtime::AgentSessionDependencies dependencies = MakeDependencies(model);
    dependencies.services = &services;
    agent::runtime::AgentSession session(MakeConfig(), dependencies);

    const agent::runtime::AgentRunResult result = session.RunTurn("hi");

    CHECK(result.ok);
    CHECK(tracer->m_turns == 1);
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
