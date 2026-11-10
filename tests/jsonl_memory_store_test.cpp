#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "agent/core/memory.h"
#include "agent/plugins/memory/in_memory_store.h"
#include "agent/plugins/memory/jsonl_memory_store.h"

namespace
{

class TempDirectory
{
  public:
    TempDirectory()
    {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        m_path = std::filesystem::temp_directory_path() / ("agent-core-memory-" + std::to_string(stamp));
        std::filesystem::create_directories(m_path);
    }

    ~TempDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    const std::filesystem::path& Path() const
    {
        return m_path;
    }

  private:
    std::filesystem::path m_path;
};

std::shared_ptr<agent::plugins::memory::JsonlMemoryStore> OpenStore(const std::filesystem::path& path,
                                                                    std::string& error)
{
    return agent::plugins::memory::JsonlMemoryStore::Open(path, error);
}

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

std::vector<std::string> HitIds(const std::vector<agent::core::MemoryHit>& hits)
{
    std::vector<std::string> ids;
    for (const agent::core::MemoryHit& hit : hits)
    {
        ids.push_back(hit.record.id);
    }
    return ids;
}

void CheckHitsMatch(const std::vector<agent::core::MemoryHit>& expected,
                    const std::vector<agent::core::MemoryHit>& actual)
{
    REQUIRE(expected.size() == actual.size());
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        CHECK(actual[index].record.id == expected[index].record.id);
        CHECK(actual[index].record.text == expected[index].record.text);
        CHECK(actual[index].score == doctest::Approx(expected[index].score));
    }
}

void WriteLines(const std::filesystem::path& path, const std::vector<std::string>& lines)
{
    std::ofstream file(path, std::ios::out | std::ios::trunc | std::ios::binary);
    for (const std::string& line : lines)
    {
        file << line << "\n";
    }
}

} // namespace

TEST_CASE("jsonl store creates missing directories and an empty file")
{
    TempDirectory temp;
    const std::filesystem::path path = temp.Path() / "nested" / "deeper" / "memory.jsonl";

    std::string error;
    const auto store = OpenStore(path, error);

    INFO("open error: " << error);
    REQUIRE(store != nullptr);
    CHECK(error.empty());
    CHECK(std::filesystem::is_regular_file(path));
    CHECK(store->Path() == path);
    CHECK(store->Size() == 0);
    CHECK(store->Records().empty());
}

TEST_CASE("jsonl store restores record fields after reopen")
{
    TempDirectory temp;
    const std::filesystem::path path = temp.Path() / "memory.jsonl";

    agent::core::MemoryRecord record;
    record.id = "m-1";
    record.kind = agent::core::MemoryKind::Preference;
    record.text = "张三喜欢喝咖啡";
    record.payload_json = R"({"subject":"张三","key":"drink"})";
    record.tags = {"张三", "drink"};
    record.importance = 1.5;
    record.confidence = 0.8;
    record.scope = {"agent", "npc-1"};
    record.source_session = "session-1";
    record.source_run = "run-1";
    record.created_at = 100;
    record.updated_at = 200;
    record.valid_to = 300;
    record.expires_at = 400;

    std::string error;
    {
        const auto store = OpenStore(path, error);
        INFO("open error: " << error);
        REQUIRE(store != nullptr);
        REQUIRE(store->Remember(record, error));
        CHECK(store->Size() == 1);
    }

    const auto reopened = OpenStore(path, error);
    INFO("reopen error: " << error);
    REQUIRE(reopened != nullptr);
    REQUIRE(reopened->Size() == 1);

    const std::vector<agent::core::MemoryRecord> records = reopened->Records();
    REQUIRE(records.size() == 1);
    const agent::core::MemoryRecord& restored = records.front();

    // 标识与分类
    CHECK(restored.id == record.id);
    CHECK(restored.kind == agent::core::MemoryKind::Preference);

    // 内容与可信度
    CHECK(restored.text == record.text);
    CHECK(restored.payload_json == record.payload_json);
    CHECK(restored.tags == record.tags);
    CHECK(restored.importance == doctest::Approx(record.importance));
    CHECK(restored.confidence == doctest::Approx(record.confidence));

    // 作用域、来源与时间
    CHECK(restored.scope.kind == "agent");
    CHECK(restored.scope.id == "npc-1");
    CHECK(restored.source_session == "session-1");
    CHECK(restored.source_run == "run-1");
    CHECK(restored.created_at == 100);
    CHECK(restored.updated_at == 200);
    CHECK(restored.valid_to == 300);
    CHECK(restored.expires_at == 400);

    agent::core::MemoryQuery query;
    query.text = "咖啡";
    query.scopes = {{"agent", "npc-1"}};
    const std::vector<agent::core::MemoryHit> hits = reopened->Recall(query);
    REQUIRE(hits.size() == 1);
    CHECK(hits.front().record.id == "m-1");
    CHECK(hits.front().record.kind == agent::core::MemoryKind::Preference);
}

TEST_CASE("jsonl store keeps the latest record for a repeated id after reopen")
{
    TempDirectory temp;
    const std::filesystem::path path = temp.Path() / "memory.jsonl";

    std::string error;
    {
        const auto store = OpenStore(path, error);
        INFO("open error: " << error);
        REQUIRE(store != nullptr);
        REQUIRE(store->Remember(MakeRecord("m-1", "旧号码 13800001111", "agent", "npc-1", 0.5, 10), error));
        REQUIRE(store->Remember(MakeRecord("m-1", "新号码 13900002222", "agent", "npc-1", 0.9, 20), error));
        CHECK(store->Size() == 1);
    }

    const auto reopened = OpenStore(path, error);
    INFO("reopen error: " << error);
    REQUIRE(reopened != nullptr);
    REQUIRE(reopened->Size() == 1);

    const std::vector<agent::core::MemoryRecord> records = reopened->Records();
    REQUIRE(records.size() == 1);
    CHECK(records.front().id == "m-1");
    CHECK(records.front().text == "新号码 13900002222");
    CHECK(records.front().importance == doctest::Approx(0.9));
    CHECK(records.front().updated_at == 20);

    agent::core::MemoryQuery old_query;
    old_query.text = "13800001111";
    CHECK(reopened->Recall(old_query).empty());

    agent::core::MemoryQuery new_query;
    new_query.text = "13900002222";
    REQUIRE(reopened->Recall(new_query).size() == 1);
}

TEST_CASE("jsonl store persists forget and rejects unknown ids")
{
    TempDirectory temp;
    const std::filesystem::path path = temp.Path() / "memory.jsonl";

    std::string error;
    {
        const auto store = OpenStore(path, error);
        INFO("open error: " << error);
        REQUIRE(store != nullptr);
        REQUIRE(store->Remember(MakeRecord("m-1", "旧号码 13800001111", "agent", "npc-1"), error));
        REQUIRE(store->Remember(MakeRecord("m-2", "街市今天很热闹", "world", "village"), error));

        std::string missing_error;
        CHECK_FALSE(store->Forget("missing", missing_error));
        CHECK(missing_error == "memory not found: missing");
        CHECK(store->Size() == 2);

        std::string forget_error;
        REQUIRE(store->Forget("m-1", forget_error));
        CHECK(forget_error.empty());
        CHECK(store->Size() == 1);
    }

    const auto reopened = OpenStore(path, error);
    INFO("reopen error: " << error);
    REQUIRE(reopened != nullptr);
    REQUIRE(reopened->Size() == 1);

    const std::vector<agent::core::MemoryRecord> records = reopened->Records();
    REQUIRE(records.size() == 1);
    CHECK(records.front().id == "m-2");

    agent::core::MemoryQuery removed_query;
    removed_query.text = "13800001111";
    CHECK(reopened->Recall(removed_query).empty());

    agent::core::MemoryQuery kept_query;
    kept_query.text = "街市";
    REQUIRE(reopened->Recall(kept_query).size() == 1);
}

TEST_CASE("jsonl store clears one scope and keeps other scopes after reopen")
{
    TempDirectory temp;
    const std::filesystem::path path = temp.Path() / "memory.jsonl";

    std::string error;
    {
        const auto store = OpenStore(path, error);
        INFO("open error: " << error);
        REQUIRE(store != nullptr);
        REQUIRE(store->Remember(MakeRecord("w-1", "world event", "world", "village"), error));
        REQUIRE(store->Remember(MakeRecord("a-1", "agent event", "agent", "npc-1"), error));

        std::string clear_error;
        CHECK(store->ClearScope({"world", "village"}, clear_error));
        CHECK(clear_error.empty());
        CHECK(store->Size() == 1);

        // 空 scope 也返回成功，保证重开后不会回放已清空的记录。
        CHECK(store->ClearScope({"world", "village"}, clear_error));
        CHECK(store->Size() == 1);
    }

    const auto reopened = OpenStore(path, error);
    INFO("reopen error: " << error);
    REQUIRE(reopened != nullptr);
    REQUIRE(reopened->Size() == 1);

    const std::vector<agent::core::MemoryRecord> records = reopened->Records();
    REQUIRE(records.size() == 1);
    CHECK(records.front().id == "a-1");
    CHECK(records.front().scope.kind == "agent");
    CHECK(records.front().scope.id == "npc-1");

    agent::core::MemoryQuery cleared_query;
    cleared_query.scopes = {{"world", "village"}};
    CHECK(reopened->Recall(cleared_query).empty());

    agent::core::MemoryQuery kept_query;
    kept_query.scopes = {{"agent", "npc-1"}};
    REQUIRE(reopened->Recall(kept_query).size() == 1);
}

TEST_CASE("jsonl store appends one operation per line")
{
    TempDirectory temp;
    const std::filesystem::path path = temp.Path() / "memory.jsonl";

    std::string error;
    {
        const auto store = OpenStore(path, error);
        INFO("open error: " << error);
        REQUIRE(store != nullptr);
        REQUIRE(store->Remember(MakeRecord("m-1", "号码 13800001111", "agent", "npc-1"), error));
        REQUIRE(store->Forget("m-1", error));
        REQUIRE(store->ClearScope({"agent", "npc-1"}, error));
    }

    std::ifstream file(path, std::ios::in | std::ios::binary);
    REQUIRE(file.is_open());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line))
    {
        lines.push_back(line);
    }

    REQUIRE(lines.size() == 3);
    CHECK(lines[0].find(R"("op":"remember")") != std::string::npos);
    CHECK(lines[0].find(R"("id":"m-1")") != std::string::npos);
    CHECK(lines[0].find(R"("kind":"Fact")") != std::string::npos);
    CHECK(lines[1].find(R"("op":"forget")") != std::string::npos);
    CHECK(lines[1].find(R"("id":"m-1")") != std::string::npos);
    CHECK(lines[2].find(R"("op":"clear_scope")") != std::string::npos);
    CHECK(lines[2].find(R"("scope":{"id":"npc-1","kind":"agent"})") != std::string::npos);
}

TEST_CASE("jsonl store recall matches in-memory store ordering and budgets")
{
    TempDirectory temp;
    const std::filesystem::path path = temp.Path() / "memory.jsonl";

    std::vector<agent::core::MemoryRecord> records;

    agent::core::MemoryRecord fact = MakeRecord("m-1", "张三的电话是 13800001111", "agent", "npc-1", 2.0, 10);
    fact.kind = agent::core::MemoryKind::Fact;
    fact.tags = {"电话"};
    records.push_back(fact);

    agent::core::MemoryRecord event = MakeRecord("m-2", "张三的电话是 13900002222", "agent", "npc-2", 1.5, 20);
    event.kind = agent::core::MemoryKind::Event;
    event.tags = {"电话"};
    records.push_back(event);

    agent::core::MemoryRecord episode = MakeRecord("m-3", "街市今天很热闹", "world", "village");
    episode.kind = agent::core::MemoryKind::Episode;
    episode.tags = {"world"};
    episode.updated_at = 30;
    records.push_back(episode);

    agent::core::MemoryRecord preference = MakeRecord("m-4", "张三的电话是 13700003333", "agent", "npc-1", 1.5, 5);
    preference.kind = agent::core::MemoryKind::Preference;
    preference.tags = {"电话"};
    records.push_back(preference);

    agent::core::MemoryRecord goal = MakeRecord("m-5", "张三的电话是 13600004444", "world", "village", 1.5, 20);
    goal.kind = agent::core::MemoryKind::Goal;
    goal.tags = {"电话"};
    records.push_back(goal);

    // 查询用例：文本命中、tags/kinds/scopes 过滤、limit 与 max_chars 预算
    agent::core::MemoryQuery all_query;
    agent::core::MemoryQuery text_query;
    text_query.text = "电话";
    agent::core::MemoryQuery tag_query;
    tag_query.tags = {"电话"};
    agent::core::MemoryQuery kind_query;
    kind_query.kinds = {agent::core::MemoryKind::Event, agent::core::MemoryKind::Goal};
    agent::core::MemoryQuery scope_query;
    scope_query.scopes = {{"agent", "npc-1"}};
    agent::core::MemoryQuery limit_query;
    limit_query.text = "电话";
    limit_query.limit = 2;
    agent::core::MemoryQuery budget_query;
    budget_query.text = "电话";
    budget_query.max_chars = 9;
    agent::core::MemoryQuery narrow_budget_query;
    narrow_budget_query.max_chars = 7;

    const std::vector<std::string> expected_text_order = {"m-1", "m-2", "m-5", "m-4"};

    std::string error;
    agent::plugins::memory::InMemoryMemoryStore memory_store;
    {
        const auto store = OpenStore(path, error);
        INFO("open error: " << error);
        REQUIRE(store != nullptr);

        for (const agent::core::MemoryRecord& record : records)
        {
            REQUIRE(memory_store.Remember(record, error));
            REQUIRE(store->Remember(record, error));
        }
        REQUIRE(store->Size() == records.size());

        // 排序显式验证：文本命中优先，其次 importance、updated_at、id
        CHECK(HitIds(store->Recall(text_query)) == expected_text_order);

        // 与 InMemoryMemoryStore 逐项对齐
        CheckHitsMatch(memory_store.Recall(all_query), store->Recall(all_query));
        CheckHitsMatch(memory_store.Recall(text_query), store->Recall(text_query));
        CheckHitsMatch(memory_store.Recall(tag_query), store->Recall(tag_query));
        CheckHitsMatch(memory_store.Recall(kind_query), store->Recall(kind_query));
        CheckHitsMatch(memory_store.Recall(scope_query), store->Recall(scope_query));
        CheckHitsMatch(memory_store.Recall(limit_query), store->Recall(limit_query));
        CheckHitsMatch(memory_store.Recall(budget_query), store->Recall(budget_query));
        CheckHitsMatch(memory_store.Recall(narrow_budget_query), store->Recall(narrow_budget_query));
    }

    // 重开后排序、limit 与 UTF-8 截断仍与 InMemoryMemoryStore 一致
    const auto reopened = OpenStore(path, error);
    INFO("reopen error: " << error);
    REQUIRE(reopened != nullptr);
    REQUIRE(reopened->Size() == records.size());
    CHECK(HitIds(reopened->Recall(text_query)) == expected_text_order);
    CheckHitsMatch(memory_store.Recall(all_query), reopened->Recall(all_query));
    CheckHitsMatch(memory_store.Recall(text_query), reopened->Recall(text_query));
    CheckHitsMatch(memory_store.Recall(limit_query), reopened->Recall(limit_query));
    CheckHitsMatch(memory_store.Recall(budget_query), reopened->Recall(budget_query));

    // 预算截断落在 UTF-8 边界：9 字节保留 3 个完整汉字，7 字节回退到 2 个完整汉字
    const std::vector<agent::core::MemoryHit> budgeted = reopened->Recall(budget_query);
    REQUIRE(budgeted.size() == 1);
    CHECK(budgeted.front().record.text == "张三的");

    const std::vector<agent::core::MemoryHit> narrow_budgeted = reopened->Recall(narrow_budget_query);
    REQUIRE(narrow_budgeted.size() == 1);
    CHECK(narrow_budgeted.front().record.text == "张三");
}

TEST_CASE("jsonl store skips blank lines and uses record defaults")
{
    TempDirectory temp;
    const std::filesystem::path path = temp.Path() / "memory.jsonl";

    std::vector<std::string> lines;
    lines.push_back("");
    lines.push_back(R"({"op":"remember","record":{"id":"m-1","kind":"Goal","text":"想学 C++"}})");
    lines.push_back("   ");
    lines.push_back(R"({"op":"forget","id":"m-2"})");
    lines.push_back("");
    WriteLines(path, lines);

    std::string error;
    const auto store = OpenStore(path, error);
    INFO("open error: " << error);
    REQUIRE(store != nullptr);
    REQUIRE(store->Size() == 1);

    const std::vector<agent::core::MemoryRecord> records = store->Records();
    REQUIRE(records.size() == 1);
    CHECK(records.front().id == "m-1");
    CHECK(records.front().kind == agent::core::MemoryKind::Goal);
    CHECK(records.front().text == "想学 C++");
    CHECK(records.front().scope.kind == "agent");
    CHECK(records.front().scope.id.empty());
    CHECK(records.front().importance == doctest::Approx(0.5));
    CHECK(records.front().confidence == doctest::Approx(1.0));
    CHECK(records.front().created_at == 0);
}

TEST_CASE("jsonl store rejects malformed lines and reports line numbers")
{
    TempDirectory temp;
    const std::filesystem::path path = temp.Path() / "memory.jsonl";

    std::vector<std::string> lines;
    lines.push_back(R"({"op":"remember","record":{"id":"m-1","text":"ok"}})");
    lines.push_back("{this is not json");
    lines.push_back(R"({"op":"forget","id":"m-1"})");
    WriteLines(path, lines);

    std::string error;
    const auto store = OpenStore(path, error);
    CHECK(store == nullptr);
    CHECK(error.find("line 2") != std::string::npos);

    std::vector<std::string> unknown_op_lines;
    unknown_op_lines.push_back(R"({"op":"bogus"})");
    WriteLines(path, unknown_op_lines);

    std::string unknown_error;
    const auto rejected = OpenStore(path, unknown_error);
    CHECK(rejected == nullptr);
    CHECK(unknown_error.find("line 1") != std::string::npos);
    CHECK(unknown_error.find("unknown op") != std::string::npos);
}

TEST_CASE("jsonl store rejects empty ids without writing")
{
    TempDirectory temp;
    const std::filesystem::path path = temp.Path() / "memory.jsonl";

    std::string error;
    {
        const auto store = OpenStore(path, error);
        INFO("open error: " << error);
        REQUIRE(store != nullptr);

        std::string remember_error;
        CHECK_FALSE(store->Remember(agent::core::MemoryRecord{}, remember_error));
        CHECK(remember_error == "memory id is required");
        CHECK(store->Size() == 0);
        CHECK(store->Records().empty());
    }

    CHECK(std::filesystem::file_size(path) == 0u);

    const auto reopened = OpenStore(path, error);
    INFO("reopen error: " << error);
    REQUIRE(reopened != nullptr);
    CHECK(reopened->Size() == 0);
}
