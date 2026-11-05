#include "agent/runtime/agent_run.h"

namespace agent::runtime
{

core::TurnEvent TurnRecord::ToEvent() const
{
    core::TurnEvent event;
    event.session_id = session_id;
    event.run_id = run_id;
    event.status = ToString(status);
    event.error_code = ToString(error_code);
    event.error = error;
    event.steps = steps;
    event.tool_attempts = tool_attempts;
    event.ok = status == RunStatus::Completed;
    return event;
}

} // namespace agent::runtime
