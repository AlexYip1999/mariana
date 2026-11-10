#include "agent/runtime/run_context.h"

#include <chrono>
#include <limits>
#include <stdexcept>

#include "agent/runtime/execution_id.h"

namespace agent::runtime
{

RunContext RunContext::Create(const AgentProfile& profile, const std::string& session_id, const RunOptions& options)
{
    const auto timeout = options.timeout.value_or(std::chrono::milliseconds(profile.turn_timeout_ms));
    if (timeout.count() <= 0 || timeout.count() > std::numeric_limits<int>::max())
    {
        throw std::invalid_argument("turn timeout must be a positive supported millisecond value");
    }

    RunContext context;
    context.session_id = session_id.empty() ? ExecutionId::NewSession() : session_id;
    context.run_id = options.run_id.empty() ? ExecutionId::NewRun() : options.run_id;
    context.call.stop_token = options.stop_token;
    context.call.deadline = std::chrono::steady_clock::now() + timeout;
    context.budget.max_model_calls = profile.max_steps;
    context.budget.max_tool_calls = profile.max_tool_calls;
    context.budget.max_tool_output_bytes = profile.max_tool_output_bytes;
    return context;
}

} // namespace agent::runtime
