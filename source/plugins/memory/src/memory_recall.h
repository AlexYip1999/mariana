#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "agent/core/memory.h"
#include "agent/core/utf8.h"

// 内部共享头：InMemoryMemoryStore 与 JsonlMemoryStore 的召回规则必须逐字一致，统一实现在这里。
// 本文件只被同目录 .cpp 直接 include，不安装到 public include。
namespace agent::plugins::memory::detail
{

inline std::string LowerAscii(std::string text)
{
    for (char& c : text)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

inline bool ContainsIgnoreCase(const std::string& haystack, const std::string& needle)
{
    return LowerAscii(haystack).find(LowerAscii(needle)) != std::string::npos;
}

inline bool MatchesKind(const std::vector<core::MemoryKind>& kinds, core::MemoryKind kind)
{
    return kinds.empty() || std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}

inline bool MatchesTags(const std::vector<std::string>& required, const std::vector<std::string>& actual)
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

inline bool MatchesScope(const std::vector<core::MemoryScope>& scopes, const core::MemoryScope& scope)
{
    if (scopes.empty())
    {
        return true;
    }
    return std::find(scopes.begin(), scopes.end(), scope) != scopes.end();
}

inline bool MatchesText(const std::string& query_text, const core::MemoryRecord& record)
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

inline double ScoreHit(bool text_match, const core::MemoryRecord& record)
{
    return (text_match ? 1.0 : 0.0) + record.importance;
}

// kinds / tags / scopes 过滤是 AND 关系：任一项不满足就不召回。
inline bool MatchesFilters(const core::MemoryQuery& query, const core::MemoryRecord& record)
{
    return MatchesKind(query.kinds, record.kind) && MatchesTags(query.tags, record.tags) &&
           MatchesScope(query.scopes, record.scope);
}

// 排序：文本命中优先（score 已含 importance），其次 updated_at、id，保证结果确定性。
inline bool IsHitBefore(const core::MemoryHit& left, const core::MemoryHit& right)
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
}

// 超出字符预算时按排序顺序截断文本，尽量保留高分条目；截断落在 UTF-8 边界。
inline void ApplyCharBudget(std::vector<core::MemoryHit>& hits, std::size_t max_chars)
{
    if (max_chars == 0)
    {
        return;
    }

    std::vector<core::MemoryHit> budgeted;
    std::size_t remaining = max_chars;
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

inline std::vector<core::MemoryHit> RecallRecords(const std::vector<core::MemoryRecord>& records,
                                                  const core::MemoryQuery& query)
{
    // 过滤与打分：text 为空时只按 kinds/tags/scopes 过滤，命中项统一得 importance + 1.0。
    std::vector<core::MemoryHit> hits;
    for (const core::MemoryRecord& record : records)
    {
        if (!MatchesFilters(query, record))
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

    std::sort(hits.begin(), hits.end(), IsHitBefore);
    if (query.limit > 0 && hits.size() > query.limit)
    {
        hits.resize(query.limit);
    }

    ApplyCharBudget(hits, query.max_chars);
    return hits;
}

} // namespace agent::plugins::memory::detail
