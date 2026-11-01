#pragma once

#include <string>
#include <vector>

#include "agent/core/conversation.h"
#include "agent/runtime/agent_executor.h"
#include "agent/runtime/agent_run.h"

namespace agent::runtime
{

using AgentSessionConfig = AgentProfile;

/// 一次 Agent 会话：维护 Conversation，并把每个用户 Turn 交给 AgentExecutor。
class AgentSession
{
  public:
    AgentSession(AgentSessionConfig config, AgentExecutor& executor);

    /// 执行一个用户 Turn；内部可能包含多次模型/工具 Step。
    AgentRunResult Send(const std::string& user_input);
    void ClearHistory();

    const core::Conversation& History() const;

  private:
    void AppendSystemPrompt();

    AgentProfile m_profile;
    AgentExecutor& m_executor;
    core::Conversation m_conversation;
};

} // namespace agent::runtime
