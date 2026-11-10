#include "agent/runtime/turn_transaction.h"

#include <algorithm>
#include <set>
#include <utility>

namespace agent::runtime
{

namespace
{

bool ShouldRollback(const RunOutcome& outcome)
{
    if (outcome.status == RunStatus::Cancelled)
    {
        return true;
    }
    return outcome.error_code == RunErrorCode::DeadlineExceeded || outcome.error_code == RunErrorCode::InternalError ||
           outcome.error_code == RunErrorCode::InvalidInput ||
           outcome.error_code == RunErrorCode::InvalidConfiguration ||
           outcome.error_code == RunErrorCode::ContextFailure;
}

bool HasSystemMessage(const core::Conversation& conversation)
{
    return std::any_of(conversation.messages.begin(), conversation.messages.end(),
                       [](const core::ChatMessage& message)
                       {
                           return message.role == core::ChatRole::System;
                       });
}

} // namespace

TurnWorkspace::TurnWorkspace(core::Conversation& conversation, const RunContext& context)
    : m_conversation(conversation)
    , m_context(context)
{}

core::Conversation& TurnWorkspace::Conversation()
{
    return m_conversation;
}

const core::Conversation& TurnWorkspace::Conversation() const
{
    return m_conversation;
}

const RunContext& TurnWorkspace::Context() const
{
    return m_context;
}

TurnTransaction::TurnTransaction(core::Conversation& conversation, const RunContext& context)
    : m_conversation(conversation)
    , m_context(context)
    , m_start_size(conversation.messages.size())
{}

void TurnTransaction::Start(const AgentProfile& profile, const std::string& input)
{
    if (!HasSystemMessage(m_conversation) && !profile.system_prompt.empty())
    {
        m_conversation.Append(core::ChatRole::System, profile.system_prompt);
    }
    m_conversation.Append(core::ChatRole::User, input);
}

TurnCommit TurnTransaction::Finish(const RunOutcome& outcome)
{
    TurnCommit commit;
    commit.result.session_id = m_context.session_id;
    commit.result.run_id = m_context.run_id;
    commit.result.status = outcome.status;
    commit.result.error_code = outcome.error_code;
    commit.result.error = outcome.error;
    commit.result.content = outcome.content;
    commit.result.steps = outcome.steps;
    commit.result.tool_attempts = outcome.tool_attempts;
    commit.result.tool_executions = outcome.tool_executions;

    if (ShouldRollback(outcome))
    {
        m_conversation.messages.resize(m_start_size);
        return commit;
    }

    RemoveIncompleteBatch();
    commit.history = std::move(m_conversation);
    return commit;
}

void TurnTransaction::RemoveIncompleteBatch()
{
    std::set<std::string> pending;
    std::size_t batch_start = m_conversation.messages.size();
    for (std::size_t index = m_start_size; index < m_conversation.messages.size(); ++index)
    {
        const core::ChatMessage& message = m_conversation.messages[index];
        if (!message.tool_calls.empty())
        {
            batch_start = index;
            pending.clear();
            for (const core::ToolCall& call : message.tool_calls)
            {
                pending.insert(call.id);
            }
        }
        else if (message.role == core::ChatRole::Tool)
        {
            pending.erase(message.tool_call_id);
        }
    }

    if (!pending.empty())
    {
        m_conversation.messages.resize(batch_start);
    }
}

} // namespace agent::runtime
