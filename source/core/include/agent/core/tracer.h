#pragma once

#include "agent/core/chat_model.h"
#include "agent/core/message.h"
#include "agent/core/tool.h"

namespace agent::core
{

/// 可观测性接口：编排层发出事件，具体实现决定写日志、文件或上报。
class Tracer
{
  public:
    virtual ~Tracer() = default;

    virtual void OnModelRequest(const ChatRequest& request) = 0;
    virtual void OnModelResponse(const ChatResponse& response) = 0;
    virtual void OnToolCall(const ToolCall& call) = 0;
    virtual void OnToolResult(const ToolCall& call, const ToolResult& result) = 0;
};

/// 默认空实现：不需要观测时直接注入，避免各处判空。
class NullTracer : public Tracer
{
  public:
    void OnModelRequest(const ChatRequest&) override
    {
    }
    void OnModelResponse(const ChatResponse&) override
    {
    }
    void OnToolCall(const ToolCall&) override
    {
    }
    void OnToolResult(const ToolCall&, const ToolResult&) override
    {
    }
};

} // namespace agent::core
