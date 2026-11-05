#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "agent/core/tool.h"
#include "agent/runtime/tool_descriptor.h"

namespace agent::runtime
{

/// 工具能力目录：按工具名注册/查找，生成 ToolSpec，并按名字执行。
/// 它本身是一个服务，可注册进 ServiceRegistry；不负责插件加载或模型循环。
class ToolRegistry
{
  public:
    bool Register(std::shared_ptr<core::Tool> tool, std::string& error);
    bool Register(std::shared_ptr<core::Tool> tool, ToolDescriptor descriptor, std::string& error);
    void Clear();

    core::Tool* Find(const std::string& name) const;
    std::shared_ptr<core::Tool> FindShared(const std::string& name) const;
    bool Has(const std::string& name) const;

    const ToolDescriptor* Descriptor(const std::string& name) const;
    std::vector<ToolDescriptor> Descriptors() const;

    std::size_t Size() const;

    std::vector<core::ToolSpec> AllSpecs() const;
    std::vector<core::ToolSpec> SpecsFor(const std::vector<std::string>& names) const;

    core::ToolResult Execute(const std::string& name, const std::string& arguments_json,
                             const core::CallContext& context = {});

  private:
    std::vector<std::shared_ptr<core::Tool>> m_tools;
    std::unordered_map<std::string, std::shared_ptr<core::Tool>> m_tools_by_name;
    std::unordered_map<std::string, ToolDescriptor> m_descriptors_by_name;
};

} // namespace agent::runtime
