#include "agent/runtime/agent_runner.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <set>
#include <stdexcept>
#include <utility>

#include "agent/core/utf8.h"
#include "agent/runtime/composition.h"
#include "agent/runtime/system_notifier.h"
#include "agent/runtime/turn_transaction.h"

namespace agent::runtime
{
namespace
{

constexpr const char* kMemoryTag = "memory";
constexpr const char* kContextTag = "context";

RunOutcome Decided(RunOutcome result, RunStatus status, RunErrorCode code, std::string error,
                   std::string content = std::string())
{
    result.status = status;
    result.error_code = code;
    result.error = std::move(error);
    result.content = std::move(content);
    return result;
}

RunOutcome Interrupted(RunOutcome result, RunErrorCode code)
{
    if (code == RunErrorCode::Cancelled)
    {
        return Decided(std::move(result), RunStatus::Cancelled, code, "agent turn cancelled");
    }
    return Decided(std::move(result), RunStatus::BudgetExceeded, code, "agent turn deadline exceeded");
}

bool IsFatalToolError(RunErrorCode code)
{
    return code == RunErrorCode::ToolCallLimit || code == RunErrorCode::ToolOutputLimit ||
           code == RunErrorCode::InvalidToolCall;
}

bool HasIncompleteResponse(const core::ChatResponse& response)
{
    return response.finish_reason == "length" || response.finish_reason == "content_filter" ||
           response.finish_reason == "insufficient_system_resource";
}

bool HasValidToolCalls(const std::vector<core::ToolCall>& calls)
{
    std::set<std::string> ids;
    for (const auto& call : calls)
    {
        if (call.id.empty() || !core::IsValidToolName(call.name) || !ids.insert(call.id).second)
        {
            return false;
        }
    }
    return true;
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

std::size_t EstimateMessagesChars(const std::vector<core::ChatMessage>& messages)
{
    std::size_t total = 0;
    for (const core::ChatMessage& message : messages)
    {
        total += EstimateMessageChars(message);
    }
    return total;
}

std::size_t TaggedWrapperChars(const std::string& tag)
{
    return 2 * tag.size() + 7;
}

std::string WrapTagged(const std::string& tag, const std::string& payload)
{
    return "<" + tag + ">\n" + payload + "\n</" + tag + ">";
}

/// 把带标签的上下文包成一条 System 消息，插在最近一条 User（当前轮输入）之前。
void InjectTaggedContext(std::vector<core::ChatMessage>& messages, const std::string& tag, const std::string& payload)
{
    if (payload.empty())
    {
        return;
    }

    core::ChatMessage message;
    message.role = core::ChatRole::System;
    message.content = WrapTagged(tag, payload);

    for (std::size_t index = messages.size(); index > 0; --index)
    {
        if (messages[index - 1].role == core::ChatRole::User)
        {
            messages.insert(messages.begin() + static_cast<std::ptrdiff_t>(index - 1), std::move(message));
            return;
        }
    }
    messages.push_back(std::move(message));
}

/// 每轮调用一次 ContextProvider；失败返回 false（fail-closed），由调用方转成 RunOutcome。
bool BuildProviderContext(const RunnerDependencies& dependencies, const AgentProfile& profile,
                          const std::string& user_input, std::size_t used_chars, const RunContext& context,
                          std::string& context_json, std::string& error)
{
    if (!dependencies.context_provider || !dependencies.context_policy)
    {
        return true;
    }

    std::size_t payload_budget = 0;
    if (profile.max_context_chars > 0)
    {
        const std::size_t remaining =
            used_chars < profile.max_context_chars ? profile.max_context_chars - used_chars : 0;
        const std::size_t wrapper = TaggedWrapperChars(kContextTag);
        if (remaining <= wrapper)
        {
            // 没有剩余上下文预算：跳过 Provider，不注入。
            return true;
        }
        payload_budget = remaining - wrapper;
    }

    core::AgentContextRequest request;
    request.session_id = context.session_id;
    request.run_id = context.run_id;
    request.user_input = user_input;
    request.max_context_chars = payload_budget;
    request.call = context.call;

    core::AgentContextResult result;
    try
    {
        result = dependencies.context_provider->BuildContext(request);
    }
    catch (const std::exception& provider_error)
    {
        error = std::string("context provider threw: ") + provider_error.what();
        return false;
    }
    catch (...)
    {
        error = "context provider threw an unknown exception";
        return false;
    }

    if (!result.ok)
    {
        error = result.error.empty() ? "context provider failed" : result.error;
        return false;
    }

    if (profile.max_context_chars > 0 && result.context_json.size() > payload_budget)
    {
        result.context_json.resize(core::Utf8PrefixSize(result.context_json, payload_budget));
    }
    context_json = std::move(result.context_json);
    return true;
}

/// 每轮调用一次 MemoryStore；召回失败降级为空（记忆是增强项，不阻塞决策）。
void BuildMemoryContext(const RunnerDependencies& dependencies, const AgentProfile& profile, const RunContext& context,
                        const std::string& user_input, std::size_t used_chars, std::string& memory_text)
{
    const MemoryPolicy& policy = profile.memory_policy;
    if (!policy.enable_recall || !dependencies.memory)
    {
        return;
    }

    std::size_t payload_budget = policy.recall_budget_chars;
    if (profile.max_context_chars > 0)
    {
        const std::size_t remaining =
            used_chars < profile.max_context_chars ? profile.max_context_chars - used_chars : 0;
        const std::size_t wrapper = TaggedWrapperChars(kMemoryTag);
        if (remaining <= wrapper)
        {
            return;
        }
        payload_budget = remaining - wrapper;
        if (policy.recall_budget_chars > 0 && policy.recall_budget_chars < payload_budget)
        {
            payload_budget = policy.recall_budget_chars;
        }
    }

    core::MemoryQuery query;
    query.text = user_input;
    query.scopes = policy.recall_scopes;
    query.limit = policy.recall_limit;
    query.max_chars = payload_budget;
    query.call = context.call;

    std::vector<core::MemoryHit> hits;
    try
    {
        hits = dependencies.memory->Recall(query);
    }
    catch (...)
    {
        // 记忆召回异常降级为空，不影响本轮。
        return;
    }

    std::string rendered;
    for (const core::MemoryHit& hit : hits)
    {
        if (hit.record.text.empty())
        {
            continue;
        }
        rendered += "[" + std::string(core::ToString(hit.record.kind)) + "] " + hit.record.text + "\n";
    }
    if (rendered.empty())
    {
        return;
    }
    if (payload_budget > 0 && rendered.size() > payload_budget)
    {
        rendered.resize(core::Utf8PrefixSize(rendered, payload_budget));
    }
    memory_text = std::move(rendered);
}

} // namespace

static RunOutcome RunSteps(const RunnerDependencies& dependencies, const AgentProfile& profile,
                           TurnWorkspace& workspace, const RunContext& context, const SystemNotifier& notifier,
                           const std::string& memory_context, const std::string& provider_context, RunOutcome& result)
{
    core::Conversation& conversation = workspace.Conversation();
    const auto specs = dependencies.tools->SpecsFor(profile.tool_names);
    ToolDispatcher dispatcher(*dependencies.tools, *dependencies.tool_policy, *dependencies.tool_output_policy,
                              *dependencies.tool_scheduler, notifier, context);

    for (int step = 0; step < context.budget.max_model_calls; ++step)
    {
        if (context.Interruption() != RunErrorCode::None)
        {
            return Interrupted(std::move(result), context.Interruption());
        }

        // 组装本轮请求：裁剪历史、附加工具定义与两类注入上下文。
        core::ChatRequest request;
        request.call = context.call;
        request.model = profile.model;
        request.temperature = profile.temperature;
        request.max_tokens = profile.max_tokens;
        request.tools = specs;
        request.messages = dependencies.context_policy->Prepare(conversation.messages, profile.max_context_chars);
        InjectTaggedContext(request.messages, kMemoryTag, memory_context);
        InjectTaggedContext(request.messages, kContextTag, provider_context);

        notifier.ModelRequested(request);
        if (context.Interruption() != RunErrorCode::None)
        {
            return Interrupted(std::move(result), context.Interruption());
        }
        ++result.steps;

        core::ChatResponse response;
        try
        {
            response = dependencies.model->Chat(request);
        }
        catch (const std::exception& error)
        {
            response.error = std::string("model call failed: ") + error.what();
            response.error_code = core::CallError::ModelFailure;
        }
        catch (...)
        {
            response.error = "model call failed";
            response.error_code = core::CallError::ModelFailure;
        }

        notifier.ModelResponded(response);
        if (context.Interruption() != RunErrorCode::None)
        {
            return Interrupted(std::move(result), context.Interruption());
        }

        if (!response.ok)
        {
            const auto call_code =
                response.error_code == core::CallError::None ? core::CallError::ModelFailure : response.error_code;
            const auto code = ToRunError(call_code);
            if (code == RunErrorCode::Cancelled || code == RunErrorCode::DeadlineExceeded)
            {
                return Interrupted(std::move(result), code);
            }
            return Decided(std::move(result), RunStatus::Failed, code, response.error);
        }
        if (HasIncompleteResponse(response))
        {
            return Decided(std::move(result), RunStatus::Failed, RunErrorCode::IncompleteResponse,
                           "model response incomplete: " + response.finish_reason);
        }
        if (response.tool_calls.empty())
        {
            if (response.content.empty())
            {
                return Decided(std::move(result), RunStatus::Failed, RunErrorCode::EmptyResponse,
                               "model returned empty response");
            }
            conversation.Append(core::ChatRole::Assistant, response.content);
            return Decided(std::move(result), RunStatus::Completed, RunErrorCode::None, {}, response.content);
        }
        if (!HasValidToolCalls(response.tool_calls))
        {
            return Decided(std::move(result), RunStatus::Failed, RunErrorCode::InvalidToolCall,
                           "invalid tool call id or name");
        }

        // 执行工具调用并把结果逐条回填对话；致命错误直接结束本轮。
        core::ChatMessage assistant;
        assistant.role = core::ChatRole::Assistant;
        assistant.content = response.content;
        assistant.tool_calls = response.tool_calls;
        conversation.Append(std::move(assistant));

        for (const auto& call : response.tool_calls)
        {
            const ToolDispatchResult dispatched = dispatcher.Execute(call, profile.tool_names, result);
            if (context.Interruption() != RunErrorCode::None)
            {
                return Interrupted(std::move(result), context.Interruption());
            }
            if (dispatched.error_code == RunErrorCode::Cancelled ||
                dispatched.error_code == RunErrorCode::DeadlineExceeded)
            {
                return Interrupted(std::move(result), dispatched.error_code);
            }
            if (IsFatalToolError(dispatched.error_code))
            {
                const auto status = dispatched.error_code == RunErrorCode::InvalidToolCall ? RunStatus::Failed
                                                                                           : RunStatus::BudgetExceeded;
                return Decided(std::move(result), status, dispatched.error_code, dispatched.result.error);
            }
            core::ChatMessage tool_message;
            tool_message.role = core::ChatRole::Tool;
            tool_message.tool_call_id = call.id;
            tool_message.content = dispatched.result.ok ? dispatched.result.content : dispatched.result.error;
            conversation.Append(std::move(tool_message));
        }
    }
    return Decided(std::move(result), RunStatus::BudgetExceeded, RunErrorCode::ModelCallLimit, "max steps exceeded");
}

namespace
{

bool HasRequiredDependencies(const RunnerDependencies& dependencies)
{
    return dependencies.model && dependencies.tools && dependencies.tool_policy && dependencies.context_policy &&
           dependencies.tool_output_policy && dependencies.tool_scheduler && dependencies.systems;
}

} // namespace

AgentRunner::AgentRunner(RunnerDependencies dependencies)
    : m_dependencies(std::move(dependencies))
{
    if (!HasRequiredDependencies(m_dependencies))
    {
        throw std::invalid_argument("runner requires explicit model, tools, policies, scheduler and systems");
    }

    // Snapshot registration so later host edits cannot change an active executor's capabilities.
    m_dependencies.tools = std::make_shared<ToolRegistry>(*m_dependencies.tools);
}

std::optional<std::string> AgentRunner::ValidationError(const AgentProfile& profile) const
{
    const CompositionValidation validation = ValidateComposition(m_dependencies, profile);
    if (validation.Ok())
    {
        return std::nullopt;
    }
    return validation.Describe();
}

std::shared_ptr<core::SystemRegistry> AgentRunner::SystemRegistry() const
{
    return m_dependencies.systems;
}

std::shared_ptr<core::Summarizer> AgentRunner::Summarizer() const
{
    return m_dependencies.summarizer;
}

RunOutcome AgentRunner::RunTurn(const AgentProfile& profile, TurnWorkspace& workspace, const std::string& user_input,
                                const RunContext& context)
{
    RunOutcome outcome;
    SystemNotifier notifier(*m_dependencies.systems);

    try
    {
        if (user_input.empty())
        {
            outcome = {RunStatus::Failed, RunErrorCode::InvalidInput, "empty input"};
        }
        else if (profile.max_steps <= 0)
        {
            outcome = {RunStatus::Failed, RunErrorCode::InvalidConfiguration, "max_steps must be positive"};
        }
        else if (context.Interruption() != RunErrorCode::None)
        {
            outcome = Interrupted(RunOutcome{}, context.Interruption());
        }
        else if (context.session_id.empty() || context.run_id.empty())
        {
            outcome = {RunStatus::Failed, RunErrorCode::InvalidConfiguration, "run and session ids are required"};
        }
        else
        {
            const CompositionValidation validation = ValidateComposition(m_dependencies, profile);
            if (!validation.Ok())
            {
                outcome = {RunStatus::Failed, RunErrorCode::InvalidConfiguration, validation.Describe()};
            }
            else
            {
                // 准备本轮上下文：裁剪历史、召回记忆、构建 Provider 上下文。
                const auto prepared = m_dependencies.context_policy->Prepare(workspace.Conversation().messages,
                                                                             profile.max_context_chars);
                std::size_t used_chars = EstimateMessagesChars(prepared);
                std::string memory_context;
                BuildMemoryContext(m_dependencies, profile, context, user_input, used_chars, memory_context);
                if (!memory_context.empty())
                {
                    used_chars += TaggedWrapperChars(kMemoryTag) + memory_context.size();
                }

                std::string provider_context;
                std::string provider_error;
                const bool context_ok = BuildProviderContext(m_dependencies, profile, user_input, used_chars, context,
                                                             provider_context, provider_error);
                if (!context_ok)
                {
                    outcome = {RunStatus::Failed, RunErrorCode::ContextFailure,
                               "context provider failed: " + provider_error};
                }
                else
                {
                    outcome = RunSteps(m_dependencies, profile, workspace, context, notifier, memory_context,
                                       provider_context, outcome);
                }
            }
        }
    }
    catch (const std::exception& error)
    {
        outcome.status = RunStatus::Failed;
        outcome.error_code = RunErrorCode::InternalError;
        outcome.error = std::string("unhandled exception: ") + error.what();
    }
    catch (...)
    {
        outcome.status = RunStatus::Failed;
        outcome.error_code = RunErrorCode::InternalError;
        outcome.error = "unhandled unknown exception";
    }

    return outcome;
}

} // namespace agent::runtime
