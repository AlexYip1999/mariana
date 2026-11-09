#pragma once

#include <cstddef>
#include <string>

#include "agent/core/call_context.h"
#include "agent/runtime/agent_profile.h"

namespace agent::runtime
{

enum class RunStatus
{
    Completed,
    Cancelled,
    Failed,
    BudgetExceeded,
    Busy,
};

inline const char* ToString(RunStatus value)
{
    switch (value)
    {
        case RunStatus::Completed:
        {
            return "Completed";
        }
        case RunStatus::Cancelled:
        {
            return "Cancelled";
        }
        case RunStatus::Failed:
        {
            return "Failed";
        }
        case RunStatus::BudgetExceeded:
        {
            return "BudgetExceeded";
        }
        case RunStatus::Busy:
        {
            return "Busy";
        }
    }
    return "Unknown";
}

/// 一次 Turn 的错误分类；包含调用级错误和 Run 预算/策略/生命周期错误。
enum class RunErrorCode
{
    None,
    Cancelled,
    DeadlineExceeded,
    InvalidInput,
    InvalidConfiguration,
    Busy,
    ModelCallLimit,
    ToolCallLimit,
    ToolOutputLimit,
    ModelFailure,
    IncompleteResponse,
    EmptyResponse,
    ToolDenied,
    ToolFailure,
    InvalidToolCall,
    ContextFailure,
    InternalError,
};

inline const char* ToString(RunErrorCode value)
{
    switch (value)
    {
        case RunErrorCode::None:
        {
            return "None";
        }
        case RunErrorCode::Cancelled:
        {
            return "Cancelled";
        }
        case RunErrorCode::DeadlineExceeded:
        {
            return "DeadlineExceeded";
        }
        case RunErrorCode::InvalidInput:
        {
            return "InvalidInput";
        }
        case RunErrorCode::InvalidConfiguration:
        {
            return "InvalidConfiguration";
        }
        case RunErrorCode::Busy:
        {
            return "Busy";
        }
        case RunErrorCode::ModelCallLimit:
        {
            return "ModelCallLimit";
        }
        case RunErrorCode::ToolCallLimit:
        {
            return "ToolCallLimit";
        }
        case RunErrorCode::ToolOutputLimit:
        {
            return "ToolOutputLimit";
        }
        case RunErrorCode::ModelFailure:
        {
            return "ModelFailure";
        }
        case RunErrorCode::IncompleteResponse:
        {
            return "IncompleteResponse";
        }
        case RunErrorCode::EmptyResponse:
        {
            return "EmptyResponse";
        }
        case RunErrorCode::ToolDenied:
        {
            return "ToolDenied";
        }
        case RunErrorCode::ToolFailure:
        {
            return "ToolFailure";
        }
        case RunErrorCode::InvalidToolCall:
        {
            return "InvalidToolCall";
        }
        case RunErrorCode::ContextFailure:
        {
            return "ContextFailure";
        }
        case RunErrorCode::InternalError:
        {
            return "InternalError";
        }
    }
    return "Unknown";
}

/// 把调用级错误提升为 Run 级错误；唯一映射点。
inline RunErrorCode ToRunError(core::CallError error)
{
    switch (error)
    {
        case core::CallError::None:
        {
            return RunErrorCode::None;
        }
        case core::CallError::Cancelled:
        {
            return RunErrorCode::Cancelled;
        }
        case core::CallError::DeadlineExceeded:
        {
            return RunErrorCode::DeadlineExceeded;
        }
        case core::CallError::ModelFailure:
        {
            return RunErrorCode::ModelFailure;
        }
        case core::CallError::ToolFailure:
        {
            return RunErrorCode::ToolFailure;
        }
    }
    return RunErrorCode::InternalError;
}

struct RunBudget
{
    int max_model_calls = 8;
    std::size_t max_tool_calls = 64;
    std::size_t max_tool_output_bytes = 32 * 1024;
};

/// 一次 Agent Turn 的运行时上下文：身份、预算和调用级取消/deadline。
struct RunContext
{
    // 关联
    std::string session_id;
    std::string run_id;

    // 单次调用上下文
    core::CallContext call;

    // 本轮预算
    RunBudget budget;

    /// 创建 RunContext 的唯一入口；生成 session/run id 与生效预算。
    static RunContext Create(const AgentProfile& profile, const std::string& session_id,
                             const RunOptions& options = {});

    RunErrorCode Interruption() const
    {
        return ToRunError(call.Interruption());
    }

    int RemainingTimeoutMs(int request_timeout_ms) const
    {
        return call.RemainingTimeoutMs(request_timeout_ms);
    }
};

} // namespace agent::runtime
