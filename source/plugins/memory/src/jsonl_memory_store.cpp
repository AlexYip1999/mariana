#include "agent/plugins/memory/jsonl_memory_store.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/core/utf8.h"

namespace agent::plugins::memory
{

namespace
{

constexpr const char* REMEMBER_OP = "remember";
constexpr const char* FORGET_OP = "forget";
constexpr const char* CLEAR_SCOPE_OP = "clear_scope";

// 空行（含 CRLF 残留的 '\r' 与纯空白行）不参与重放。
bool IsBlankLine(const std::string& line)
{
    for (char character : line)
    {
        if (character != ' ' && character != '\t' && character != '\r')
        {
            return false;
        }
    }
    return true;
}

// MemoryKind 是从 0 开始的连续枚举，遍历取值即可复用 core::ToString，无需另维护字符串表。
bool TryParseKind(const std::string& name, core::MemoryKind& kind)
{
    for (int value = 0; value <= static_cast<int>(core::MemoryKind::Goal); ++value)
    {
        const core::MemoryKind candidate = static_cast<core::MemoryKind>(value);
        if (core::ToString(candidate) == name)
        {
            kind = candidate;
            return true;
        }
    }
    return false;
}

std::string ReadString(const nlohmann::json& object, const char* key, std::string fallback = std::string())
{
    const auto found = object.find(key);
    if (found == object.end() || found->is_null())
    {
        return fallback;
    }
    return found->get<std::string>();
}

double ReadNumber(const nlohmann::json& object, const char* key, double fallback)
{
    const auto found = object.find(key);
    if (found == object.end() || found->is_null())
    {
        return fallback;
    }
    return found->get<double>();
}

std::int64_t ReadInteger(const nlohmann::json& object, const char* key, std::int64_t fallback)
{
    const auto found = object.find(key);
    if (found == object.end() || found->is_null())
    {
        return fallback;
    }
    return found->get<std::int64_t>();
}

std::vector<std::string> ReadTags(const nlohmann::json& object)
{
    std::vector<std::string> tags;
    const auto found = object.find("tags");
    if (found == object.end() || found->is_null())
    {
        return tags;
    }
    if (!found->is_array())
    {
        throw std::runtime_error("tags must be an array");
    }
    for (const nlohmann::json& tag : *found)
    {
        tags.push_back(tag.get<std::string>());
    }
    return tags;
}

// scope 本身就是对象：既用于 record 内嵌的 scope，也用于 clear_scope 行的顶层 scope。
core::MemoryScope ScopeFromJson(const nlohmann::json& value)
{
    if (!value.is_object())
    {
        throw std::runtime_error("scope must be an object");
    }

    core::MemoryScope scope;
    scope.kind = ReadString(value, "kind", scope.kind);
    scope.id = ReadString(value, "id");
    return scope;
}

core::MemoryScope ReadRecordScope(const nlohmann::json& record_object)
{
    const auto found = record_object.find("scope");
    if (found == record_object.end() || found->is_null())
    {
        return core::MemoryScope{};
    }
    return ScopeFromJson(*found);
}

core::MemoryRecord ReadRecord(const nlohmann::json& value)
{
    if (!value.is_object())
    {
        throw std::runtime_error("record must be an object");
    }

    // 标识与分类（id 必填，kind 缺省用 MemoryRecord 默认值）
    core::MemoryRecord record;
    record.id = ReadString(value, "id");
    if (record.id.empty())
    {
        throw std::runtime_error("record id is required");
    }
    const std::string kind_name = ReadString(value, "kind", core::ToString(record.kind));
    if (!TryParseKind(kind_name, record.kind))
    {
        throw std::runtime_error("unknown memory kind: " + kind_name);
    }

    // 内容与可信度
    record.text = ReadString(value, "text");
    record.payload_json = ReadString(value, "payload_json");
    record.tags = ReadTags(value);
    record.importance = ReadNumber(value, "importance", record.importance);
    record.confidence = ReadNumber(value, "confidence", record.confidence);

    // 作用域、来源与时间；缺省字段保留 MemoryRecord 默认值
    record.scope = ReadRecordScope(value);
    record.source_session = ReadString(value, "source_session");
    record.source_run = ReadString(value, "source_run");
    record.created_at = ReadInteger(value, "created_at", record.created_at);
    record.updated_at = ReadInteger(value, "updated_at", record.updated_at);
    record.valid_to = ReadInteger(value, "valid_to", record.valid_to);
    record.expires_at = ReadInteger(value, "expires_at", record.expires_at);
    return record;
}

nlohmann::json ScopeToJson(const core::MemoryScope& scope)
{
    nlohmann::json value;
    value["kind"] = scope.kind;
    value["id"] = scope.id;
    return value;
}

nlohmann::json RecordToJson(const core::MemoryRecord& record)
{
    nlohmann::json value;
    value["id"] = record.id;
    value["kind"] = core::ToString(record.kind);
    value["text"] = record.text;
    value["payload_json"] = record.payload_json;
    value["tags"] = record.tags;
    value["importance"] = record.importance;
    value["confidence"] = record.confidence;
    value["scope"] = ScopeToJson(record.scope);
    value["source_session"] = record.source_session;
    value["source_run"] = record.source_run;
    value["created_at"] = record.created_at;
    value["updated_at"] = record.updated_at;
    value["valid_to"] = record.valid_to;
    value["expires_at"] = record.expires_at;
    return value;
}

// 编码失败通常来自 text/payload 中的非法 UTF-8；此时不写盘，内存也不更新。
bool SerializeLine(const nlohmann::json& value, std::string& line, std::string& error)
{
    try
    {
        line = value.dump();
        return true;
    }
    catch (const std::exception& exception)
    {
        error = std::string("failed to encode memory jsonl: ") + exception.what();
        return false;
    }
}

void UpsertRecord(std::vector<core::MemoryRecord>& records, const core::MemoryRecord& record)
{
    const auto existing = std::find_if(records.begin(), records.end(),
                                       [&record](const core::MemoryRecord& item)
                                       {
                                           return item.id == record.id;
                                       });
    if (existing != records.end())
    {
        *existing = record;
        return;
    }
    records.push_back(record);
}

void RemoveRecord(std::vector<core::MemoryRecord>& records, const std::string& id)
{
    const auto found = std::remove_if(records.begin(), records.end(),
                                      [&id](const core::MemoryRecord& record)
                                      {
                                          return record.id == id;
                                      });
    records.erase(found, records.end());
}

void RemoveScopeRecords(std::vector<core::MemoryRecord>& records, const core::MemoryScope& scope)
{
    const auto found = std::remove_if(records.begin(), records.end(),
                                      [&scope](const core::MemoryRecord& record)
                                      {
                                          return record.scope == scope;
                                      });
    records.erase(found, records.end());
}

bool AppendLine(std::ofstream& file, const std::filesystem::path& path, const std::string& line, std::string& error)
{
    file << line << '\n';
    file.flush();
    if (!file.good())
    {
        error = "failed to write memory jsonl: " + path.string();
        return false;
    }
    return true;
}

// 重放一行：按 op 修改内存状态并校验格式；空行已由调用方跳过。
bool ReplayLine(std::vector<core::MemoryRecord>& records, const std::string& line, std::size_t line_number,
                std::string& error)
{
    const std::string prefix = "failed to parse memory jsonl at line " + std::to_string(line_number) + ": ";
    try
    {
        const nlohmann::json parsed = nlohmann::json::parse(line);
        if (!parsed.is_object())
        {
            error = prefix + "line is not a JSON object";
            return false;
        }

        const std::string op = ReadString(parsed, "op");
        if (op == REMEMBER_OP)
        {
            const auto record = parsed.find("record");
            if (record == parsed.end())
            {
                error = prefix + "record is required";
                return false;
            }
            UpsertRecord(records, ReadRecord(*record));
            return true;
        }

        if (op == FORGET_OP)
        {
            const std::string id = ReadString(parsed, "id");
            if (id.empty())
            {
                error = prefix + "forget id is required";
                return false;
            }
            RemoveRecord(records, id);
            return true;
        }

        if (op == CLEAR_SCOPE_OP)
        {
            const auto scope = parsed.find("scope");
            if (scope == parsed.end())
            {
                error = prefix + "scope is required";
                return false;
            }
            RemoveScopeRecords(records, ScopeFromJson(*scope));
            return true;
        }

        error = prefix + "unknown op: " + op;
        return false;
    }
    catch (const std::exception& exception)
    {
        error = prefix + exception.what();
        return false;
    }
}

// 召回规则必须与 InMemoryMemoryStore 完全一致：以下辅助函数与 in_memory_store.cpp 保持同构。
std::string LowerAscii(std::string text)
{
    for (char& c : text)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

bool ContainsIgnoreCase(const std::string& haystack, const std::string& needle)
{
    return LowerAscii(haystack).find(LowerAscii(needle)) != std::string::npos;
}

bool MatchesKind(const std::vector<core::MemoryKind>& kinds, core::MemoryKind kind)
{
    return kinds.empty() || std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}

bool MatchesTags(const std::vector<std::string>& required, const std::vector<std::string>& actual)
{
    if (required.empty())
    {
        return true;
    }
    for (const std::string& tag : required)
    {
        if (std::find(actual.begin(), actual.end(), tag) == actual.end())
        {
            return false;
        }
    }
    return true;
}

bool MatchesScope(const std::vector<core::MemoryScope>& scopes, const core::MemoryScope& scope)
{
    if (scopes.empty())
    {
        return true;
    }
    return std::find(scopes.begin(), scopes.end(), scope) != scopes.end();
}

bool MatchesText(const std::string& query_text, const core::MemoryRecord& record)
{
    if (query_text.empty())
    {
        return true;
    }
    if (ContainsIgnoreCase(record.text, query_text) || ContainsIgnoreCase(record.payload_json, query_text))
    {
        return true;
    }
    // 中文没有空格分词：query 里包含某个 tag 也算命中（Host 应写入有意义的 tags）。
    for (const std::string& tag : record.tags)
    {
        if (!tag.empty() && ContainsIgnoreCase(query_text, tag))
        {
            return true;
        }
    }
    return false;
}

double ScoreHit(bool text_match, const core::MemoryRecord& record)
{
    return (text_match ? 1.0 : 0.0) + record.importance;
}

} // namespace

std::shared_ptr<JsonlMemoryStore> JsonlMemoryStore::Open(const std::filesystem::path& path, std::string& error)
{
    error.clear();
    if (path.empty())
    {
        error = "memory path is required";
        return nullptr;
    }

    // 父目录不存在时先创建，避免首次运行因目录缺失失败。
    const std::filesystem::path directory = path.parent_path();
    if (!directory.empty())
    {
        std::error_code create_error;
        std::filesystem::create_directories(directory, create_error);
        if (create_error)
        {
            error = "failed to create memory directory " + directory.string() + ": " + create_error.message();
            return nullptr;
        }
    }

    // 先重放已有记录；解析失败时不打开追加流，文件保持原样。
    std::vector<core::MemoryRecord> records;
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (input.is_open())
    {
        std::string line;
        std::size_t line_number = 0;
        while (std::getline(input, line))
        {
            ++line_number;
            if (IsBlankLine(line))
            {
                continue;
            }
            if (!ReplayLine(records, line, line_number, error))
            {
                return nullptr;
            }
        }
        if (input.bad())
        {
            error = "failed to read memory jsonl: " + path.string();
            return nullptr;
        }
    }

    std::shared_ptr<JsonlMemoryStore> store(new JsonlMemoryStore(path));
    store->m_records = std::move(records);
    store->m_file.open(path, std::ios::out | std::ios::app | std::ios::binary);
    if (!store->m_file.is_open())
    {
        error = "failed to open memory jsonl for append: " + path.string();
        return nullptr;
    }
    return store;
}

JsonlMemoryStore::JsonlMemoryStore(std::filesystem::path path)
    : m_path(std::move(path))
{}

bool JsonlMemoryStore::Remember(const core::MemoryRecord& record, std::string& error)
{
    if (record.id.empty())
    {
        error = "memory id is required";
        return false;
    }

    nlohmann::json value;
    value["op"] = REMEMBER_OP;
    value["record"] = RecordToJson(record);

    std::string line;
    if (!SerializeLine(value, line, error))
    {
        return false;
    }

    // 先追加成功再更新内存：写失败时内存状态保持不变。
    std::lock_guard lock(m_mutex);
    if (!AppendLine(m_file, m_path, line, error))
    {
        return false;
    }
    UpsertRecord(m_records, record);
    return true;
}

std::vector<core::MemoryHit> JsonlMemoryStore::Recall(const core::MemoryQuery& query)
{
    std::lock_guard lock(m_mutex);
    std::vector<core::MemoryHit> hits;
    for (const core::MemoryRecord& record : m_records)
    {
        if (!MatchesKind(query.kinds, record.kind) || !MatchesTags(query.tags, record.tags) ||
            !MatchesScope(query.scopes, record.scope))
        {
            continue;
        }
        const bool text_match = MatchesText(query.text, record);
        if (!query.text.empty() && !text_match)
        {
            continue;
        }
        hits.push_back({record, ScoreHit(text_match, record)});
    }

    std::sort(hits.begin(), hits.end(),
              [](const core::MemoryHit& left, const core::MemoryHit& right)
              {
                  if (left.score != right.score)
                  {
                      return left.score > right.score;
                  }
                  if (left.record.updated_at != right.record.updated_at)
                  {
                      return left.record.updated_at > right.record.updated_at;
                  }
                  return left.record.id < right.record.id;
              });

    if (query.limit > 0 && hits.size() > query.limit)
    {
        hits.resize(query.limit);
    }

    // 超出字符预算时按排序顺序截断文本，尽量保留高分条目。
    if (query.max_chars > 0)
    {
        std::vector<core::MemoryHit> budgeted;
        std::size_t remaining = query.max_chars;
        for (core::MemoryHit hit : hits)
        {
            if (remaining == 0)
            {
                break;
            }
            if (hit.record.text.size() > remaining)
            {
                hit.record.text.resize(core::Utf8PrefixSize(hit.record.text, remaining));
                remaining = 0;
            }
            else
            {
                remaining -= hit.record.text.size();
            }
            budgeted.push_back(std::move(hit));
        }
        hits = std::move(budgeted);
    }

    return hits;
}

bool JsonlMemoryStore::Forget(const std::string& id, std::string& error)
{
    nlohmann::json value;
    value["op"] = FORGET_OP;
    value["id"] = id;

    std::string line;
    if (!SerializeLine(value, line, error))
    {
        return false;
    }

    // 先追加成功再更新内存：写失败时内存状态保持不变。
    std::lock_guard lock(m_mutex);
    const auto found = std::find_if(m_records.begin(), m_records.end(),
                                    [&id](const core::MemoryRecord& record)
                                    {
                                        return record.id == id;
                                    });
    if (found == m_records.end())
    {
        error = "memory not found: " + id;
        return false;
    }
    if (!AppendLine(m_file, m_path, line, error))
    {
        return false;
    }
    m_records.erase(found);
    return true;
}

bool JsonlMemoryStore::ClearScope(const core::MemoryScope& scope, std::string& error)
{
    nlohmann::json value;
    value["op"] = CLEAR_SCOPE_OP;
    value["scope"] = ScopeToJson(scope);

    std::string line;
    if (!SerializeLine(value, line, error))
    {
        return false;
    }

    // 即使当前没有该 scope 的记录也要落一条 clear_scope，保证重开后不会重新载入旧记录。
    std::lock_guard lock(m_mutex);
    if (!AppendLine(m_file, m_path, line, error))
    {
        return false;
    }
    RemoveScopeRecords(m_records, scope);
    return true;
}

const std::filesystem::path& JsonlMemoryStore::Path() const
{
    return m_path;
}

std::size_t JsonlMemoryStore::Size() const
{
    std::lock_guard lock(m_mutex);
    return m_records.size();
}

std::vector<core::MemoryRecord> JsonlMemoryStore::Records() const
{
    std::lock_guard lock(m_mutex);
    return m_records;
}

} // namespace agent::plugins::memory
