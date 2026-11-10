#include "agent/plugins/memory/in_memory_store.h"

#include <algorithm>

#include "memory_recall.h"

namespace agent::plugins::memory
{

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
    return detail::RecallRecords(m_records, query);
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
