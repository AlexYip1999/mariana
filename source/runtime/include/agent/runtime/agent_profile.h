#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "agent/core/message.h"
#include "agent/core/tool.h"

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

} // namespace agent::runtime
