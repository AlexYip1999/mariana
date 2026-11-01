#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace agent::runtime
{

struct AgentProfile
{
    std::string model;
    std::string system_prompt;
    // 空 = 不启用工具；非空时每个名字都必须存在于 ToolRegistry。
    std::vector<std::string> tool_names;
    double temperature = 0.0;
    int max_tokens = 1024;
    int max_steps = 8;
    // 0 = 不裁剪；按字符估算，避免引入 tokenizer 依赖。
    std::size_t max_context_chars = 0;
};

struct AgentRunResult
{
    bool ok = false;
    std::string content;
    std::string error;
    int steps = 0;
};

} // namespace agent::runtime
