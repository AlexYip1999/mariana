#pragma once

#include <string>

#include "agent/core/agent_system.h"
#include "agent/core/chat_model.h"
#include "agent/core/conversation.h"
#include "agent/core/system_registry.h"
#include "agent/runtime/agent_executor.h"
#include "agent/runtime/agent_run.h"
#include "agent/runtime/context_policy.h"
#include "agent/runtime/service_registry.h"
#include "agent/runtime/tool_output_policy.h"
#include "agent/runtime/tool_policy.h"
#include "agent/runtime/tool_registry.h"

namespace agent::runtime
{

/// 默认 ReAct 执行器：模型请求 → tool_calls → 执行工具 → 继续，直到最终回答或 max_steps。
class AgentRunner : public AgentExecutor
{
  public:
    AgentRunner(core::ChatModel& model, ServiceRegistry& services);

    AgentRunResult RunTurn(const AgentProfile& profile, core::Conversation& conversation,
                           const std::string& user_input) override;

  private:
    core::ChatModel& m_model;
    std::shared_ptr<ToolRegistry> m_tools;
    std::shared_ptr<core::SystemRegistry> m_systems;
    std::shared_ptr<ToolPolicy> m_tool_policy;
    std::shared_ptr<ContextPolicy> m_context_policy;
    std::shared_ptr<ToolOutputPolicy> m_tool_output_policy;
};

} // namespace agent::runtime
