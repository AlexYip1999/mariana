#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "agent/core/conversation.h"
#include "agent/core/system_registry.h"
#include "agent/runtime/agent_executor.h"
#include "agent/runtime/agent_run.h"

namespace agent::runtime
{

using AgentSessionConfig = AgentProfile;

/// 一次 Agent 会话：维护 Conversation，并把每个用户 Turn 交给 AgentExecutor。
class AgentSession
{
  public:
    AgentSession(AgentSessionConfig config, std::shared_ptr<AgentExecutor> executor,
                 std::string session_id = std::string(), std::shared_ptr<core::SystemRegistry> systems = nullptr,
                 std::size_t max_records = 128);

    /// 执行一个用户 Turn；内部可能包含多次模型/工具 Step。
    AgentRunResult Send(const std::string& user_input, const RunOptions& options = {});
    AgentRunResult Send(const std::string& user_input, std::stop_token stop_token);
    bool ClearHistory();

    core::Conversation History() const;
    AgentProfile Profile() const;
    std::shared_ptr<AgentExecutor> Executor() const;
    std::vector<TurnRecord> Records() const;
    const std::string& Id() const;

    /// 切换 profile；系统提示变更会替换历史中的 System 消息，保留对话轮次。
    bool SetProfile(AgentProfile profile);
    bool Reconfigure(AgentProfile profile, std::shared_ptr<AgentExecutor> executor);

  private:
    void AppendSystemPrompt();

    AgentProfile m_profile;
    std::shared_ptr<AgentExecutor> m_executor;
    const std::string m_session_id;
    std::shared_ptr<core::SystemRegistry> m_systems;
    const std::size_t m_max_records;
    mutable std::mutex m_state_mutex;
    std::atomic<bool> m_running{false};
    core::Conversation m_conversation;
    std::vector<TurnRecord> m_records;
};

} // namespace agent::runtime
