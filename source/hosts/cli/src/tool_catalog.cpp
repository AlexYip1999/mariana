#include "agent/hosts/cli/tool_catalog.h"

#include <exception>
#include <string>
#include <utility>

#include "agent/plugins/tools/list_dir_tool.h"
#include "agent/plugins/tools/read_file_tool.h"
#include "agent/plugins/tools/search_text_tool.h"

namespace agent::hosts::cli
{

namespace
{

runtime::ToolDescriptor MakeReadToolDescriptor(std::string name, std::string description)
{
    runtime::ToolDescriptor descriptor;
    descriptor.name = std::move(name);
    descriptor.description = std::move(description);
    descriptor.side_effects = false;
    descriptor.access_scope = runtime::ToolAccessScope::Read;
    descriptor.approval = runtime::ToolApprovalMode::NotRequired;
    descriptor.thread_affinity = "worker";
    return descriptor;
}

} // namespace

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
    (void)catalog.Add(
        "read_file",
        [](const std::filesystem::path& workspace)
        {
            return std::make_shared<plugins::tools::ReadFileTool>(workspace);
        },
        MakeReadToolDescriptor("read_file", "Read a UTF-8 text file inside the workspace."));
    (void)catalog.Add(
        "list_dir",
        [](const std::filesystem::path& workspace)
        {
            return std::make_shared<plugins::tools::ListDirTool>(workspace);
        },
        MakeReadToolDescriptor("list_dir", "List directory entries inside the workspace."));
    (void)catalog.Add(
        "search_text",
        [](const std::filesystem::path& workspace)
        {
            return std::make_shared<plugins::tools::SearchTextTool>(workspace);
        },
        MakeReadToolDescriptor("search_text", "Search a case-sensitive substring in workspace text files."));
    return catalog;
}

} // namespace agent::hosts::cli
