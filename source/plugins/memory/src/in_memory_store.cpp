#include "agent/plugins/memory/in_memory_store.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include "agent/core/utf8.h"

namespace agent::plugins::memory
{

namespace
{

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

bool InMemoryMemoryStore::Remember(const core::MemoryRecord& record, std::string& error)
{
    if (record.id.empty())
    {
        error = "memory id is required";
        return false;
    }
    std::lock_guard lock(m_mutex);
    const auto existing = std::find_if(m_records.begin(), m_records.end(),
                                       [&record](const core::MemoryRecord& item)
                                       {
                                           return item.id == record.id;
                                       });
    if (existing != m_records.end())
    {
        *existing = record;
    }
    else
    {
        m_records.push_back(record);
    }
    return true;
}

std::vector<core::MemoryHit> InMemoryMemoryStore::Recall(const core::MemoryQuery& query)
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

bool InMemoryMemoryStore::Forget(const std::string& id, std::string& error)
{
    std::lock_guard lock(m_mutex);
    const auto found = std::remove_if(m_records.begin(), m_records.end(),
                                      [&id](const core::MemoryRecord& record)
                                      {
                                          return record.id == id;
                                      });
    if (found == m_records.end())
    {
        error = "memory not found: " + id;
        return false;
    }
    m_records.erase(found, m_records.end());
    return true;
}

bool InMemoryMemoryStore::ClearScope(const core::MemoryScope& scope, std::string& error)
{
    (void)error;
    std::lock_guard lock(m_mutex);
    const auto found = std::remove_if(m_records.begin(), m_records.end(),
                                      [&scope](const core::MemoryRecord& record)
                                      {
                                          return record.scope == scope;
                                      });
    m_records.erase(found, m_records.end());
    return true;
}

std::size_t InMemoryMemoryStore::Size() const
{
    std::lock_guard lock(m_mutex);
    return m_records.size();
}

} // namespace agent::plugins::memory
