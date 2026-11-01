#pragma once

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
};

struct AgentRunResult
{
    bool ok = false;
    std::string content;
    std::string error;
    int steps = 0;
};

} // namespace agent::runtime
