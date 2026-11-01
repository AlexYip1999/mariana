#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "agent/core/agent_system.h"

namespace agent::core
{

/// 只负责注册 loop systems；Runner 在每个阶段遍历所有 system。
class SystemRegistry
{
  public:
    void Add(std::shared_ptr<AgentSystem> system);
    void Clear();

    std::size_t Size() const;

    const std::vector<std::shared_ptr<AgentSystem>>& Systems() const;

  private:
    std::vector<std::shared_ptr<AgentSystem>> m_systems;
};

} // namespace agent::core
