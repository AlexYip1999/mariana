#pragma once

#include <string>

#include "agent/core/agent_system.h"
#include "agent/core/chat_model.h"
#include "agent/core/conversation.h"
#include "agent/core/system_registry.h"
#include "agent/runtime/agent_executor.h"
#include "agent/runtime/agent_run.h"
#include "agent/runtime/tool_registry.h"

namespace agent::runtime
{

/// 默认 ReAct 执行器：模型请求 → tool_calls → 执行工具 → 继续，直到最终回答或 max_steps。
class AgentRunner : public AgentExecutor
{
  public:
    AgentRunner(core::ChatModel& model, ToolRegistry* tools, core::SystemRegistry* systems = nullptr);

    AgentRunResult RunTurn(const AgentProfile& profile, core::Conversation& conversation,
                           const std::string& user_input) override;

  private:
    core::ChatModel& m_model;
    ToolRegistry* m_tools = nullptr;
    core::SystemRegistry* m_systems = nullptr;
};

} // namespace agent::runtime
