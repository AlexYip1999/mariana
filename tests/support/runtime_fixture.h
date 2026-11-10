#pragma once

#include "agent/runtime/agent_runner.h"
#include "agent/runtime/service_registry.h"
#include "agent/runtime/system_notifier.h"
#include "agent/runtime/turn_executor.h"

namespace agent::tests
{

// Test fixtures explicitly keep stack fakes alive longer than the executor/session under test.
template <typename T> std::shared_ptr<T> BorrowForTest(T& value)
{
    return std::shared_ptr<T>(&value,
                              [](T*)
                              {
                              });
}

inline runtime::RunnerDependencies MakeRunnerDependencies(core::ChatModel& model, runtime::ServiceRegistry& services)
{
    runtime::RunnerDependencies dependencies;
    dependencies.model = BorrowForTest(model);

    const auto tools = services.Get<runtime::ToolRegistry>();
    dependencies.tools = tools ? tools : std::make_shared<runtime::ToolRegistry>();

    const auto tool_policy = services.Get<runtime::ToolPolicy>();
    dependencies.tool_policy = tool_policy ? tool_policy : std::make_shared<runtime::AllowAllToolPolicy>();
    const auto context_policy = services.Get<runtime::ContextPolicy>();
    dependencies.context_policy = context_policy ? context_policy : std::make_shared<runtime::KeepRecentTurnsPolicy>();
    const auto output_policy = services.Get<runtime::ToolOutputPolicy>();
    dependencies.tool_output_policy =
        output_policy ? output_policy : std::make_shared<runtime::TruncateToolOutputPolicy>();

    dependencies.tool_scheduler = std::make_shared<runtime::InlineToolScheduler>();
    const auto systems = services.Get<core::SystemRegistry>();
    dependencies.systems = systems ? systems : std::make_shared<core::SystemRegistry>();
    return dependencies;
}

inline runtime::AgentRunResult RunTurnForTest(runtime::AgentRunner& runner, const runtime::AgentProfile& profile,
                                              core::Conversation& conversation, const std::string& input,
                                              std::shared_ptr<core::SystemRegistry> systems = nullptr)
{
    const runtime::RunContext context = runtime::RunContext::Create(profile, conversation.session_id);
    core::SystemRegistry empty_systems;
    runtime::TurnExecution execution =
        runtime::ExecuteTurn(profile, conversation, runner, input, context, systems ? *systems : empty_systems);

    if (execution.history)
    {
        conversation = std::move(*execution.history);
    }
    if (systems)
    {
        runtime::SystemNotifier(*systems).TurnFinished(execution.result.ToEvent());
    }
    return execution.result;
}

} // namespace agent::tests
