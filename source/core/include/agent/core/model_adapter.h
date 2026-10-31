#pragma once

#include <string>
#include <vector>

namespace agent::core
{

enum class ChatRole
{
    System,
    User,
    Assistant,
    Tool,
};

struct ChatMessage
{
    ChatRole role = ChatRole::User;
    std::string content;
};

struct ChatRequest
{
    std::string model;
    std::vector<ChatMessage> messages;
    double temperature = 0.0;
    int max_tokens = 1024;
};

struct ChatResponse
{
    bool ok = false;
    std::string content;
    std::string error;
};

/// 模型适配抽象：换模型 / 厂商不改 core。
class ModelAdapter
{
  public:
    virtual ~ModelAdapter() = default;

    virtual ChatResponse Chat(const ChatRequest& request) = 0;
};

} // namespace agent::core
