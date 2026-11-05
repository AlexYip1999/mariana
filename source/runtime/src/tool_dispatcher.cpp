#include "agent/runtime/tool_dispatcher.h"

#include <algorithm>
#include <exception>

namespace agent::runtime
{

namespace
{

ToolDispatchResult Failure(RunErrorCode code, std::string message)
{
    ToolDispatchResult dispatch;
    dispatch.result.ok = false;
    dispatch.result.error_code = core::CallError::ToolFailure;
    dispatch.result.error = std::move(message);
    dispatch.error_code = code;
    return dispatch;
}

void SetFailure(ToolExecutionRecord& execution, RunErrorCode code, std::string message)
{
    execution.dispatch_error_code = code;
    execution.result.ok = false;
    execution.result.error_code = core::CallError::ToolFailure;
    execution.result.error = std::move(message);
}

void NotifyToolCall(const SystemNotifier& notifier, const core::ToolCall& call)
{
    notifier.ToolCalled(call);
}

void NotifyToolResult(const SystemNotifier& notifier, const core::ToolCall& call, const core::ToolResult& result)
{
    notifier.ToolFinished(call, result);
}

} // namespace

ToolDispatcher::ToolDispatcher(const ToolRegistry& tools, const ToolPolicy& policy,
                               const ToolOutputPolicy& output_policy, ToolScheduler& scheduler,
                               const SystemNotifier& notifier, const RunContext& context)
    : m_tools(tools)
    , m_policy(policy)
    , m_output_policy(output_policy)
    , m_scheduler(scheduler)
    , m_notifier(notifier)
    , m_context(context)
{
    m_output_budget.max_bytes = context.budget.max_tool_output_bytes;
}

ToolDispatchResult ToolDispatcher::Execute(const core::ToolCall& call, const std::vector<std::string>& allowed,
                                           RunOutcome& run_outcome)
{
    if (run_outcome.tool_attempts >= m_context.budget.max_tool_calls)
    {
        return Failure(RunErrorCode::ToolCallLimit, "tool call budget exceeded");
    }
    ++run_outcome.tool_attempts;
    if (m_context.Interruption() != RunErrorCode::None)
    {
        return Failure(m_context.Interruption(), "tool execution interrupted");
    }
    if (call.id.empty() || !core::IsValidToolName(call.name))
    {
        return Failure(RunErrorCode::InvalidToolCall, "invalid tool call id or name");
    }
    for (const auto& previous : run_outcome.tool_executions)
    {
        if (previous.call.id == call.id)
        {
            const bool matches = previous.call.name == call.name && previous.call.arguments_json == call.arguments_json;
            if (!matches)
            {
                return Failure(RunErrorCode::InvalidToolCall, "tool call id reused with different arguments");
            }
            if (m_output_budget.used_bytes >= m_output_budget.max_bytes)
            {
                return Failure(RunErrorCode::ToolOutputLimit, "tool output budget exhausted");
            }
            auto cached = previous.result;
            auto& output = cached.ok ? cached.content : cached.error;
            // Keep the operation result intact if a custom output policy throws.
            output = m_output_policy.Limit(call, output, m_output_budget);
            m_notifier.ToolReplayed(call, cached);
            return {cached, previous.dispatch_error_code};
        }
    }
    if (m_output_budget.used_bytes >= m_output_budget.max_bytes)
    {
        return Failure(RunErrorCode::ToolOutputLimit, "tool output budget exhausted");
    }

    ToolExecutionRecord record;
    record.call = call;
    // Allocate the record before entering a tool: exceptions after entry must not erase the operation.
    run_outcome.tool_executions.push_back(std::move(record));
    auto& execution = run_outcome.tool_executions.back();
    NotifyToolCall(m_notifier, call);

    const auto tool = m_tools.FindShared(call.name);
    std::string deny_reason;
    try
    {
        if (m_context.Interruption() != RunErrorCode::None)
        {
            SetFailure(execution, m_context.Interruption(), "tool execution interrupted");
        }
        else if (!tool || std::find(allowed.begin(), allowed.end(), call.name) == allowed.end())
        {
            SetFailure(execution, RunErrorCode::ToolDenied, "tool not allowed: " + call.name);
        }
        else if (!m_policy.Allow(call, deny_reason))
        {
            SetFailure(execution, RunErrorCode::ToolDenied, "tool denied by policy: " + deny_reason);
        }
        else if (m_context.Interruption() != RunErrorCode::None)
        {
            SetFailure(execution, m_context.Interruption(), "tool execution interrupted");
        }
        else
        {
            execution.execution_started = true;
            execution.result = m_scheduler.Execute(tool, call.arguments_json, m_context.call);
            if (!execution.result.ok)
            {
                execution.dispatch_error_code = ToRunError(execution.result.error_code);
                if (execution.dispatch_error_code == RunErrorCode::None)
                {
                    execution.dispatch_error_code = RunErrorCode::ToolFailure;
                    execution.result.error_code = core::CallError::ToolFailure;
                }
            }
        }
    }
    catch (const std::exception& error)
    {
        SetFailure(execution, RunErrorCode::ToolFailure, std::string("tool execution failed: ") + error.what());
    }
    catch (...)
    {
        SetFailure(execution, RunErrorCode::ToolFailure, "tool execution failed");
    }

    std::string& output = execution.result.ok ? execution.result.content : execution.result.error;
    // Keep the operation result intact if a custom output policy throws.
    output = m_output_policy.Limit(call, output, m_output_budget);
    NotifyToolResult(m_notifier, call, execution.result);
    return {execution.result, execution.dispatch_error_code};
}

} // namespace agent::runtime
