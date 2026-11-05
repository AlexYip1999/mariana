#include "agent/runtime/agent_run.h"

#include <atomic>
#include <limits>
#include <stdexcept>

namespace agent::runtime
{

std::string NewExecutionId(const char* prefix)
{
    static std::atomic<unsigned long long> sequence{0};
    static const auto epoch = std::chrono::system_clock::now().time_since_epoch().count();
    return std::string(prefix) + "-" + std::to_string(epoch) + "-" + std::to_string(++sequence);
}

RunContext MakeRunContext(const AgentProfile& profile, const std::string& session_id, const RunOptions& options)
{
    const auto timeout = options.timeout.value_or(std::chrono::milliseconds(profile.turn_timeout_ms));
    if (timeout.count() <= 0 || timeout.count() > std::numeric_limits<int>::max())
    {
        throw std::invalid_argument("turn timeout must be a positive supported millisecond value");
    }
    RunContext context;
    context.session_id = session_id.empty() ? NewExecutionId("session") : session_id;
    context.run_id = NewExecutionId("run");
    context.call.stop_token = options.stop_token;
    context.call.deadline = std::chrono::steady_clock::now() + timeout;
    context.budget.max_model_calls = profile.max_steps;
    context.budget.max_tool_calls = profile.max_tool_calls;
    context.budget.max_tool_output_bytes = profile.max_tool_output_bytes;
    return context;
}

core::TurnEvent MakeTurnEvent(const TurnRecord& record)
{
    core::TurnEvent event;
    event.session_id = record.session_id;
    event.run_id = record.run_id;
    event.status = ToString(record.status);
    event.error_code = ToString(record.error_code);
    event.error = record.error;
    event.steps = record.steps;
    event.tool_attempts = record.tool_attempts;
    event.ok = record.status == RunStatus::Completed;
    return event;
}

} // namespace agent::runtime
