#include "agent/runtime/tool_output_policy.h"

#include <cstddef>
#include <string>
#include <utility>

namespace agent::runtime
{

std::string TruncateToolOutputPolicy::Limit(const core::ToolCall&, std::string content, ToolOutputBudget& budget) const
{
    if (budget.used_bytes >= budget.max_bytes)
    {
        return "[truncated: tool output budget exhausted]";
    }

    const std::size_t remaining = budget.max_bytes - budget.used_bytes;
    if (content.size() > remaining)
    {
        content.resize(remaining);
        content += "\n[truncated]";
        budget.used_bytes = budget.max_bytes;
        return content;
    }

    budget.used_bytes += content.size();
    return content;
}

} // namespace agent::runtime
