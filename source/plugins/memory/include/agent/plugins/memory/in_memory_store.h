#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "agent/core/memory.h"

namespace agent::plugins::memory
{

/// 进程内实现：仅用于测试、CLI 演示和 UE 早期接入；持久化由 Host 负责。
/// 召回规则是确定性的：文本命中优先，其次 importance、updated_at、id。
class InMemoryMemoryStore final : public core::MemoryStore
{
  public:
    bool Remember(const core::MemoryRecord& record, std::string& error) override;
    std::vector<core::MemoryHit> Recall(const core::MemoryQuery& query) override;
    bool Forget(const std::string& id, std::string& error) override;
    bool ClearScope(const core::MemoryScope& scope, std::string& error) override;

    std::size_t Size() const;

  private:
    mutable std::mutex m_mutex;
    std::vector<core::MemoryRecord> m_records;
};

} // namespace agent::plugins::memory
