#pragma once

#include <string>

#include "agent/core/call_context.h"

namespace agent::core
{

/// 工具的声明信息，用于生成 OpenAI tools 数组。
struct ToolSpec
{
    std::string name;
    std::string description;
    // JSON Schema 字符串；core 不解析，避免依赖具体 JSON 库。
    std::string parameters_json;
};

struct ToolResult
{
    // 结果
    bool ok = true;
    std::string content;

    // 失败原因（ok=false 时由实现填写）
    std::string error;
    CallError error_code = CallError::None;
};

namespace detail
{

inline bool IsToolNameCharacter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

} // namespace detail

/// OpenAI / DeepSeek 的 function name 约束：字母、数字、下划线、短横线，最长 64。
inline bool IsValidToolName(const std::string& name)
{
    if (name.empty() || name.size() > 64)
    {
        return false;
    }

    for (char c : name)
    {
        if (!detail::IsToolNameCharacter(c))
        {
            return false;
        }
    }
    return true;
}

/// 工具抽象：具体工具（读文件、查文档、编辑器操作）由宿主或 tools 层实现。
class Tool
{
  public:
    virtual ~Tool() = default;

    virtual std::string Name() const = 0;
    virtual std::string Description() const = 0;
    virtual std::string ParametersJson() const = 0;
    virtual ToolResult Execute(const std::string& arguments_json, const CallContext& context = {}) = 0;

    /// 由工具元数据生成 ToolSpec，避免每个调用方重复拼装。
    ToolSpec Spec() const
    {
        return ToolSpec{Name(), Description(), ParametersJson()};
    }
};

} // namespace agent::core
