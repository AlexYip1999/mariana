#pragma once

#include <memory>
#include <vector>

#include "agent/core/system_registry.h"
#include "agent/runtime/agent_run.h"
#include "agent/runtime/run_context.h"
#include "agent/runtime/system_notifier.h"
#include "agent/runtime/tool_output_policy.h"
#include "agent/runtime/tool_policy.h"
#include "agent/runtime/tool_registry.h"

namespace agent::runtime
{

/// A host may marshal execution to its main thread. It must honor context while waiting.
class ToolScheduler
{
  public:
    virtual ~ToolScheduler() = default;
    virtual core::ToolResult Execute(std::shared_ptr<core::Tool> tool, const std::string& arguments,
                                     const core::CallContext& context) = 0;
};

class InlineToolScheduler final : public ToolScheduler
{
  public:
    core::ToolResult Execute(std::shared_ptr<core::Tool> tool, const std::string& arguments,
                             const core::CallContext& context) override
    {
        return tool->Execute(arguments, context);
    }
};

/// ToolResult 只带调用级错误；Run 级错误由 dispatch_error_code 单独表达。
struct ToolDispatchResult
{
    core::ToolResult result;
    RunErrorCode error_code = RunErrorCode::None;
};

/// Per-turn dispatcher; keeps operation records independently of conversation rollback.
class ToolDispatcher
{
  public:
    ToolDispatcher(const ToolRegistry& tools, const ToolPolicy& policy, const ToolOutputPolicy& output_policy,
                   ToolScheduler& scheduler, const SystemNotifier& notifier, const RunContext& context);

    ToolDispatchResult Execute(const core::ToolCall& call, const std::vector<std::string>& allowed,
                               RunOutcome& run_outcome);

  private:
    const ToolRegistry& m_tools;
    const ToolPolicy& m_policy;
    const ToolOutputPolicy& m_output_policy;
    ToolScheduler& m_scheduler;
    const SystemNotifier& m_notifier;
    const RunContext& m_context;
    ToolOutputBudget m_output_budget;
};

} // namespace agent::runtime
