#pragma once

#include <string>

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
    bool ok = true;
    std::string content;
    std::string error;
};

/// 工具抽象：具体工具（读文件、查文档、编辑器操作）由宿主或 tools 层实现。
class Tool
{
  public:
    virtual ~Tool() = default;

    virtual std::string Name() const = 0;
    virtual std::string Description() const = 0;
    virtual std::string ParametersJson() const = 0;
    virtual ToolResult Execute(const std::string& arguments_json) = 0;
};

} // namespace agent::core
