#pragma once

#include <string>

#include "agent/core/memory.h"

namespace agent::plugins::memory
{

/// 空实现：用于未启用记忆的宿主。写操作静默成功，召回永远为空。
class NoopMemoryStore final : public core::MemoryStore
{
  public:
    bool Remember(const core::MemoryRecord&, std::string&) override
    {
        return true;
    }

    std::vector<core::MemoryHit> Recall(const core::MemoryQuery&) override
    {
        return {};
    }

    bool Forget(const std::string&, std::string&) override
    {
        return true;
    }

    bool ClearScope(const core::MemoryScope&, std::string&) override
    {
        return true;
    }
};

} // namespace agent::plugins::memory
