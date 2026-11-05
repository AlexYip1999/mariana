#include "agent/runtime/turn_executor.h"

#include <exception>
#include <utility>

#include "agent/runtime/system_notifier.h"
#include "agent/runtime/turn_transaction.h"

namespace agent::runtime
{

TurnExecution ExecuteTurn(const AgentProfile& profile, core::Conversation& staged, AgentExecutor& executor,
                          const std::string& user_input, const RunContext& context, core::SystemRegistry& systems)
{
    TurnTransaction transaction(staged, context);
    transaction.Start(profile, user_input);
    TurnWorkspace workspace(staged, context);

    core::TurnEvent started_event;
    started_event.session_id = context.session_id;
    started_event.run_id = context.run_id;
    SystemNotifier(systems).TurnStarted(started_event);

    RunOutcome outcome;
    try
    {
        outcome = executor.RunTurn(profile, workspace, user_input, context);
    }
    catch (const std::invalid_argument& error)
    {
        outcome = {RunStatus::Failed, RunErrorCode::InvalidConfiguration, error.what()};
    }
    catch (const std::exception& error)
    {
        outcome = {RunStatus::Failed, RunErrorCode::InternalError, error.what()};
    }
    catch (...)
    {
        outcome = {RunStatus::Failed, RunErrorCode::InternalError, "executor failed"};
    }

    TurnCommit commit = transaction.Finish(outcome);
    return {std::move(commit.result), std::move(commit.history)};
}

} // namespace agent::runtime
