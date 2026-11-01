#include "agent/runtime/context_policy.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace agent::runtime
{

namespace
{

std::size_t EstimateMessageChars(const core::ChatMessage& message)
{
    std::size_t total = message.content.size() + message.tool_call_id.size();
    for (const core::ToolCall& call : message.tool_calls)
    {
        total += call.id.size() + call.name.size() + call.arguments_json.size();
    }
    return total;
}

std::size_t EstimateMessagesChars(const std::vector<core::ChatMessage>& messages, std::size_t start)
{
    std::size_t total = 0;
    for (std::size_t index = start; index < messages.size(); ++index)
    {
        total += EstimateMessageChars(messages[index]);
    }
    return total;
}

std::vector<core::ChatMessage> BuildTrimmedMessages(const std::vector<core::ChatMessage>& messages,
                                                    std::size_t system_end, std::size_t start)
{
    std::vector<core::ChatMessage> trimmed;
    trimmed.reserve(system_end + messages.size() - start);
    trimmed.insert(trimmed.end(), messages.begin(), messages.begin() + static_cast<std::ptrdiff_t>(system_end));
    trimmed.insert(trimmed.end(), messages.begin() + static_cast<std::ptrdiff_t>(start), messages.end());
    return trimmed;
}

} // namespace

std::vector<core::ChatMessage> KeepRecentTurnsPolicy::Prepare(const std::vector<core::ChatMessage>& messages,
                                                              std::size_t max_chars) const
{
    if (max_chars == 0 || EstimateMessagesChars(messages, 0) <= max_chars)
    {
        return messages;
    }

    std::size_t system_end = 0;
    while (system_end < messages.size() && messages[system_end].role == core::ChatRole::System)
    {
        ++system_end;
    }

    const std::size_t system_chars = EstimateMessagesChars(messages, 0) - EstimateMessagesChars(messages, system_end);
    for (std::size_t start = system_end; start < messages.size(); ++start)
    {
        if (messages[start].role != core::ChatRole::User)
        {
            continue;
        }
        if (system_chars + EstimateMessagesChars(messages, start) <= max_chars)
        {
            return BuildTrimmedMessages(messages, system_end, start);
        }
    }

    for (std::size_t index = messages.size(); index > system_end; --index)
    {
        if (messages[index - 1].role == core::ChatRole::User)
        {
            return BuildTrimmedMessages(messages, system_end, index - 1);
        }
    }

    return messages;
}

} // namespace agent::runtime
