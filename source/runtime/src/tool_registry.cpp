#include "agent/runtime/tool_registry.h"

#include <utility>

namespace agent::runtime
{

namespace
{

ToolDescriptor DefaultDescriptor(const core::Tool& tool)
{
    ToolDescriptor descriptor;
    descriptor.name = tool.Name();
    descriptor.description = tool.Description();
    descriptor.side_effects = false;
    descriptor.access_scope = ToolAccessScope::Read;
    descriptor.approval = ToolApprovalMode::NotRequired;
    descriptor.thread_affinity = "worker";
    return descriptor;
}

} // namespace

bool ToolRegistry::Register(std::shared_ptr<core::Tool> tool, std::string& error)
{
    if (tool == nullptr)
    {
        error = "tool is null";
        return false;
    }
    return Register(std::move(tool), DefaultDescriptor(*tool), error);
}

bool ToolRegistry::Register(std::shared_ptr<core::Tool> tool, ToolDescriptor descriptor, std::string& error)
{
    if (tool == nullptr)
    {
        error = "tool is null";
        return false;
    }

    const std::string name = tool->Name();
    if (!core::IsValidToolName(name))
    {
        error = "invalid tool name: " + name;
        return false;
    }
    if (descriptor.name != name)
    {
        error = "tool descriptor name mismatch: " + descriptor.name + " != " + name;
        return false;
    }

    if (m_tools_by_name.find(name) != m_tools_by_name.end())
    {
        error = "duplicate tool name: " + name;
        return false;
    }
    if (descriptor.description.empty())
    {
        descriptor.description = tool->Description();
    }

    m_tools.push_back(tool);
    m_tools_by_name.emplace(name, std::move(tool));
    m_descriptors_by_name.emplace(name, std::move(descriptor));
    return true;
}

void ToolRegistry::Clear()
{
    m_tools.clear();
    m_tools_by_name.clear();
    m_descriptors_by_name.clear();
}

core::Tool* ToolRegistry::Find(const std::string& name) const
{
    return FindShared(name).get();
}

std::shared_ptr<core::Tool> ToolRegistry::FindShared(const std::string& name) const
{
    const auto it = m_tools_by_name.find(name);
    if (it == m_tools_by_name.end())
    {
        return nullptr;
    }
    return it->second;
}

bool ToolRegistry::Has(const std::string& name) const
{
    return m_tools_by_name.find(name) != m_tools_by_name.end();
}

const ToolDescriptor* ToolRegistry::Descriptor(const std::string& name) const
{
    const auto it = m_descriptors_by_name.find(name);
    return it == m_descriptors_by_name.end() ? nullptr : &it->second;
}

std::vector<ToolDescriptor> ToolRegistry::Descriptors() const
{
    std::vector<ToolDescriptor> descriptors;
    descriptors.reserve(m_tools.size());
    for (const std::shared_ptr<core::Tool>& tool : m_tools)
    {
        const ToolDescriptor* descriptor = Descriptor(tool->Name());
        if (descriptor != nullptr)
        {
            descriptors.push_back(*descriptor);
        }
    }
    return descriptors;
}

std::size_t ToolRegistry::Size() const
{
    return m_tools.size();
}

std::vector<core::ToolSpec> ToolRegistry::AllSpecs() const
{
    std::vector<core::ToolSpec> specs;
    specs.reserve(m_tools.size());
    for (const std::shared_ptr<core::Tool>& tool : m_tools)
    {
        specs.push_back(tool->Spec());
    }
    return specs;
}

std::vector<core::ToolSpec> ToolRegistry::SpecsFor(const std::vector<std::string>& names) const
{
    std::vector<core::ToolSpec> specs;
    specs.reserve(names.size());
    for (const std::string& name : names)
    {
        const core::Tool* tool = Find(name);
        if (tool != nullptr)
        {
            specs.push_back(tool->Spec());
        }
    }
    return specs;
}

core::ToolResult ToolRegistry::Execute(const std::string& name, const std::string& arguments_json,
                                       const core::CallContext& context)
{
    core::Tool* tool = Find(name);
    if (tool == nullptr)
    {
        core::ToolResult result;
        result.ok = false;
        result.error = "unknown tool: " + name;
        return result;
    }
    return tool->Execute(arguments_json, context);
}

} // namespace agent::runtime
