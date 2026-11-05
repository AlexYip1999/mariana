#include "agent/runtime/composition.h"

#include <algorithm>
#include <set>
#include <sstream>

namespace agent::runtime
{

namespace
{

bool IsSupportedThreadAffinity(const std::string& affinity)
{
    return affinity.empty() || affinity == "inline" || affinity == "worker" || affinity == "host";
}

} // namespace

CompositionValidation ValidateComposition(const RunnerDependencies& dependencies, const AgentProfile& profile)
{
    CompositionValidation validation;
    const auto add = [&validation](std::string field, std::string message)
    {
        validation.issues.push_back({std::move(field), std::move(message)});
    };

    if (!dependencies.model)
    {
        add("model", "ChatModel is required");
    }
    if (!dependencies.tools)
    {
        add("tools", "ToolRegistry is required");
    }
    if (!dependencies.tool_policy)
    {
        add("tool_policy", "ToolPolicy is required");
    }
    if (!dependencies.context_policy)
    {
        add("context_policy", "ContextPolicy is required");
    }
    if (!dependencies.tool_output_policy)
    {
        add("tool_output_policy", "ToolOutputPolicy is required");
    }
    if (!dependencies.tool_scheduler)
    {
        add("tool_scheduler", "ToolScheduler is required");
    }
    if (!dependencies.systems)
    {
        add("systems", "SystemRegistry is required");
    }
    if (!validation.Ok() || !dependencies.tools)
    {
        return validation;
    }

    std::set<std::string> seen;
    for (const std::string& name : profile.tool_names)
    {
        if (!seen.insert(name).second)
        {
            add("tool_names", "duplicate tool name: " + name);
            continue;
        }
        if (!dependencies.tools->Has(name))
        {
            add("tool_names", "unknown tool in profile: " + name);
            continue;
        }
        const ToolDescriptor* descriptor = dependencies.tools->Descriptor(name);
        if (descriptor == nullptr)
        {
            add("tools", "missing descriptor for tool: " + name);
            continue;
        }
        if (!IsSupportedThreadAffinity(descriptor->thread_affinity))
        {
            add("thread_affinity",
                "unsupported thread_affinity '" + descriptor->thread_affinity + "' for tool " + name);
        }
        if (descriptor->side_effects || descriptor->access_scope != ToolAccessScope::Read ||
            descriptor->approval != ToolApprovalMode::NotRequired)
        {
            add("approval", "side-effecting or approval-required tool is not executable yet: " + name);
        }
    }
    return validation;
}

std::string DescribeCompositionIssues(const CompositionValidation& validation)
{
    std::ostringstream stream;
    for (std::size_t index = 0; index < validation.issues.size(); ++index)
    {
        if (index != 0)
        {
            stream << "; ";
        }
        stream << validation.issues[index].field << ": " << validation.issues[index].message;
    }
    return stream.str();
}

} // namespace agent::runtime
