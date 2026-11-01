#include "agent/runtime/agent_session.h"

#include <utility>

namespace agent::runtime
{

AgentSession::AgentSession(AgentSessionConfig config, AgentExecutor& executor)
    : m_profile(std::move(config))
    , m_executor(executor)
{
    AppendSystemPrompt();
}

AgentRunResult AgentSession::Send(const std::string& user_input)
{
    AgentRunResult result;
    if (user_input.empty())
    {
        result.error = "empty input";
        return result;
    }

    return m_executor.RunTurn(m_profile, m_conversation, user_input);
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

const AgentProfile& AgentSession::Profile() const
{
    return m_profile;
}

void AgentSession::SetProfile(AgentProfile profile)
{
    m_profile = std::move(profile);
}

void AgentSession::AppendSystemPrompt()
{
    if (!m_profile.system_prompt.empty())
    {
        m_conversation.Append(core::ChatRole::System, m_profile.system_prompt);
    }
}

} // namespace agent::runtime
