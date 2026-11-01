#include "agent/runtime/agent.h"

#include <stdexcept>
#include <utility>

namespace agent::runtime
{

namespace
{

AgentExecutor& ValidatedExecutor(const AgentServices& services)
{
    if (!services.services || !services.executor)
    {
        throw std::invalid_argument("agent services must include a service registry and executor");
    }
    return *services.executor;
}

} // namespace

Agent::Agent(AgentDescriptor descriptor, AgentServices services)
    : m_descriptor(std::move(descriptor))
    , m_services(std::move(services))
    , m_session(m_descriptor.profile, ValidatedExecutor(m_services))
{}

AgentRunResult Agent::Send(const std::string& user_input)
{
    return m_session.Send(user_input);
}

void Agent::ClearHistory()
{
    m_session.ClearHistory();
}

AgentSession& Agent::Session()
{
    return m_session;
}

const AgentSession& Agent::Session() const
{
    return m_session;
}

const core::Conversation& Agent::History() const
{
    return m_session.History();
}

const AgentDescriptor& Agent::Descriptor() const
{
    return m_descriptor;
}

const AgentId& Agent::Id() const
{
    return m_descriptor.id;
}

const AgentProfile& Agent::Profile() const
{
    return m_session.Profile();
}

const AgentServices& Agent::Services() const
{
    return m_services;
}

void Agent::SetProfile(AgentProfile profile)
{
    m_session.SetProfile(std::move(profile));
    m_descriptor.profile = m_session.Profile();
}

} // namespace agent::runtime
