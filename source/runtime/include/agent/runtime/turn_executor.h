#pragma once

#include <optional>
#include <string>

#include "agent/core/conversation.h"
#include "agent/core/system_registry.h"
#include "agent/runtime/agent_executor.h"
#include "agent/runtime/agent_run.h"
#include "agent/runtime/run_context.h"

namespace agent::runtime
{

struct TurnExecution
{
    AgentRunResult result;
    // 有值表示本轮历史应提交；空表示回滚。
    std::optional<core::Conversation> history;
};

/// Session 与测试共用的 Turn 执行协议：事务、事件、executor 调用顺序只在这里维护。
TurnExecution ExecuteTurn(const AgentProfile& profile, core::Conversation& staged, AgentExecutor& executor,
                          const std::string& user_input, const RunContext& context, core::SystemRegistry& systems);

} // namespace agent::runtime
