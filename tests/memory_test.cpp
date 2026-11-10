#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "agent/core/chat_model.h"
#include "agent/core/memory.h"
#include "agent/core/summarizer.h"
#include "agent/plugins/memory/in_memory_store.h"
#include "agent/plugins/memory/noop_memory_store.h"
#include "agent/plugins/memory/rule_based_summarizer.h"
#include "agent/runtime/agent_runner.h"
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

agent::core::MemoryRecord MakeRecord(std::string id, std::string text, std::string scope_kind, std::string scope_id,
                                     double importance = 0.5, std::int64_t updated_at = 0)
{
    agent::core::MemoryRecord record;
    record.id = std::move(id);
    record.text = std::move(text);
    record.scope.kind = std::move(scope_kind);
    record.scope.id = std::move(scope_id);
    record.importance = importance;
    record.updated_at = updated_at;
    return record;
}

class ThrowingMemoryStore final : public agent::core::MemoryStore
{
  public:
    bool Remember(const agent::core::MemoryRecord&, std::string&) override
    {
        return true;
    }

    std::vector<agent::core::MemoryHit> Recall(const agent::core::MemoryQuery&) override
    {
        throw std::runtime_error("memory backend unavailable");
    }

    bool Forget(const std::string&, std::string&) override
    {
        return true;
    }

    bool ClearScope(const agent::core::MemoryScope&, std::string&) override
    {
        return true;
    }
};

std::size_t FindMessageIndex(const std::vector<agent::core::ChatMessage>& messages, const std::string& needle)
{
    for (std::size_t index = 0; index < messages.size(); ++index)
    {
        if (messages[index].content.find(needle) != std::string::npos)
        {
            return index;
        }
    }
    return messages.size();
}

agent::core::ChatMessage MakeMessage(agent::core::ChatRole role, std::string content)
{
    agent::core::ChatMessage message;
    message.role = role;
    message.content = std::move(content);
    return message;
}

agent::core::ChatMessage MakeToolMessage(std::string content, std::string tool_call_id)
{
    agent::core::ChatMessage message;
    message.role = agent::core::ChatRole::Tool;
    message.content = std::move(content);
    message.tool_call_id = std::move(tool_call_id);
    return message;
}

} // namespace

TEST_CASE("in-memory store recalls by text, tags and scope")
{
    agent::plugins::memory::InMemoryMemoryStore store;
    std::string error;
    REQUIRE(store.Remember(MakeRecord("m-1", "张三的电话是 13800001111", "agent", "npc-1"), error));
    REQUIRE(store.Remember(MakeRecord("m-2", "张三的电话是 13900002222", "agent", "npc-2"), error));
    REQUIRE(store.Remember(MakeRecord("m-3", "街市今天很热闹", "world", "village"), error));

    agent::core::MemoryQuery query;
    query.text = "电话";
    query.scopes = {{"agent", "npc-1"}};
    const std::vector<agent::core::MemoryHit> hits = store.Recall(query);
    REQUIRE(hits.size() == 1);
    CHECK(hits.front().record.id == "m-1");

    agent::core::MemoryQuery tag_query;
    tag_query.tags = {"phone"};
    agent::core::MemoryRecord tagged = MakeRecord("m-4", "娜洛的咖啡店", "agent", "npc-1");
    tagged.tags = {"shop", "phone"};
    REQUIRE(store.Remember(tagged, error));

    const std::vector<agent::core::MemoryHit> tagged_hits = store.Recall(tag_query);
    REQUIRE(tagged_hits.size() == 1);
    CHECK(tagged_hits.front().record.id == "m-4");

    CHECK(store.Recall(agent::core::MemoryQuery{}).size() == 4);
}

TEST_CASE("in-memory store upserts, forgets and clears scope")
{
    agent::plugins::memory::InMemoryMemoryStore store;
    std::string error;
    REQUIRE(store.Remember(MakeRecord("m-1", "旧号码 13800001111", "agent", "npc-1"), error));
    REQUIRE(store.Remember(MakeRecord("m-1", "新号码 13900002222", "agent", "npc-1"), error));
    CHECK(store.Size() == 1);

    agent::core::MemoryQuery query;
    query.text = "13900002222";
    REQUIRE(store.Recall(query).size() == 1);

    {
        std::string missing_error;
        CHECK_FALSE(store.Forget("missing", missing_error));
        CHECK(missing_error.find("not found") != std::string::npos);
    }

    std::string forget_error;
    CHECK(store.Forget("m-1", forget_error));
    CHECK(store.Size() == 0);

    REQUIRE(store.Remember(MakeRecord("w-1", "world event", "world", "village"), error));
    REQUIRE(store.Remember(MakeRecord("a-1", "agent event", "agent", "npc-1"), error));

    std::string clear_error;
    CHECK(store.ClearScope({"world", "village"}, clear_error));
    CHECK(store.Size() == 1);
}

TEST_CASE("noop store accepts writes and never recalls")
{
    agent::plugins::memory::NoopMemoryStore store;
    std::string error;
    CHECK(store.Remember(MakeRecord("m-1", "anything", "agent", "npc-1"), error));
    CHECK(store.Recall(agent::core::MemoryQuery{}).empty());
    CHECK(store.Forget("m-1", error));
    CHECK(store.ClearScope({"agent", "npc-1"}, error));
}

TEST_CASE("rule based summarizer keeps recent content and respects budget")
{
    agent::plugins::memory::RuleBasedSummarizer summarizer(2);
    agent::core::SummaryRequest request;
    request.turns.push_back(MakeMessage(agent::core::ChatRole::User, "第一句话"));
    request.turns.push_back(MakeMessage(agent::core::ChatRole::Assistant, "第二个回答"));
    request.turns.push_back(MakeToolMessage("工具输出不应进入摘要", "call-1"));
    request.turns.push_back(MakeMessage(agent::core::ChatRole::User, "最后一句话"));

    const agent::core::SummaryResult result = summarizer.Summarize(request);
    CHECK(result.ok);
    CHECK(result.summary.find("第一句话") == std::string::npos);
    CHECK(result.summary.find("第二个回答") != std::string::npos);
    CHECK(result.summary.find("最后一句话") != std::string::npos);
    CHECK(result.summary.find("工具输出不应进入摘要") == std::string::npos);

    request.max_chars = 10;
    const agent::core::SummaryResult truncated = summarizer.Summarize(request);
    CHECK(truncated.summary.size() <= 10);
}

TEST_CASE("runner injects recalled memory before the current user input")
{
    FakeChatModel model;
    model.response.ok = true;
    model.response.content = "hello";

    agent::runtime::ServiceRegistry services;
    auto dependencies = agent::tests::MakeRunnerDependencies(model, services);
    auto store = std::make_shared<agent::plugins::memory::InMemoryMemoryStore>();
    std::string error;
    agent::core::MemoryRecord phone = MakeRecord("m-1", "张三的电话是 13800001111", "agent", "npc-1", 2.0, 10);
    phone.tags = {"张三", "电话"};
    REQUIRE(store->Remember(phone, error));
    dependencies.memory = store;
    agent::runtime::AgentRunner runner(dependencies);

    agent::runtime::AgentProfile profile;
    profile.system_prompt = "be brief";
    profile.memory_policy.enable_recall = true;
    profile.memory_policy.recall_scopes = {{"agent", "npc-1"}};

    agent::core::Conversation conversation;
    const agent::runtime::AgentRunResult result =
        agent::tests::RunTurnForTest(runner, profile, conversation, "张三电话多少");

    CHECK(result.Ok());
    REQUIRE(model.requests.size() == 1);

    const std::size_t memory_index = FindMessageIndex(model.requests.front().messages, "<memory>");
    const std::size_t user_index = FindMessageIndex(model.requests.front().messages, "张三电话多少");
    REQUIRE(memory_index < model.requests.front().messages.size());
    REQUIRE(user_index < model.requests.front().messages.size());

    CHECK(memory_index < user_index);
    CHECK(model.requests.front().messages[memory_index].content.find("13800001111") != std::string::npos);
}

TEST_CASE("memory recall is skipped when disabled or store throws")
{
    FakeChatModel model;
    model.response.ok = true;
    model.response.content = "hello";

    agent::runtime::ServiceRegistry services;
    auto dependencies = agent::tests::MakeRunnerDependencies(model, services);
    auto store = std::make_shared<agent::plugins::memory::InMemoryMemoryStore>();
    std::string error;
    REQUIRE(store->Remember(MakeRecord("m-1", "不该被召回", "agent", "npc-1"), error));
    dependencies.memory = store;
    agent::runtime::AgentRunner disabled_runner(dependencies);

    agent::runtime::AgentProfile profile;
    profile.system_prompt = "be brief";

    agent::core::Conversation conversation;
    const agent::runtime::AgentRunResult disabled =
        agent::tests::RunTurnForTest(disabled_runner, profile, conversation, "hello");

    CHECK(disabled.Ok());
    REQUIRE(model.requests.size() == 1);
    CHECK(FindMessageIndex(model.requests.front().messages, "<memory>") == model.requests.front().messages.size());

    auto throwing = std::make_shared<ThrowingMemoryStore>();
    auto throwing_dependencies = agent::tests::MakeRunnerDependencies(model, services);
    throwing_dependencies.memory = throwing;
    agent::runtime::AgentRunner throwing_runner(throwing_dependencies);

    profile.memory_policy.enable_recall = true;
    agent::core::Conversation throwing_conversation;
    const agent::runtime::AgentRunResult degraded =
        agent::tests::RunTurnForTest(throwing_runner, profile, throwing_conversation, "hello");

    CHECK(degraded.Ok());
    REQUIRE(model.requests.size() == 2);
    CHECK(FindMessageIndex(model.requests.back().messages, "<memory>") == model.requests.back().messages.size());
}
