#include "agent/runtime/system_notifier.h"

namespace agent::runtime
{

void SystemNotifier::TurnStarted(const core::TurnEvent& event) const
{
    Notify(
        [&event](core::AgentSystem& system)
        {
            system.OnTurnStarted(event);
        });
}

void SystemNotifier::ModelRequested(const core::ChatRequest& request) const
{
    Notify(
        [&request](core::AgentSystem& system)
        {
            system.OnModelRequest(request);
        });
}

void SystemNotifier::ModelResponded(const core::ChatResponse& response) const
{
    Notify(
        [&response](core::AgentSystem& system)
        {
            system.OnModelResponse(response);
        });
}

void SystemNotifier::ToolCalled(const core::ToolCall& call) const
{
    Notify(
        [&call](core::AgentSystem& system)
        {
            system.OnToolCall(call);
        });
}

void SystemNotifier::ToolFinished(const core::ToolCall& call, const core::ToolResult& result) const
{
    Notify(
        [&call, &result](core::AgentSystem& system)
        {
            system.OnToolResult(call, result);
        });
}

void SystemNotifier::ToolReplayed(const core::ToolCall& call, const core::ToolResult& result) const
{
    Notify(
        [&call, &result](core::AgentSystem& system)
        {
            system.OnToolReplayed(call, result);
        });
}

void SystemNotifier::TurnFinished(const core::TurnEvent& event) const
{
    Notify(
        [&event](core::AgentSystem& system)
        {
            system.OnTurnEnded(event);
        });
}

} // namespace agent::runtime
