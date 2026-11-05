#include "agent/hosts/cli/tool_catalog.h"

#include <exception>
#include <utility>

#include "agent/plugins/tools/read_file_tool.h"

namespace agent::hosts::cli
{

bool ToolCatalog::Add(std::string name, Factory factory, runtime::ToolDescriptor descriptor)
{
    if (name.empty() || !factory || Has(name))
    {
        return false;
    }
    if (descriptor.name.empty())
    {
        descriptor.name = name;
    }
    if (descriptor.name != name)
    {
        return false;
    }
    m_entries.push_back({std::move(name), std::move(factory), std::move(descriptor)});
    return true;
}

bool ToolCatalog::Has(const std::string& name) const
{
    for (const Entry& entry : m_entries)
    {
        if (entry.name == name)
        {
            return true;
        }
    }
    return false;
}

std::vector<std::string> ToolCatalog::Names() const
{
    std::vector<std::string> names;
    names.reserve(m_entries.size());
    for (const Entry& entry : m_entries)
    {
        names.push_back(entry.name);
    }
    return names;
}

std::shared_ptr<runtime::ToolRegistry> ToolCatalog::CreateRegistry(const std::filesystem::path& workspace,
                                                                   std::string& error) const
{
    auto registry = std::make_shared<runtime::ToolRegistry>();
    for (const Entry& entry : m_entries)
    {
        std::shared_ptr<core::Tool> tool;
        try
        {
            tool = entry.factory(workspace);
        }
        catch (const std::exception& exception)
        {
            error = "cannot create tool '" + entry.name + "': " + exception.what();
            return nullptr;
        }
        catch (...)
        {
            error = "cannot create tool '" + entry.name + "'";
            return nullptr;
        }
        if (!registry->Register(std::move(tool), entry.descriptor, error))
        {
            return nullptr;
        }
    }
    return registry;
}

ToolCatalog DefaultToolCatalog()
{
    ToolCatalog catalog;
    runtime::ToolDescriptor read_file;
    read_file.name = "read_file";
    read_file.description = "Read a UTF-8 text file inside the workspace.";
    read_file.side_effects = false;
    read_file.access_scope = runtime::ToolAccessScope::Read;
    read_file.approval = runtime::ToolApprovalMode::NotRequired;
    read_file.thread_affinity = "worker";
    (void)catalog.Add(
        "read_file",
        [](const std::filesystem::path& workspace)
        {
            return std::make_shared<plugins::tools::ReadFileTool>(workspace);
        },
        std::move(read_file));
    return catalog;
}

} // namespace agent::hosts::cli
