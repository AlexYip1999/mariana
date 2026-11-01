#pragma once

#include <filesystem>

namespace agent::plugins::tools
{

/// 工具共享的 workspace 边界策略：canonical root + 敏感路径判断。
class WorkspacePathPolicy
{
  public:
    explicit WorkspacePathPolicy(std::filesystem::path root);

    bool IsValid() const;
    bool IsWithinWorkspace(const std::filesystem::path& candidate) const;
    bool IsSensitive(const std::filesystem::path& relative_path) const;

    const std::filesystem::path& Root() const;

  private:
    std::filesystem::path m_root;
};

} // namespace agent::plugins::tools
