#include "agent/core/system_registry.h"

#include <utility>

namespace agent::core
{

void SystemRegistry::Add(std::shared_ptr<AgentSystem> system)
{
    if (system != nullptr)
    {
        m_systems.push_back(std::move(system));
    }
}

void SystemRegistry::Clear()
{
    m_systems.clear();
}

std::size_t SystemRegistry::Size() const
{
    return m_systems.size();
}

const std::vector<std::shared_ptr<AgentSystem>>& SystemRegistry::Systems() const
{
    return m_systems;
}

} // namespace agent::core
