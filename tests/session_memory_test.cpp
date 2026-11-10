#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agent/core/chat_model.h"
#include "agent/core/conversation.h"
#include "agent/core/memory.h"
#include "agent/core/summarizer.h"
#include "agent/plugins/memory/model_summarizer.h"
#include "agent/plugins/memory/rule_based_summarizer.h"
#include "agent/runtime/agent.h"
#include "agent/runtime/agent_runner.h"
#include "agent/runtime/memory_policy.h"
#include "support/runtime_fixture.h"

namespace
{

class FakeChatModel final : public agent::core::ChatModel
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

class FakeSummarizer final : public agent::core::Summarizer
{
  public:
    agent::core::SummaryResult Summarize(const agent::core::SummaryRequest& request) override
    {
        requests.push_back(request);
        return result;
    }

    std::vector<agent::core::SummaryRequest> requests;
    agent::core::SummaryResult result;
};

std::shared_ptr<FakeSummarizer> MakeSummarizer()
{
    auto summarizer = std::make_shared<FakeSummarizer>();
    summarizer->result.ok = true;
    summarizer->result.summary = "SUMMARY";
    return summarizer;
}

std::size_t CountRole(const agent::core::Conversation& conversation, agent::core::ChatRole role)
{
    std::size_t count = 0;
    for (const agent::core::ChatMessage& message : conversation.messages)
    {
        if (message.role == role)
        {
            ++count;
        }
    }
    return count;
}

bool HasSummaryMessage(const agent::core::Conversation& conversation, const std::string& needle = std::string())
{
    for (const agent::core::ChatMessage& message : conversation.messages)
    {
        if (message.role == agent::core::ChatRole::System &&
            message.content.rfind(agent::core::kSummaryMessagePrefix, 0) == 0)
        {
            if (needle.empty() || message.content.find(needle) != std::string::npos)
            {
                return true;
            }
        }
    }
    return false;
}

agent::core::SummaryRequest MakeSummaryRequest()
{
    agent::core::SummaryRequest request;
    request.turns.push_back({agent::core::ChatRole::User, "电话是 13800001111", {}, {}});
    request.turns.push_back({agent::core::ChatRole::Assistant, "好的，已记住", {}, {}});
    return request;
}

} // namespace

TEST_CASE("session compacts old turns into a summary and keeps recent turns")
{
    FakeChatModel model;
    model.response.ok = true;
    model.response.content = "ok";

    agent::runtime::ServiceRegistry services;
    services.Set<agent::runtime::ToolPolicy>(std::make_shared<agent::runtime::AllowAllToolPolicy>());
    services.Set<agent::runtime::ContextPolicy>(std::make_shared<agent::runtime::KeepRecentTurnsPolicy>());
    services.Set<agent::runtime::ToolOutputPolicy>(std::make_shared<agent::runtime::TruncateToolOutputPolicy>());
    auto dependencies = agent::tests::MakeRunnerDependencies(model, services);
    auto summarizer = MakeSummarizer();
    dependencies.summarizer = summarizer;

    agent::runtime::AgentProfile profile;
    profile.system_prompt = "be brief";
    profile.memory_policy.enable_compaction = true;
    profile.memory_policy.compact_trigger_chars = 10;
    profile.memory_policy.compact_keep_recent_turns = 2;

    agent::runtime::AgentDescriptor descriptor;
    descriptor.id.value = "npc";
    agent::runtime::AgentDependencies agent_dependencies;
    agent_dependencies.executor = std::make_shared<agent::runtime::AgentRunner>(dependencies);
    agent::runtime::Agent agent(descriptor, profile, agent_dependencies, "session-compact");

    REQUIRE(agent.Send("turn-1").Ok());
    REQUIRE(agent.Send("turn-2").Ok());
    REQUIRE(agent.Send("turn-3").Ok());

    const agent::core::Conversation history = agent.History();
    REQUIRE(history.messages.size() == 6);
    CHECK(history.messages[0].content == "be brief");
    CHECK(HasSummaryMessage(history, "SUMMARY"));
    CHECK(CountRole(history, agent::core::ChatRole::User) == 2);
    CHECK(history.messages[2].content == "turn-2");
    CHECK(history.messages[4].content == "turn-3");
    REQUIRE(summarizer->requests.size() == 1);
    CHECK(summarizer->requests.front().max_chars == 2000);
    CHECK(summarizer->requests.front().turns.size() == 2);
    CHECK(summarizer->requests.front().turns.front().content == "turn-1");
}

TEST_CASE("session keeps history when compaction fails or is disabled")
{
    FakeChatModel model;
    model.response.ok = true;
    model.response.content = "ok";

    agent::runtime::ServiceRegistry services;
    services.Set<agent::runtime::ToolPolicy>(std::make_shared<agent::runtime::AllowAllToolPolicy>());
    services.Set<agent::runtime::ContextPolicy>(std::make_shared<agent::runtime::KeepRecentTurnsPolicy>());
    services.Set<agent::runtime::ToolOutputPolicy>(std::make_shared<agent::runtime::TruncateToolOutputPolicy>());

    auto failing = MakeSummarizer();
    failing->result.ok = false;
    failing->result.error = "summarizer unavailable";
    auto failing_dependencies = agent::tests::MakeRunnerDependencies(model, services);
    failing_dependencies.summarizer = failing;

    agent::runtime::AgentProfile failing_profile;
    failing_profile.system_prompt = "be brief";
    failing_profile.memory_policy.enable_compaction = true;
    failing_profile.memory_policy.compact_trigger_chars = 10;
    failing_profile.memory_policy.compact_keep_recent_turns = 1;

    agent::runtime::AgentDescriptor descriptor;
    descriptor.id.value = "npc-failing";
    agent::runtime::AgentDependencies failing_agent_dependencies;
    failing_agent_dependencies.executor = std::make_shared<agent::runtime::AgentRunner>(failing_dependencies);
    agent::runtime::Agent failing_agent(descriptor, failing_profile, failing_agent_dependencies, "session-failing");
    REQUIRE(failing_agent.Send("turn-1").Ok());
    REQUIRE(failing_agent.Send("turn-2").Ok());
    const agent::core::Conversation failing_history = failing_agent.History();
    CHECK(CountRole(failing_history, agent::core::ChatRole::User) == 2);
    CHECK_FALSE(HasSummaryMessage(failing_history));
    CHECK(failing->requests.size() == 1);

    auto disabled_summarizer = MakeSummarizer();
    auto disabled_dependencies = agent::tests::MakeRunnerDependencies(model, services);
    disabled_dependencies.summarizer = disabled_summarizer;

    agent::runtime::AgentProfile disabled_profile;
    disabled_profile.system_prompt = "be brief";
    disabled_profile.memory_policy.enable_compaction = false;

    agent::runtime::AgentDescriptor disabled_descriptor;
    disabled_descriptor.id.value = "npc-disabled";
    agent::runtime::AgentDependencies disabled_agent_dependencies;
    disabled_agent_dependencies.executor = std::make_shared<agent::runtime::AgentRunner>(disabled_dependencies);
    agent::runtime::Agent disabled_agent(disabled_descriptor, disabled_profile, disabled_agent_dependencies,
                                         "session-disabled");
    REQUIRE(disabled_agent.Send("turn-1").Ok());
    REQUIRE(disabled_agent.Send("turn-2").Ok());
    CHECK(CountRole(disabled_agent.History(), agent::core::ChatRole::User) == 2);
    CHECK(disabled_summarizer->requests.empty());
}

TEST_CASE("model summarizer calls the chat model with deterministic settings")
{
    auto model = std::make_shared<FakeChatModel>();
    model->response.ok = true;
    model->response.content = "compressed summary";

    agent::plugins::memory::ModelSummarizer::Config config;
    config.model = "cheap-model";
    config.max_tokens = 32;
    agent::plugins::memory::ModelSummarizer summarizer(model, config);
    const agent::core::SummaryRequest request = MakeSummaryRequest();

    const agent::core::SummaryResult result = summarizer.Summarize(request);
    CHECK(result.ok);
    CHECK(result.summary == "compressed summary");
    REQUIRE(model->requests.size() == 1);
    CHECK(model->requests.front().model == "cheap-model");
    CHECK(model->requests.front().temperature == 0.0);
    CHECK(model->requests.front().max_tokens == 32);
    REQUIRE(model->requests.front().messages.size() == 2);
    CHECK(model->requests.front().messages[0].role == agent::core::ChatRole::System);
    CHECK(model->requests.front().messages[1].content.find("13800001111") != std::string::npos);
}

TEST_CASE("model summarizer truncates and falls back on failure")
{
    auto model = std::make_shared<FakeChatModel>();
    model->response.ok = true;
    model->response.content = std::string(100, 'x');

    agent::plugins::memory::ModelSummarizer::Config config;
    agent::plugins::memory::ModelSummarizer summarizer(model, config);
    agent::core::SummaryRequest request = MakeSummaryRequest();
    request.max_chars = 10;
    const agent::core::SummaryResult truncated = summarizer.Summarize(request);
    CHECK(truncated.ok);
    CHECK(truncated.summary.size() <= 10);

    model->response = {};
    model->response.error = "provider down";
    const agent::core::SummaryResult failed = summarizer.Summarize(request);
    CHECK_FALSE(failed.ok);
    CHECK(failed.error.find("provider down") != std::string::npos);

    auto fallback = std::make_shared<agent::plugins::memory::RuleBasedSummarizer>(4);
    agent::plugins::memory::ModelSummarizer fallback_summarizer(model, config, fallback);
    const agent::core::SummaryResult recovered = fallback_summarizer.Summarize(MakeSummaryRequest());
    CHECK(recovered.ok);
    CHECK(recovered.summary.find("13800001111") != std::string::npos);
}
