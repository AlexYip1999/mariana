#pragma once

#include <cstddef>
#include <string>

#include "agent/core/call_context.h"

namespace agent::core
{

/// 上下文构建请求：只携带最小标识与本轮预算，具体字段由 application/provider 约定。
struct AgentContextRequest
{
    // 关联
    std::string application;
    std::string session_id;
    std::string run_id;

    // 本轮输入
    std::string user_input;

    // 本轮可注入字符上限；0 = 不限（仅当 profile.max_context_chars = 0 时出现）。
    // 预算已扣除消息包装开销，Provider 返回内容超过上限会被截断。
    std::size_t max_context_chars = 0;

    // 单次调用上下文：Provider 查询世界/检索时应协作取消与 deadline。
    CallContext call;
};

struct AgentContextResult
{
    bool ok = true;
    // 上下文 JSON 字符串；core 不解析。
    std::string context_json;
    std::string error;
};

/// 上下文提供者：引擎状态、NPC 世界状态、检索结果等都由具体 host 实现。
/// Runner 每轮调用一次，把结果作为一条 System 消息注入模型请求（位置见 docs/context-provider.md）。
class ContextProvider
{
  public:
    virtual ~ContextProvider() = default;

    virtual AgentContextResult BuildContext(const AgentContextRequest& request) = 0;
};

} // namespace agent::core
