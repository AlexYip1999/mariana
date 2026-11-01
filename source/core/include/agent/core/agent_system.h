#pragma once

#include <string>

#include "agent/core/chat_model.h"
#include "agent/core/tool.h"

namespace agent::core
{

/// Agent loop 的系统/参与者：默认空实现，具体 system 按需 override。
class AgentSystem
{
  public:
    virtual ~AgentSystem() = default;

    virtual void OnTurnStart() {}

    virtual void OnModelRequest(const ChatRequest&) {}

    virtual void OnModelResponse(const ChatResponse&) {}

    virtual void OnToolCall(const ToolCall&) {}

    virtual void OnToolResult(const ToolCall&, const ToolResult&) {}

    virtual void OnTurnEnd(bool, const std::string&, int) {}
};

} // namespace agent::core
