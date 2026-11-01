#pragma once

#include <memory>
#include <string>

#include "agent/core/chat_model.h"
#include "agent/core/conversation.h"
#include "agent/core/tracer.h"
#include "agent/runtime/service_registry.h"

namespace agent::runtime
{

struct AgentSessionConfig
{
    std::string model;
    std::string system_prompt;
    double temperature = 0.0;
    int max_tokens = 1024;
};

struct AgentRunResult
{
    bool ok = false;
    std::string content;
    std::string error;
};

/// Session 的服务依赖；新增服务在这里追加可选字段，避免构造函数参数膨胀。
struct AgentSessionDependencies
{
    core::ChatModel* model = nullptr;
    ServiceRegistry* services = nullptr;
    // 未来：ToolRegistry* tools；ContextProvider* context；AgentExecutor* executor。
};

/// 一次 Agent 会话：维护 Conversation，并执行单轮 RunTurn。
/// 当前直接调用 ChatModel；接入 AgentRunner 后内部切换到 AgentExecutor。
class AgentSession
{
  public:
    AgentSession(AgentSessionConfig config, AgentSessionDependencies dependencies);

    AgentRunResult RunTurn(const std::string& user_input);
    void ClearHistory();

    const core::Conversation& History() const;

  private:
    void AppendSystemPrompt();

    AgentSessionConfig m_config;
    AgentSessionDependencies m_dependencies;
    std::shared_ptr<core::Tracer> m_tracer;
    core::Conversation m_conversation;
};

} // namespace agent::runtime
