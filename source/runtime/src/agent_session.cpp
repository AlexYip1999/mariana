#include "agent/runtime/agent_session.h"

#include <utility>

#include "agent/runtime/agent_runner.h"

namespace agent::runtime
{

AgentSession::AgentSession(AgentSessionConfig config, AgentSessionDependencies dependencies)
    : m_config(std::move(config))
    , m_dependencies(dependencies)
    , m_systems(dependencies.systems)
{
    AppendSystemPrompt();
}

AgentRunResult AgentSession::RunTurn(const std::string& user_input)
{
    AgentRunResult result;
    if (user_input.empty())
    {
        result.error = "empty input";
        return result;
    }
    if (m_dependencies.model == nullptr)
    {
        result.error = "chat model unavailable";
        return result;
    }

    AgentProfile profile;
    profile.model = m_config.model;
    profile.system_prompt = m_config.system_prompt;
    profile.temperature = m_config.temperature;
    profile.max_tokens = m_config.max_tokens;
    profile.max_steps = m_config.max_steps;
    if (m_dependencies.tools != nullptr)
    {
        profile.tool_names = m_config.tool_names;
    }

    ToolRegistry& tools = m_dependencies.tools != nullptr ? *m_dependencies.tools : m_empty_tools;
    AgentRunner runner(*m_dependencies.model, tools, m_systems);
    return runner.Run(profile, m_conversation, user_input);
}

void AgentSession::ClearHistory()
{
    m_conversation.Clear();
    AppendSystemPrompt();
}

const core::Conversation& AgentSession::History() const
{
    return m_conversation;
}

void AgentSession::AppendSystemPrompt()
{
    if (!m_config.system_prompt.empty())
    {
        m_conversation.Append(core::ChatRole::System, m_config.system_prompt);
    }
}

} // namespace agent::runtime
