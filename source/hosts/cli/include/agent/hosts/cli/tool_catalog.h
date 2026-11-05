#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "agent/core/tool.h"
#include "agent/runtime/tool_descriptor.h"
#include "agent/runtime/tool_registry.h"

namespace agent::hosts::cli
{

/// Host 侧工具目录：唯一工具注册源，负责为某个 workspace 创建 ToolRegistry。
class ToolCatalog
{
  public:
    using Factory = std::function<std::shared_ptr<core::Tool>(const std::filesystem::path&)>;

    bool Add(std::string name, Factory factory, runtime::ToolDescriptor descriptor = {});
    bool Has(const std::string& name) const;

    std::vector<std::string> Names() const;

    std::shared_ptr<runtime::ToolRegistry> CreateRegistry(const std::filesystem::path& workspace,
                                                          std::string& error) const;

  private:
    struct Entry
    {
        std::string name;
        Factory factory;
        runtime::ToolDescriptor descriptor;
    };

    std::vector<Entry> m_entries;
};

/// CLI 内置工具集（当前只有 read_file）。
ToolCatalog DefaultToolCatalog();

} // namespace agent::hosts::cli
