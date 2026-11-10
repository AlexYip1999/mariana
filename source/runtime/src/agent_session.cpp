#include "agent/runtime/agent_session.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

#include "agent/core/utf8.h"
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

std::size_t EstimateMessageChars(const core::ChatMessage& message)
{
    std::size_t total = message.content.size() + message.tool_call_id.size();
    for (const core::ToolCall& call : message.tool_calls)
    {
        total += call.id.size() + call.name.size() + call.arguments_json.size();
    }
    return total;
}

std::size_t EstimateConversationChars(const core::Conversation& conversation)
{
    std::size_t total = 0;
    for (const core::ChatMessage& message : conversation.messages)
    {
        total += EstimateMessageChars(message);
    }
    return total;
}

bool IsSummaryMessage(const core::ChatMessage& message)
{
    return message.role == core::ChatRole::System && message.content.rfind(core::kSummaryMessagePrefix, 0) == 0;
}

std::size_t SystemPrefixEnd(const std::vector<core::ChatMessage>& messages)
{
    std::size_t end = 0;
    while (end < messages.size() && messages[end].role == core::ChatRole::System)
    {
        ++end;
    }
    return end;
}

std::vector<std::size_t> UserMessageIndices(const std::vector<core::ChatMessage>& messages, std::size_t start)
{
    std::vector<std::size_t> indices;
    for (std::size_t index = start; index < messages.size(); ++index)
    {
        if (messages[index].role == core::ChatRole::User)
        {
            indices.push_back(index);
        }
    }
    return indices;
}

core::CallContext MakeCompactionCall(const AgentProfile& profile)
{
    const int timeout_ms = profile.turn_timeout_ms > 0 ? profile.turn_timeout_ms : 30000;
    core::CallContext call;
    call.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    return call;
}

bool ShouldCompact(const AgentProfile& profile, const core::Conversation& conversation)
{
    const MemoryPolicy& policy = profile.memory_policy;
    if (!policy.enable_compaction || policy.compact_trigger_chars == 0)
    {
        return false;
    }
    return EstimateConversationChars(conversation) >= policy.compact_trigger_chars;
}

/// 把旧轮次压成一条摘要 System 消息；成功时替换 conversation。
bool TryCompact(const AgentProfile& profile, core::Summarizer& summarizer, core::Conversation& conversation)
{
    const MemoryPolicy& policy = profile.memory_policy;
    const std::size_t system_end = SystemPrefixEnd(conversation.messages);
    const std::vector<std::size_t> user_indices = UserMessageIndices(conversation.messages, system_end);
    const std::size_t keep = policy.compact_keep_recent_turns == 0 ? 1 : policy.compact_keep_recent_turns;
    if (user_indices.size() <= keep)
    {
        return false;
    }
    const std::size_t split = user_indices[user_indices.size() - keep];

    // 保留非摘要的前导 System 消息（system prompt），旧摘要稍后替换。
    std::vector<core::ChatMessage> prefix;
    for (std::size_t index = 0; index < system_end; ++index)
    {
        if (!IsSummaryMessage(conversation.messages[index]))
        {
            prefix.push_back(conversation.messages[index]);
        }
    }

    core::SummaryRequest request;
    request.scope = policy.recall_scopes.empty() ? core::MemoryScope{} : policy.recall_scopes.front();
    request.max_chars = policy.compact_summary_max_chars;
    request.call = MakeCompactionCall(profile);
    request.turns.assign(conversation.messages.begin() + static_cast<std::ptrdiff_t>(system_end),
                         conversation.messages.begin() + static_cast<std::ptrdiff_t>(split));
    if (request.turns.empty())
    {
        return false;
    }

    core::SummaryResult result;
    try
    {
        result = summarizer.Summarize(request);
    }
    catch (...)
    {
        // 压缩失败保留原历史，不影响会话。
        return false;
    }
    if (!result.ok || result.summary.empty())
    {
        return false;
    }
    if (request.max_chars > 0 && result.summary.size() > request.max_chars)
    {
        result.summary.resize(core::Utf8PrefixSize(result.summary, request.max_chars));
    }

    core::Conversation compacted;
    compacted.session_id = conversation.session_id;
    compacted.messages = std::move(prefix);
    core::ChatMessage summary_message;
    summary_message.role = core::ChatRole::System;
    summary_message.content = std::string(core::kSummaryMessagePrefix) + result.summary;
    compacted.messages.push_back(std::move(summary_message));
    compacted.messages.insert(compacted.messages.end(),
                              conversation.messages.begin() + static_cast<std::ptrdiff_t>(split),
                              conversation.messages.end());
    conversation = std::move(compacted);
    return true;
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

    CompactIfNeeded();

    SystemNotifier(*m_systems).TurnFinished(execution.result.ToEvent());
    return execution.result;
}

void AgentSession::CompactIfNeeded()
{
    const std::shared_ptr<core::Summarizer> summarizer = m_executor ? m_executor->Summarizer() : nullptr;
    if (!summarizer)
    {
        return;
    }

    AgentProfile profile;
    core::Conversation snapshot;
    {
        std::lock_guard state_lock(m_state_mutex);
        profile = m_profile;
        if (!profile.memory_policy.enable_compaction)
        {
            return;
        }
        snapshot = m_conversation;
    }

    if (!ShouldCompact(profile, snapshot))
    {
        return;
    }
    if (!TryCompact(profile, *summarizer, snapshot))
    {
        return;
    }

    std::lock_guard state_lock(m_state_mutex);
    m_conversation = std::move(snapshot);
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
