#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "agent/core/agent_system.h"
#include "agent/core/message.h"
#include "agent/core/tool.h"
#include "agent/runtime/run_context.h"

namespace agent::runtime
{

struct AgentProfile
{
    // 模型与提示词
    std::string model;
    std::string system_prompt;
    // 空 = 不启用工具；非空时每个名字都必须存在于 ToolRegistry。
    std::vector<std::string> tool_names;

    // 采样参数
    double temperature = 0.0;
    int max_tokens = 1024;

    // 执行与上下文预算
    int max_steps = 8;
    // 0 = 不裁剪；按字符估算，避免引入 tokenizer 依赖。
    std::size_t max_context_chars = 0;
    std::size_t max_tool_calls = 64;
    std::size_t max_tool_output_bytes = 32 * 1024;
    int turn_timeout_ms = 120000;
};

struct RunOptions
{
    std::stop_token stop_token;
    std::optional<std::chrono::milliseconds> timeout;
};

struct ToolExecutionRecord
{
    core::ToolCall call;
    core::ToolResult result;
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
};

struct AgentRunResult : TurnRecord
{
    std::string content;

    bool Ok() const
    {
        return status == RunStatus::Completed;
    }
};

std::string NewExecutionId(const char* prefix);
RunContext MakeRunContext(const AgentProfile& profile, const std::string& session_id, const RunOptions& options = {});
core::TurnEvent MakeTurnEvent(const TurnRecord& record);

} // namespace agent::runtime
