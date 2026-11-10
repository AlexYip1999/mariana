#pragma once

#include <memory>
#include <optional>
#include <string>

#include "agent/core/conversation.h"
#include "agent/core/summarizer.h"
#include "agent/core/system_registry.h"
#include "agent/runtime/agent_run.h"
#include "agent/runtime/turn_transaction.h"

namespace agent::runtime
{

/// 执行策略接口：只负责一次 Turn 的模型/工具循环，不拥有历史提交权。
class AgentExecutor
{
  public:
    virtual ~AgentExecutor() = default;

    virtual RunOutcome RunTurn(const AgentProfile& profile, TurnWorkspace& workspace, const std::string& user_input,
                               const RunContext& context) = 0;

    /// Session 在开始 Turn 前调用的组合校验；返回空表示可以开始。
    virtual std::optional<std::string> ValidationError(const AgentProfile&) const
    {
        return std::nullopt;
    }

    /// 执行器实际使用的观察者注册表；仅用于组合期一致性校验，可为空。
    virtual std::shared_ptr<core::SystemRegistry> SystemRegistry() const
    {
        return nullptr;
    }

    /// 可选的会话压缩实现；Session 在历史超过阈值时调用，空表示不压缩。
    virtual std::shared_ptr<core::Summarizer> Summarizer() const
    {
        return nullptr;
    }
};

} // namespace agent::runtime
