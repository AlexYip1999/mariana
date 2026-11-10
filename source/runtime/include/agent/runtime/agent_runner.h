#pragma once

#include <memory>
#include <optional>
#include <string>

#include "agent/core/agent_system.h"
#include "agent/core/chat_model.h"
#include "agent/core/context.h"
#include "agent/core/conversation.h"
#include "agent/core/memory.h"
#include "agent/core/system_registry.h"
#include "agent/runtime/agent_executor.h"
#include "agent/runtime/agent_run.h"
#include "agent/runtime/context_policy.h"
#include "agent/runtime/memory_policy.h"
#include "agent/runtime/tool_dispatcher.h"
#include "agent/runtime/tool_output_policy.h"
#include "agent/runtime/tool_policy.h"
#include "agent/runtime/tool_registry.h"
#include "agent/runtime/validator.h"

namespace agent::runtime
{

/// The composition root owns defaults and validation choices. Runner never queries a service locator.
struct RunnerDependencies
{
    // 模型与工具
    std::shared_ptr<core::ChatModel> model;
    std::shared_ptr<const ToolRegistry> tools;

    // 策略
    std::shared_ptr<const ToolPolicy> tool_policy;
    std::shared_ptr<const ContextPolicy> context_policy;
    std::shared_ptr<const ToolOutputPolicy> tool_output_policy;

    // 宿主线程与观察
    std::shared_ptr<ToolScheduler> tool_scheduler;
    std::shared_ptr<core::SystemRegistry> systems;

    // 可选注入：宿主上下文 / 长期记忆召回 / 会话压缩 / 输出契约校验
    /// 可选：每轮模型调用前构建宿主上下文（世界快照/检索）；为空表示不注入。
    std::shared_ptr<core::ContextProvider> context_provider;
    /// 可选：长期记忆召回（由 AgentProfile::memory_policy 控制是否启用）；为空表示不召回。
    std::shared_ptr<core::MemoryStore> memory;
    /// 可选：会话压缩（历史超阈值时由 Session 调用）；为空表示不压缩。
    std::shared_ptr<core::Summarizer> summarizer;
    /// 可选：最终回答写入对话前校验输出契约；为空表示不校验。
    std::shared_ptr<Validator> validator;
};

/// 默认 ReAct 执行器：模型请求 → tool_calls → 执行工具 → 继续，直到最终回答或 max_steps。
class AgentRunner : public AgentExecutor
{
  public:
    AgentRunner(RunnerDependencies dependencies);

    RunOutcome RunTurn(const AgentProfile& profile, TurnWorkspace& workspace, const std::string& user_input,
                       const RunContext& context) override;

    std::optional<std::string> ValidationError(const AgentProfile& profile) const override;

    std::shared_ptr<core::SystemRegistry> SystemRegistry() const override;

    std::shared_ptr<core::Summarizer> Summarizer() const override;

  private:
    RunnerDependencies m_dependencies;
};

} // namespace agent::runtime
