#include "agent/runtime/agent_runner.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <utility>

namespace agent::runtime
{

namespace
{

bool HasSystemMessage(const core::Conversation& conversation)
{
    for (const core::ChatMessage& message : conversation.messages)
    {
        if (message.role == core::ChatRole::System)
        {
            return true;
        }
    }
    return false;
}

void NotifyTurnStart(core::SystemRegistry* systems)
{
    if (systems == nullptr)
    {
        return;
    }
    for (const std::shared_ptr<core::AgentSystem>& system : systems->Systems())
    {
        system->OnTurnStart();
    }
}

void NotifyModelRequest(core::SystemRegistry* systems, const core::ChatRequest& request)
{
    if (systems == nullptr)
    {
        return;
    }
    for (const std::shared_ptr<core::AgentSystem>& system : systems->Systems())
    {
        system->OnModelRequest(request);
    }
}

void NotifyModelResponse(core::SystemRegistry* systems, const core::ChatResponse& response)
{
    if (systems == nullptr)
    {
        return;
    }
    for (const std::shared_ptr<core::AgentSystem>& system : systems->Systems())
    {
        system->OnModelResponse(response);
    }
}

void NotifyToolCall(core::SystemRegistry* systems, const core::ToolCall& call)
{
    if (systems == nullptr)
    {
        return;
    }
    for (const std::shared_ptr<core::AgentSystem>& system : systems->Systems())
    {
        system->OnToolCall(call);
    }
}

void NotifyToolResult(core::SystemRegistry* systems, const core::ToolCall& call, const core::ToolResult& result)
{
    if (systems == nullptr)
    {
        return;
    }
    for (const std::shared_ptr<core::AgentSystem>& system : systems->Systems())
    {
        system->OnToolResult(call, result);
    }
}

void NotifyTurnEnd(core::SystemRegistry* systems, bool ok, const std::string& error, int steps)
{
    if (systems == nullptr)
    {
        return;
    }
    for (const std::shared_ptr<core::AgentSystem>& system : systems->Systems())
    {
        system->OnTurnEnd(ok, error, steps);
    }
}

bool IsToolAllowed(const AgentProfile& profile, const std::string& name)
{
    return std::find(profile.tool_names.begin(), profile.tool_names.end(), name) != profile.tool_names.end();
}

constexpr std::size_t MAX_TOOL_OUTPUT_BYTES = 32 * 1024;

std::string LimitToolOutput(std::string content, std::size_t& used_bytes)
{
    if (used_bytes >= MAX_TOOL_OUTPUT_BYTES)
    {
        return "[truncated: tool output budget exhausted]";
    }

    const std::size_t remaining = MAX_TOOL_OUTPUT_BYTES - used_bytes;
    if (content.size() > remaining)
    {
        content.resize(remaining);
        content += "\n[truncated]";
        used_bytes = MAX_TOOL_OUTPUT_BYTES;
        return content;
    }

    used_bytes += content.size();
    return content;
}

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

std::vector<core::ChatMessage> TrimMessages(const std::vector<core::ChatMessage>& messages, std::size_t max_chars)
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

    // 兜底：即使超预算，也保留 system + 最近一个 User 及其后续消息，避免无 user 请求。
    for (std::size_t index = messages.size(); index > system_end; --index)
    {
        if (messages[index - 1].role == core::ChatRole::User)
        {
            return BuildTrimmedMessages(messages, system_end, index - 1);
        }
    }

    return messages;
}

} // namespace

AgentRunner::AgentRunner(core::ChatModel& model, ToolRegistry* tools, core::SystemRegistry* systems)
    : m_model(model)
    , m_tools(tools)
    , m_systems(systems)
{}

AgentRunResult AgentRunner::RunTurn(const AgentProfile& profile, core::Conversation& conversation,
                                    const std::string& user_input)
{
    AgentRunResult result;
    const std::size_t turn_start_size = conversation.messages.size();
    bool turn_started = false;

    try
    {
        if (user_input.empty())
        {
            result.error = "empty input";
            return result;
        }
        if (profile.max_steps <= 0)
        {
            result.error = "max_steps must be positive";
            return result;
        }

        if (m_tools != nullptr)
        {
            for (const std::string& tool_name : profile.tool_names)
            {
                if (!m_tools->Has(tool_name))
                {
                    result.error = "unknown tool in profile: " + tool_name;
                    return result;
                }
            }
        }

        NotifyTurnStart(m_systems);
        turn_started = true;

        if (!profile.system_prompt.empty() && !HasSystemMessage(conversation))
        {
            conversation.Append(core::ChatRole::System, profile.system_prompt);
        }

        conversation.Append(core::ChatRole::User, user_input);

        const std::vector<core::ToolSpec> tool_specs =
            m_tools != nullptr ? m_tools->SpecsFor(profile.tool_names) : std::vector<core::ToolSpec>{};

        std::size_t tool_output_bytes = 0;

        for (int step = 0; step < profile.max_steps; ++step)
        {
            core::ChatRequest request;
            request.model = profile.model;
            request.temperature = profile.temperature;
            request.max_tokens = profile.max_tokens;
            request.tools = tool_specs;
            request.messages = TrimMessages(conversation.messages, profile.max_context_chars);

            NotifyModelRequest(m_systems, request);

            core::ChatResponse response;
            try
            {
                response = m_model.Chat(request);
            }
            catch (const std::exception& error)
            {
                result.error = std::string("model call failed: ") + error.what();
                result.steps = step + 1;
                NotifyTurnEnd(m_systems, false, result.error, result.steps);
                return result;
            }
            catch (...)
            {
                result.error = "model call failed";
                result.steps = step + 1;
                NotifyTurnEnd(m_systems, false, result.error, result.steps);
                return result;
            }

            NotifyModelResponse(m_systems, response);

            if (!response.ok)
            {
                result.error = response.error;
                result.steps = step + 1;
                NotifyTurnEnd(m_systems, false, result.error, result.steps);
                return result;
            }

            if (response.finish_reason == "length" || response.finish_reason == "content_filter" ||
                response.finish_reason == "insufficient_system_resource")
            {
                result.error = "model response incomplete: " + response.finish_reason;
                result.steps = step + 1;
                NotifyTurnEnd(m_systems, false, result.error, result.steps);
                return result;
            }

            if (response.tool_calls.empty())
            {
                if (response.content.empty())
                {
                    result.error = "model returned empty response";
                    result.steps = step + 1;
                    NotifyTurnEnd(m_systems, false, result.error, result.steps);
                    return result;
                }

                core::ChatMessage assistant;
                assistant.role = core::ChatRole::Assistant;
                assistant.content = response.content;
                conversation.Append(std::move(assistant));

                result.ok = true;
                result.content = response.content;
                result.steps = step + 1;
                NotifyTurnEnd(m_systems, true, {}, result.steps);
                return result;
            }

            core::ChatMessage assistant;
            assistant.role = core::ChatRole::Assistant;
            assistant.content = response.content;
            assistant.tool_calls = response.tool_calls;
            conversation.Append(std::move(assistant));

            for (const core::ToolCall& call : response.tool_calls)
            {
                NotifyToolCall(m_systems, call);

                core::ToolResult tool_result;
                if (m_tools == nullptr || !IsToolAllowed(profile, call.name))
                {
                    tool_result.ok = false;
                    tool_result.error = "tool not allowed: " + call.name;
                }
                else
                {
                    try
                    {
                        tool_result = m_tools->Execute(call.name, call.arguments_json);
                    }
                    catch (const std::exception& error)
                    {
                        tool_result.ok = false;
                        tool_result.error = std::string("tool execution failed: ") + error.what();
                    }
                    catch (...)
                    {
                        tool_result.ok = false;
                        tool_result.error = "tool execution failed";
                    }
                }

                NotifyToolResult(m_systems, call, tool_result);

                const std::string tool_content = tool_result.ok ? tool_result.content : tool_result.error;
                core::ChatMessage tool_message;
                tool_message.role = core::ChatRole::Tool;
                tool_message.tool_call_id = call.id;
                tool_message.content = LimitToolOutput(tool_content, tool_output_bytes);
                conversation.Append(std::move(tool_message));
            }
        }

        result.error = "max steps exceeded";
        result.steps = profile.max_steps;
        NotifyTurnEnd(m_systems, false, result.error, result.steps);
        return result;
    }
    catch (const std::exception& error)
    {
        conversation.messages.resize(turn_start_size);
        result.error = std::string("unhandled exception: ") + error.what();
        result.steps = 0;
        if (turn_started)
        {
            NotifyTurnEnd(m_systems, false, result.error, result.steps);
        }
        return result;
    }
    catch (...)
    {
        conversation.messages.resize(turn_start_size);
        result.error = "unhandled unknown exception";
        result.steps = 0;
        if (turn_started)
        {
            NotifyTurnEnd(m_systems, false, result.error, result.steps);
        }
        return result;
    }
}

} // namespace agent::runtime
