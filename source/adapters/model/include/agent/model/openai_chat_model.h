#pragma once

#include <string>

#include "agent/core/chat_model.h"
#include "agent/transport/http_transport.h"

namespace agent::model
{

struct OpenAiConfig
{
    std::string endpoint = "https://api.deepseek.com/chat/completions";
    std::string model = "deepseek-flash";
    std::string api_key;
    int timeout_ms = 30000;
};

/// OpenAI 兼容协议适配器（DeepSeek 也走这套），非流式 /chat/completions。
class OpenAiChatModel : public core::ChatModel
{
  public:
    OpenAiChatModel(transport::HttpTransport& transport, OpenAiConfig config);

    core::ChatResponse Chat(const core::ChatRequest& request) override;

  private:
    transport::HttpTransport& m_transport;
    OpenAiConfig m_config;
};

} // namespace agent::model
