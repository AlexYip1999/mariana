#include "agent/plugins/memory/jsonl_memory_store.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "memory_recall.h"

namespace agent::plugins::memory
{

namespace
{

constexpr const char* REMEMBER_OP = "remember";
constexpr const char* FORGET_OP = "forget";
constexpr const char* CLEAR_SCOPE_OP = "clear_scope";

// 自动压缩阈值：至少累计这么多次追加写才考虑压缩，避免小规模 Store 频繁重写文件。
constexpr std::size_t MIN_AUTO_COMPACT_OPS = 128;
// 日志行数相对当前记录数的额外放大容忍度：超过 2 * records + 16 行才值得压缩。
constexpr std::size_t AUTO_COMPACT_SLACK_OPS = 16;

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

bool SerializeRememberLine(const core::MemoryRecord& record, std::string& line, std::string& error)
{
    nlohmann::json value;
    value["op"] = REMEMBER_OP;
    value["record"] = RecordToJson(record);
    return SerializeLine(value, line, error);
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

// 删除临时文件；失败不影响主流程（残留文件不参与重放）。
void RemoveFileBestEffort(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::remove(path, error);
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

// 读取文件尾是否带换行：不带换行的最后一行可能是崩溃时只写了一半的撕裂行。
bool FileTailHasNewline(std::ifstream& input)
{
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size <= 0)
    {
        return true;
    }
    input.seekg(size - 1, std::ios::beg);
    char last_character = '\0';
    if (!input.get(last_character))
    {
        return true;
    }
    return last_character == '\n';
}

// 重放结果：尾行缺换行时由 Open 决定是补换行还是用 Compact 重写文件。
struct ReplayResult
{
    bool needs_trailing_newline = false;
    bool torn_tail = false;
};

// 重放整个日志：带换行的非法行按行号报错；只有末尾缺换行的最后一行允许容错。
bool ReplayFile(const std::filesystem::path& path, std::vector<core::MemoryRecord>& records, ReplayResult& result,
                std::string& error)
{
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input.is_open())
    {
        // 文件不存在或不可读：从空状态开始；追加流打开失败会在 Open 里报错。
        return true;
    }

    const bool tail_has_newline = FileTailHasNewline(input);
    input.clear();
    input.seekg(0, std::ios::beg);

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line))
    {
        ++line_number;
        const bool has_newline = !input.eof() || tail_has_newline;
        if (IsBlankLine(line))
        {
            // 空行不参与重放；尾行缺换行时也要补换行，避免与下一次追加粘成一行。
            if (!has_newline)
            {
                result.needs_trailing_newline = true;
            }
            continue;
        }
        if (has_newline)
        {
            if (!ReplayLine(records, line, line_number, error))
            {
                return false;
            }
            continue;
        }

        // 末尾缺换行的最后一行：完整可解析则正常重放并补换行；解析失败视为半写入的撕裂行，
        // 忽略且不报错（由 Open 用 Compact 重写文件清掉残留）。
        std::string replay_error;
        if (ReplayLine(records, line, line_number, replay_error))
        {
            result.needs_trailing_newline = true;
            continue;
        }
        result.torn_tail = true;
    }

    if (input.bad())
    {
        error = "failed to read memory jsonl: " + path.string();
        return false;
    }
    return true;
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

    // 先重放已有记录；带换行的非法行仍报错，撕裂尾行容错见 ReplayFile。
    std::vector<core::MemoryRecord> records;
    ReplayResult replay;
    if (!ReplayFile(path, records, replay, error))
    {
        return nullptr;
    }

    std::shared_ptr<JsonlMemoryStore> store(new JsonlMemoryStore(path));
    store->m_records = std::move(records);
    store->m_file.open(path, std::ios::out | std::ios::app | std::ios::binary);
    if (!store->m_file.is_open())
    {
        error = "failed to open memory jsonl for append: " + path.string();
        return nullptr;
    }

    // 撕裂尾行已忽略但残留在文件里：立即重写，避免后续 append 与半行粘在一起。
    if (replay.torn_tail)
    {
        std::string compact_error;
        if (!store->Compact(compact_error))
        {
            error = "failed to repair memory jsonl: " + compact_error;
            return nullptr;
        }
        return store;
    }

    // 最后一行完整但缺换行：补一个换行，保证后续 append 从新行开始。
    if (replay.needs_trailing_newline)
    {
        store->m_file << '\n';
        store->m_file.flush();
        if (!store->m_file.good())
        {
            error = "failed to write memory jsonl: " + path.string();
            return nullptr;
        }
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

    std::string line;
    if (!SerializeRememberLine(record, line, error))
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
    RecordAppendedOperation();
    return true;
}

std::vector<core::MemoryHit> JsonlMemoryStore::Recall(const core::MemoryQuery& query)
{
    std::lock_guard lock(m_mutex);
    return detail::RecallRecords(m_records, query);
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
    RecordAppendedOperation();
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
    RecordAppendedOperation();
    return true;
}

bool JsonlMemoryStore::Compact(std::string& error)
{
    error.clear();
    std::lock_guard lock(m_mutex);
    return CompactLocked(error);
}

bool JsonlMemoryStore::ReopenAppendFile(std::string& error)
{
    if (m_file.is_open())
    {
        m_file.close();
    }
    m_file.clear();
    m_file.open(m_path, std::ios::out | std::ios::app | std::ios::binary);
    if (!m_file.is_open())
    {
        error = "failed to open memory jsonl for append: " + m_path.string();
        return false;
    }
    return true;
}

// 尽力恢复追加流：压缩中途失败时避免后续写操作全部失败；恢复失败只把原因追加到 error。
void JsonlMemoryStore::TryReopenAppendFile(std::string& error)
{
    std::string reopen_error;
    if (ReopenAppendFile(reopen_error))
    {
        return;
    }
    if (!error.empty())
    {
        error += "; ";
    }
    error += reopen_error;
}

// 把当前记录写成一组 remember 行到临时文件；失败时临时文件由调用方清理。
bool JsonlMemoryStore::WriteSnapshot(const std::filesystem::path& temp_path, std::string& error)
{
    std::ofstream temp_file(temp_path, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!temp_file.is_open())
    {
        error = "failed to write memory jsonl: " + temp_path.string();
        return false;
    }

    for (const core::MemoryRecord& record : m_records)
    {
        std::string line;
        if (!SerializeRememberLine(record, line, error))
        {
            return false;
        }
        temp_file << line << '\n';
    }

    temp_file.flush();
    temp_file.close();
    if (temp_file.fail())
    {
        error = "failed to write memory jsonl: " + temp_path.string();
        return false;
    }
    return true;
}

// 把当前记录重写为 remember 行：临时文件 → 替换原文件 → 重新以 append 打开（调用方需持有 m_mutex）。
bool JsonlMemoryStore::CompactLocked(std::string& error)
{
    std::filesystem::path temp_path = m_path;
    temp_path += ".tmp";

    if (!WriteSnapshot(temp_path, error))
    {
        RemoveFileBestEffort(temp_path);
        return false;
    }

    // Windows 下 rename 不能覆盖已存在的目标文件：先关闭追加流、删除原文件，再改名。
    // 追加流可能已在一次失败的压缩恢复中关闭，此时跳过关闭步骤。
    if (m_file.is_open())
    {
        m_file.close();
        if (m_file.fail())
        {
            error = "failed to close memory jsonl: " + m_path.string();
            TryReopenAppendFile(error);
            RemoveFileBestEffort(temp_path);
            return false;
        }
    }

    std::error_code remove_error;
    std::filesystem::remove(m_path, remove_error);
    if (remove_error)
    {
        error = "failed to replace memory jsonl " + m_path.string() + ": " + remove_error.message();
        TryReopenAppendFile(error);
        RemoveFileBestEffort(temp_path);
        return false;
    }

    std::error_code rename_error;
    std::filesystem::rename(temp_path, m_path, rename_error);
    if (rename_error)
    {
        // 原文件已删除：保留临时文件（含完整状态）便于恢复，并重新打开追加流。
        error = "failed to replace memory jsonl " + m_path.string() + ": " + rename_error.message() +
                "; snapshot kept at " + temp_path.string();
        TryReopenAppendFile(error);
        return false;
    }

    if (!ReopenAppendFile(error))
    {
        return false;
    }

    m_appended_ops = 0;
    return true;
}

// 写操作成功后调用（调用方需持有 m_mutex）：累计操作数，达到阈值时尝试自动压缩。
// 数据已经落盘，自动压缩失败只影响文件体积，不影响本次操作结果，等下次写操作再试。
void JsonlMemoryStore::RecordAppendedOperation()
{
    ++m_appended_ops;
    if (!ShouldAutoCompact())
    {
        return;
    }
    std::string compact_error;
    CompactLocked(compact_error);
}

// 自动压缩阈值：至少累计 MIN_AUTO_COMPACT_OPS 次追加，且日志行数相对当前记录数放大到 2 倍以上。
// 小规模 Store 不会因少量更新反复重写文件。
bool JsonlMemoryStore::ShouldAutoCompact() const
{
    if (m_appended_ops < MIN_AUTO_COMPACT_OPS)
    {
        return false;
    }
    return m_appended_ops >= m_records.size() * 2 + AUTO_COMPACT_SLACK_OPS;
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
