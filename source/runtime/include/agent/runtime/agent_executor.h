#pragma once

#include <string>

#include "agent/core/conversation.h"
#include "agent/runtime/agent_run.h"

namespace agent::runtime
{

/// 执行策略接口：一次 Turn 内的 Agent Loop。
class AgentExecutor
{
  public:
    virtual ~AgentExecutor() = default;

    virtual AgentRunResult RunTurn(const AgentProfile& profile, core::Conversation& conversation,
                                   const std::string& user_input) = 0;
};

} // namespace agent::runtime
