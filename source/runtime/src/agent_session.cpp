#include "agent/runtime/agent_session.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "agent/runtime/execution_id.h"
#include "agent/runtime/system_notifier.h"
#include "agent/runtime/turn_executor.h"

namespace agent::runtime
{

namespace
{

// A lease also rejects same-thread reentry from synchronous observer callbacks.
class SessionLease
{
  public:
    explicit SessionLease(std::atomic<bool>& running)
        : m_running(running)
        , m_acquired(!running.exchange(true))
    {}

    ~SessionLease()
    {
        if (m_acquired)
        {
            m_running.store(false);
        }
    }

    bool Acquired() const
    {
        return m_acquired;
    }

  private:
    std::atomic<bool>& m_running;
    bool m_acquired;
};

/// 预检失败不开始 Turn，也不写历史/记录。
bool PreflightFailure(const AgentProfile& profile, const std::string& user_input, const RunContext& context,
                      AgentRunResult& result)
{
    result.session_id = context.session_id;
    result.run_id = context.run_id;

    if (user_input.empty())
    {
        result.status = RunStatus::Failed;
        result.error_code = RunErrorCode::InvalidInput;
        result.error = "empty input";
        return true;
    }
    if (profile.max_steps <= 0)
    {
        result.status = RunStatus::Failed;
        result.error_code = RunErrorCode::InvalidConfiguration;
        result.error = "max_steps must be positive";
        return true;
    }
    const RunErrorCode interruption = context.Interruption();
    if (interruption != RunErrorCode::None)
    {
        result.status = interruption == RunErrorCode::Cancelled ? RunStatus::Cancelled : RunStatus::BudgetExceeded;
        result.error_code = interruption;
        result.error =
            interruption == RunErrorCode::Cancelled ? "agent turn cancelled" : "agent turn deadline exceeded";
        return true;
    }
    return false;
}

} // namespace

AgentSession::AgentSession(AgentSessionConfig config, std::shared_ptr<AgentExecutor> executor, std::string session_id,
                           std::shared_ptr<core::SystemRegistry> systems, std::size_t max_records)
    : m_profile(std::move(config))
    , m_executor(std::move(executor))
    , m_session_id(session_id.empty() ? ExecutionId::NewSession() : std::move(session_id))
    , m_systems(systems ? std::move(systems) : std::make_shared<core::SystemRegistry>())
    , m_max_records(max_records)
{
    if (!m_executor)
    {
        throw std::invalid_argument("session requires an executor");
    }
    m_conversation.session_id = m_session_id;
    AppendSystemPrompt();
}

AgentRunResult AgentSession::Send(const std::string& user_input, std::stop_token stop_token)
{
    RunOptions options;
    options.stop_token = stop_token;
    return Send(user_input, options);
}

AgentRunResult AgentSession::Send(const std::string& user_input, const RunOptions& options)
{
    SessionLease lease(m_running);
    if (!lease.Acquired())
    {
        AgentRunResult busy;
        busy.session_id = m_session_id;
        busy.status = RunStatus::Busy;
        busy.error_code = RunErrorCode::Busy;
        busy.error = "session already has an active turn";
        return busy;
    }
    AgentProfile profile;
    {
        std::lock_guard state_lock(m_state_mutex);
        profile = m_profile;
    }

    RunContext context;
    TurnExecution execution;
    try
    {
        context = RunContext::Create(profile, m_session_id, options);
        AgentRunResult preflight;
        if (PreflightFailure(profile, user_input, context, preflight))
        {
            return preflight;
        }
        if (const auto validation_error = m_executor->ValidationError(profile))
        {
            preflight.session_id = context.session_id;
            preflight.run_id = context.run_id;
            preflight.status = RunStatus::Failed;
            preflight.error_code = RunErrorCode::InvalidConfiguration;
            preflight.error = *validation_error;
            return preflight;
        }

        core::Conversation staged;
        {
            std::lock_guard state_lock(m_state_mutex);
            staged = m_conversation;
        }
        execution = ExecuteTurn(profile, staged, *m_executor, user_input, context, *m_systems);
    }
    catch (const std::invalid_argument& error)
    {
        execution.result.session_id = m_session_id;
        execution.result.run_id = context.run_id;
        execution.result.status = RunStatus::Failed;
        execution.result.error_code = RunErrorCode::InvalidConfiguration;
        execution.result.error = error.what();
    }
    catch (const std::exception& error)
    {
        execution.result.session_id = m_session_id;
        execution.result.run_id = context.run_id;
        execution.result.status = RunStatus::Failed;
        execution.result.error_code = RunErrorCode::InternalError;
        execution.result.error = error.what();
    }
    catch (...)
    {
        execution.result.session_id = m_session_id;
        execution.result.run_id = context.run_id;
        execution.result.status = RunStatus::Failed;
        execution.result.error_code = RunErrorCode::InternalError;
        execution.result.error = "executor failed";
    }

    {
        std::lock_guard state_lock(m_state_mutex);
        if (execution.history)
        {
            m_conversation = std::move(*execution.history);
        }
        m_records.push_back(static_cast<const TurnRecord&>(execution.result));
        if (m_max_records > 0 && m_records.size() > m_max_records)
        {
            m_records.erase(m_records.begin(), m_records.begin() + (m_records.size() - m_max_records));
        }
    }
    SystemNotifier(*m_systems).TurnFinished(execution.result.ToEvent());
    return execution.result;
}

bool AgentSession::ClearHistory()
{
    SessionLease lease(m_running);
    if (!lease.Acquired())
    {
        return false;
    }
    std::lock_guard state_lock(m_state_mutex);
    m_conversation.Clear();
    AppendSystemPrompt();
    return true;
}

core::Conversation AgentSession::History() const
{
    std::lock_guard state_lock(m_state_mutex);
    return m_conversation;
}

AgentProfile AgentSession::Profile() const
{
    std::lock_guard state_lock(m_state_mutex);
    return m_profile;
}

std::shared_ptr<AgentExecutor> AgentSession::Executor() const
{
    std::lock_guard state_lock(m_state_mutex);
    return m_executor;
}

std::vector<TurnRecord> AgentSession::Records() const
{
    std::lock_guard state_lock(m_state_mutex);
    return m_records;
}

const std::string& AgentSession::Id() const
{
    return m_session_id;
}

bool AgentSession::SetProfile(AgentProfile profile)
{
    return Reconfigure(std::move(profile), nullptr);
}

bool AgentSession::Reconfigure(AgentProfile profile, std::shared_ptr<AgentExecutor> executor)
{
    SessionLease lease(m_running);
    if (!lease.Acquired())
    {
        return false;
    }
    std::lock_guard state_lock(m_state_mutex);
    if (profile.system_prompt != m_profile.system_prompt)
    {
        auto& messages = m_conversation.messages;
        std::erase_if(messages,
                      [](const core::ChatMessage& message)
                      {
                          return message.role == core::ChatRole::System;
                      });
        if (!profile.system_prompt.empty())
        {
            core::ChatMessage system;
            system.role = core::ChatRole::System;
            system.content = profile.system_prompt;
            messages.insert(messages.begin(), std::move(system));
        }
    }
    m_profile = std::move(profile);
    if (executor)
    {
        m_executor = std::move(executor);
    }
    return true;
}

void AgentSession::AppendSystemPrompt()
{
    if (!m_profile.system_prompt.empty())
    {
        m_conversation.Append(core::ChatRole::System, m_profile.system_prompt);
    }
}

} // namespace agent::runtime
