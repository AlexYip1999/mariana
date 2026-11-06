#include "agent/runtime/agent.h"

#include <stdexcept>
#include <utility>

namespace agent::runtime
{

namespace
{

std::shared_ptr<core::SystemRegistry> ResolveSystems(const std::shared_ptr<AgentScope>& scope,
                                                     const std::shared_ptr<AgentExecutor>& executor)
{
    const auto scope_systems = scope ? scope->Get<core::SystemRegistry>() : nullptr;
    const auto executor_systems = executor ? executor->SystemRegistry() : nullptr;
    if (scope_systems && executor_systems && scope_systems != executor_systems)
    {
        throw std::invalid_argument("agent scope and executor use different SystemRegistry instances");
    }
    const auto systems = scope_systems ? scope_systems : executor_systems;
    if (systems && !scope_systems && scope)
    {
        scope->Set<core::SystemRegistry>(systems);
    }
    return systems;
}

AgentSession MakeSession(const std::shared_ptr<AgentScope>& scope, AgentDependencies dependencies, AgentProfile profile,
                         std::string session_id)
{
    const auto systems = ResolveSystems(scope, dependencies.executor);
    return AgentSession(std::move(profile), std::move(dependencies.executor), std::move(session_id), systems);
}

} // namespace

Agent::Agent(AgentDescriptor descriptor, AgentProfile initial_profile, AgentDependencies dependencies,
             std::string session_id)
    : m_descriptor(std::move(descriptor))
    , m_scope(dependencies.services ? std::move(dependencies.services) : std::make_shared<AgentScope>())
    , m_session(MakeSession(m_scope, std::move(dependencies), std::move(initial_profile), std::move(session_id)))
{}

AgentRunResult Agent::Send(const std::string& user_input, const RunOptions& options)
{
    return m_session.Send(user_input, options);
}

AgentRunResult Agent::Send(const std::string& user_input, std::stop_token stop_token)
{
    return m_session.Send(user_input, stop_token);
}

bool Agent::ClearHistory()
{
    return m_session.ClearHistory();
}

const AgentSession& Agent::Session() const
{
    return m_session;
}

const std::string& Agent::SessionId() const
{
    return m_session.Id();
}

core::Conversation Agent::History() const
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

AgentProfile Agent::Profile() const
{
    return m_session.Profile();
}

AgentDependencies Agent::Dependencies() const
{
    return {m_scope, m_session.Executor()};
}

bool Agent::SetProfile(AgentProfile profile)
{
    return m_session.SetProfile(std::move(profile));
}

bool Agent::Reconfigure(AgentProfile profile, std::shared_ptr<AgentExecutor> executor)
{
    return m_session.Reconfigure(std::move(profile), std::move(executor));
}

} // namespace agent::runtime
