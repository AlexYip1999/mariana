#pragma once

#include <string>

namespace agent::core
{

/// 上下文构建请求：只携带最小标识，具体字段由 application/provider 约定。
struct AgentContextRequest
{
    std::string application;
    std::string session_id;
    std::string user_input;
};

struct AgentContextResult
{
    bool ok = true;
    // 上下文 JSON 字符串；core 不解析。
    std::string context_json;
    std::string error;
};

/// 上下文提供者：引擎状态、NPC 世界状态、检索结果等都由具体 host 实现。
class ContextProvider
{
  public:
    virtual ~ContextProvider() = default;

    virtual AgentContextResult BuildContext(const AgentContextRequest& request) = 0;
};

} // namespace agent::core
