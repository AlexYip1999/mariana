#include "agent/runtime/agent_session.h"

#include <utility>

namespace agent::runtime
{

AgentSession::AgentSession(AgentSessionConfig config, AgentSessionDependencies dependencies)
    : m_config(std::move(config))
    , m_dependencies(dependencies)
{
    if (m_dependencies.services != nullptr)
    {
        m_tracer = m_dependencies.services->Get<core::Tracer>();
    }
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

    if (m_tracer != nullptr)
    {
        m_tracer->OnTurnStart();
    }

    m_conversation.Append(core::ChatRole::User, user_input);

    core::ChatRequest request;
    request.model = m_config.model;
    request.temperature = m_config.temperature;
    request.max_tokens = m_config.max_tokens;
    request.messages = m_conversation.messages;

    if (m_tracer != nullptr)
    {
        m_tracer->OnModelRequest(request);
    }

    const core::ChatResponse response = m_dependencies.model->Chat(request);
    if (m_tracer != nullptr)
    {
        m_tracer->OnModelResponse(response);
    }

    if (!response.ok)
    {
        // 失败的那一轮不留在历史里。
        m_conversation.messages.pop_back();
        result.error = response.error;
        return result;
    }

    m_conversation.Append(core::ChatRole::Assistant, response.content);
    result.ok = true;
    result.content = response.content;
    return result;
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
