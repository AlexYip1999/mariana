#pragma once

#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

#include "agent/core/call_context.h"
#include "agent/core/message.h"
#include "agent/core/tool.h"

namespace agent::core
{

/// 模型返回的 token 用量；具体字段由厂商适配器填充。
struct ChatUsage
{
    // token 计数
    std::int64_t prompt_tokens = 0;
    std::int64_t completion_tokens = 0;
    std::int64_t total_tokens = 0;

    // 上下文缓存命中
    std::int64_t prompt_cache_hit_tokens = 0;
    std::int64_t prompt_cache_miss_tokens = 0;
};

struct ChatRequest
{
    // 模型与输入
    std::string model;
    std::vector<ChatMessage> messages;
    std::vector<ToolSpec> tools;

    // 采样参数
    double temperature = 0.0;
    int max_tokens = 1024;

    // 单次调用上下文：取消与 deadline
    CallContext call;
};

struct ChatResponse
{
    // 结果
    bool ok = false;
    std::string content;
    std::vector<ToolCall> tool_calls;

    // 用量与结束原因
    ChatUsage usage;
    std::string finish_reason;

    // 错误
    std::string error;
    CallError error_code = CallError::None;
};

/// 模型适配抽象：换模型 / 厂商不改 core。
class ChatModel
{
  public:
    virtual ~ChatModel() = default;

    virtual ChatResponse Chat(const ChatRequest& request) = 0;
};

} // namespace agent::core
