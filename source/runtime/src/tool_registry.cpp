#include "agent/runtime/tool_registry.h"

#include <utility>

namespace agent::runtime
{

bool ToolRegistry::Register(std::shared_ptr<core::Tool> tool, std::string& error)
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

    if (m_tools_by_name.find(name) != m_tools_by_name.end())
    {
        error = "duplicate tool name: " + name;
        return false;
    }

    m_tools.push_back(tool);
    m_tools_by_name.emplace(name, std::move(tool));
    return true;
}

void ToolRegistry::Clear()
{
    m_tools.clear();
    m_tools_by_name.clear();
}

core::Tool* ToolRegistry::Find(const std::string& name) const
{
    const auto it = m_tools_by_name.find(name);
    if (it == m_tools_by_name.end())
    {
        return nullptr;
    }
    return it->second.get();
}

bool ToolRegistry::Has(const std::string& name) const
{
    return m_tools_by_name.find(name) != m_tools_by_name.end();
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

core::ToolResult ToolRegistry::Execute(const std::string& name, const std::string& arguments_json)
{
    core::Tool* tool = Find(name);
    if (tool == nullptr)
    {
        core::ToolResult result;
        result.ok = false;
        result.error = "unknown tool: " + name;
        return result;
    }
    return tool->Execute(arguments_json);
}

} // namespace agent::runtime
