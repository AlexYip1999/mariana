#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "agent/core/agent_system.h"
#include "agent/core/message.h"
#include "agent/core/tool.h"
#include "agent/runtime/agent_profile.h"
#include "agent/runtime/run_context.h"

namespace agent::runtime
{

struct ToolExecutionRecord
{
    // 调用与结果
    core::ToolCall call;
    core::ToolResult result;

    // 调度状态
    RunErrorCode dispatch_error_code = RunErrorCode::None;
    // Submitted to the scheduler; a cancelled/failed dispatch may already have caused side effects.
    bool execution_started = false;
};

/// Executor 的纯输出：不包含 session/run，也不决定历史是否提交。
struct RunOutcome
{
    // 结果
    RunStatus status = RunStatus::Failed;
    RunErrorCode error_code = RunErrorCode::None;
    std::string error;
    std::string content;

    // 执行计数
    int steps = 0;
    std::size_t tool_attempts = 0;

    // 工具操作记录
    std::vector<ToolExecutionRecord> tool_executions;
};

struct TurnRecord
{
    // 关联
    std::string session_id;
    std::string run_id;

    // 结果
    RunStatus status = RunStatus::Failed;
    RunErrorCode error_code = RunErrorCode::None;
    std::string error;

    // 执行计数
    int steps = 0;
    std::size_t tool_attempts = 0;

    // 工具操作记录
    std::vector<ToolExecutionRecord> tool_executions;

    /// 转换为跨层观察事件；字段变化时映射逻辑与记录保持同步。
    core::TurnEvent ToEvent() const;
};

struct AgentRunResult : TurnRecord
{
    std::string content;

    bool Ok() const
    {
        return status == RunStatus::Completed;
    }
};

} // namespace agent::runtime
