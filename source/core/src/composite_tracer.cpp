#include "agent/core/composite_tracer.h"

#include <utility>

namespace agent::core
{

void CompositeTracer::Add(std::shared_ptr<Tracer> tracer)
{
    if (tracer != nullptr)
    {
        m_tracers.push_back(std::move(tracer));
    }
}

void CompositeTracer::Clear()
{
    m_tracers.clear();
}

std::size_t CompositeTracer::Size() const
{
    return m_tracers.size();
}

void CompositeTracer::OnTurnStart()
{
    for (const std::shared_ptr<Tracer>& tracer : m_tracers)
    {
        tracer->OnTurnStart();
    }
}

void CompositeTracer::OnModelRequest(const ChatRequest& request)
{
    for (const std::shared_ptr<Tracer>& tracer : m_tracers)
    {
        tracer->OnModelRequest(request);
    }
}

void CompositeTracer::OnModelResponse(const ChatResponse& response)
{
    for (const std::shared_ptr<Tracer>& tracer : m_tracers)
    {
        tracer->OnModelResponse(response);
    }
}

void CompositeTracer::OnToolCall(const ToolCall& call)
{
    for (const std::shared_ptr<Tracer>& tracer : m_tracers)
    {
        tracer->OnToolCall(call);
    }
}

void CompositeTracer::OnToolResult(const ToolCall& call, const ToolResult& result)
{
    for (const std::shared_ptr<Tracer>& tracer : m_tracers)
    {
        tracer->OnToolResult(call, result);
    }
}

} // namespace agent::core
