#pragma once

#include <string>
#include <vector>

#include "agent/core/message.h"
#include "agent/core/tool.h"

namespace agent::core
{

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
