#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "agent/core/tracer.h"

namespace agent::core
{

/// 多观察者 Tracer：metrics、日志、UI 等通过 Add 接入，Session 只依赖 Tracer。
class CompositeTracer : public Tracer
{
  public:
    void Add(std::shared_ptr<Tracer> tracer);
    void Clear();

    std::size_t Size() const;

    void OnTurnStart() override;
    void OnModelRequest(const ChatRequest& request) override;
    void OnModelResponse(const ChatResponse& response) override;
    void OnToolCall(const ToolCall& call) override;
    void OnToolResult(const ToolCall& call, const ToolResult& result) override;

  private:
    std::vector<std::shared_ptr<Tracer>> m_tracers;
};

} // namespace agent::core
