#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "agent/core/tool.h"
#include "agent/plugins/tools/workspace_path_policy.h"

namespace agent::plugins::tools
{

/// 在 workspace 内的文本文件中做大小写敏感子串搜索；只读。
class SearchTextTool : public core::Tool
{
  public:
    explicit SearchTextTool(std::filesystem::path workspace_root, std::size_t max_file_bytes = 256 * 1024,
                            std::size_t max_results = 50, std::size_t max_files = 500);

    std::string Name() const override;
    std::string Description() const override;
    std::string ParametersJson() const override;
    core::ToolResult Execute(const std::string& arguments_json, const core::CallContext& context = {}) override;

  private:
    WorkspacePathPolicy m_policy;

    // 扫描限制：单文件大小、命中行数、扫描文件数
    std::size_t m_max_file_bytes = 256 * 1024;
    std::size_t m_max_results = 50;
    std::size_t m_max_files = 500;
};

} // namespace agent::plugins::tools
