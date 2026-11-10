#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "agent/core/tool.h"
#include "agent/plugins/tools/workspace_path_policy.h"

namespace agent::plugins::tools
{

/// 列出 workspace 内某个目录的条目；只读，拒绝绝对路径、越界路径与敏感路径。
class ListDirTool : public core::Tool
{
  public:
    explicit ListDirTool(std::filesystem::path workspace_root, std::size_t max_entries = 200);

    std::string Name() const override;
    std::string Description() const override;
    std::string ParametersJson() const override;
    core::ToolResult Execute(const std::string& arguments_json, const core::CallContext& context = {}) override;

  private:
    WorkspacePathPolicy m_policy;

    // 单次最多展示的条目数
    std::size_t m_max_entries = 200;
};

} // namespace agent::plugins::tools
