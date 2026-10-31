#pragma once

#include <string>
#include <utility>
#include <vector>

#include "agent/core/message.h"

namespace agent::core
{

using SessionId = std::string;

/// 会话原语：只保存消息，不包含循环/执行逻辑。
struct Conversation
{
    SessionId session_id;
    std::vector<ChatMessage> messages;

    void Append(ChatMessage message)
    {
        messages.push_back(std::move(message));
    }

    void Clear()
    {
        messages.clear();
    }
};

} // namespace agent::core
