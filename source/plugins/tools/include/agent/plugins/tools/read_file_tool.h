#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "agent/core/tool.h"

namespace agent::plugins::tools
{

/// 读取 workspace 内的文本文件；拒绝绝对路径、越界路径、目录和超限文件。
class ReadFileTool : public core::Tool
{
  public:
    explicit ReadFileTool(std::filesystem::path workspace_root, std::uintmax_t max_read_bytes = 256 * 1024);

    std::string Name() const override;
    std::string Description() const override;
    std::string ParametersJson() const override;
    core::ToolResult Execute(const std::string& arguments_json) override;

  private:
    core::ToolResult ExecuteRead(const std::string& relative_path) const;

    std::filesystem::path m_workspace_root;
    std::uintmax_t m_max_read_bytes = 256 * 1024;
};

} // namespace agent::plugins::tools
