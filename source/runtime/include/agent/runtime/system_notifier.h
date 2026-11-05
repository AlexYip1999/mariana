#pragma once

#include "agent/core/agent_system.h"
#include "agent/core/system_registry.h"

namespace agent::runtime
{

/// 统一分发 AgentSystem 回调；遍历前快照观察者，异常不改变执行结果。
class SystemNotifier
{
  public:
    explicit SystemNotifier(const core::SystemRegistry& systems)
        : m_systems(systems)
    {}

    void TurnStarted(const core::TurnEvent& event) const;
    void ModelRequested(const core::ChatRequest& request) const;
    void ModelResponded(const core::ChatResponse& response) const;
    void ToolCalled(const core::ToolCall& call) const;
    void ToolFinished(const core::ToolCall& call, const core::ToolResult& result) const;
    void ToolReplayed(const core::ToolCall& call, const core::ToolResult& result) const;
    void TurnFinished(const core::TurnEvent& event) const;

  private:
    template <typename Function> void Notify(Function&& function) const
    {
        // Snapshot so observers may register/unregister during a callback without invalidating iteration.
        const auto systems = m_systems.Systems();
        for (const std::shared_ptr<core::AgentSystem>& system : systems)
        {
            try
            {
                function(*system);
            }
            catch (...)
            {}
        }
    }

    const core::SystemRegistry& m_systems;
};

} // namespace agent::runtime
