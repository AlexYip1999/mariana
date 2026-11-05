#pragma once

#include <cstddef>
#include <string>

#include "agent/core/chat_model.h"
#include "agent/core/tool.h"

namespace agent::core
{

/// 一轮 Turn 的结构化观察事件；core 不引用 runtime 的 RunStatus / RunErrorCode。
struct TurnEvent
{
    // 关联
    std::string session_id;
    std::string run_id;

    // 结束状态；status / error_code 使用稳定字符串，跨层不需要共享枚举。
    std::string status;
    std::string error_code;
    std::string error;
    int steps = 0;
    std::size_t tool_attempts = 0;
    bool ok = false;
    bool replayed = false;
};

/// Agent loop 的系统/参与者：默认空实现，具体 system 按需 override。
class AgentSystem
{
  public:
    virtual ~AgentSystem() = default;

    virtual void OnTurnStart() {}

    /// 结构化 Turn 开始；默认桥接到旧 OnTurnStart，兼容已有观察者。
    virtual void OnTurnStarted(const TurnEvent&)
    {
        OnTurnStart();
    }

    virtual void OnModelRequest(const ChatRequest&) {}

    virtual void OnModelResponse(const ChatResponse&) {}

    virtual void OnToolCall(const ToolCall&) {}

    virtual void OnToolResult(const ToolCall&, const ToolResult&) {}

    /// 缓存重放不重新执行工具；默认仍按普通工具事件上报。
    virtual void OnToolReplayed(const ToolCall& call, const ToolResult& result)
    {
        OnToolCall(call);
        OnToolResult(call, result);
    }

    virtual void OnTurnEnd(bool, const std::string&, int) {}

    /// 结构化 Turn 结束；默认桥接到旧 OnTurnEnd，兼容已有观察者。
    virtual void OnTurnEnded(const TurnEvent& event)
    {
        OnTurnEnd(event.ok, event.error, event.steps);
    }
};

} // namespace agent::core
