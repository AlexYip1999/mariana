#include "agent/runtime/agent_runner.h"

#include <algorithm>
#include <exception>
#include <set>
#include <stdexcept>
#include <utility>

#include "agent/runtime/composition.h"
#include "agent/runtime/system_notifier.h"
#include "agent/runtime/turn_transaction.h"

namespace agent::runtime
{
namespace
{

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

} // namespace

static RunOutcome RunSteps(const RunnerDependencies& dependencies, const AgentProfile& profile,
                           TurnWorkspace& workspace, const RunContext& context, const SystemNotifier& notifier,
                           RunOutcome& result)
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
        core::ChatRequest request;
        request.call = context.call;
        request.model = profile.model;
        request.temperature = profile.temperature;
        request.max_tokens = profile.max_tokens;
        request.tools = specs;
        request.messages = dependencies.context_policy->Prepare(conversation.messages, profile.max_context_chars);
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
    return DescribeCompositionIssues(validation);
}

std::shared_ptr<core::SystemRegistry> AgentRunner::SystemRegistry() const
{
    return m_dependencies.systems;
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
                outcome = {RunStatus::Failed, RunErrorCode::InvalidConfiguration,
                           DescribeCompositionIssues(validation)};
            }
            else
            {
                outcome = RunSteps(m_dependencies, profile, workspace, context, notifier, outcome);
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
