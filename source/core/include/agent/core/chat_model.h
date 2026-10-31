#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "agent/core/message.h"
#include "agent/core/tool.h"

namespace agent::core
{

/// 模型返回的 token 用量；具体字段由厂商适配器填充。
struct ChatUsage
{
    std::int64_t prompt_tokens = 0;
    std::int64_t completion_tokens = 0;
    std::int64_t total_tokens = 0;
    std::int64_t prompt_cache_hit_tokens = 0;
    std::int64_t prompt_cache_miss_tokens = 0;
};

struct ChatRequest
{
    std::string model;
    std::vector<ChatMessage> messages;
    std::vector<ToolSpec> tools;
    double temperature = 0.0;
    int max_tokens = 1024;
};

struct ChatResponse
{
    bool ok = false;
    std::string content;
    std::vector<ToolCall> tool_calls;
    ChatUsage usage;
    std::string error;
};

/// 模型适配抽象：换模型 / 厂商不改 core。
class ChatModel
{
  public:
    virtual ~ChatModel() = default;

    virtual ChatResponse Chat(const ChatRequest& request) = 0;
};

} // namespace agent::core
