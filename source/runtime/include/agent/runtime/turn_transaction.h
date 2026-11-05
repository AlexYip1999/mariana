#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "agent/core/conversation.h"
#include "agent/runtime/agent_run.h"

namespace agent::runtime
{

/// 一轮执行的 staged 写入视图；Executor 只写这里，不拥有提交权。
class TurnWorkspace
{
  public:
    TurnWorkspace(core::Conversation& conversation, const RunContext& context);

    core::Conversation& Conversation();
    const core::Conversation& Conversation() const;
    const RunContext& Context() const;

  private:
    core::Conversation& m_conversation;
    const RunContext& m_context;
};

struct TurnCommit
{
    AgentRunResult result;
    // 有值表示本轮历史应提交；空表示回滚。
    std::optional<core::Conversation> history;
};

/// Session 独占的一轮历史事务。
class TurnTransaction
{
  public:
    TurnTransaction(core::Conversation& conversation, const RunContext& context);

    void Start(const AgentProfile& profile, const std::string& input);
    TurnCommit Finish(const RunOutcome& outcome);

  private:
    void RemoveIncompleteBatch();

    core::Conversation& m_conversation;
    const RunContext& m_context;
    std::size_t m_start_size;
};

} // namespace agent::runtime
