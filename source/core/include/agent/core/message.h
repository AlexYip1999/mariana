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

/// 模型请求的工具调用；arguments 保存原始 JSON 字符串，由调用方决定是否解析。
struct ToolCall
{
    std::string id;
    std::string name;
    std::string arguments_json;
};

struct ChatMessage
{
    // 消息本体
    ChatRole role = ChatRole::User;
    std::string content;

    // 工具调用关联
    // Assistant 消息可能同时带 content 和 tool_calls。
    std::vector<ToolCall> tool_calls;
    // role == Tool 时填写，和发起调用的 ToolCall.id 对应。
    std::string tool_call_id;
};

} // namespace agent::core
