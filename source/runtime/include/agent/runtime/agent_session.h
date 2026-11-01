#pragma once

#include <string>
#include <vector>

#include "agent/core/chat_model.h"
#include "agent/core/conversation.h"
#include "agent/core/system_registry.h"
#include "agent/runtime/agent_run.h"
#include "agent/runtime/tool_registry.h"

namespace agent::runtime
{

struct AgentSessionConfig
{
    std::string model;
    std::string system_prompt;
    double temperature = 0.0;
    int max_tokens = 1024;
    int max_steps = 8;
    std::vector<std::string> tool_names;
};

/// Session 的服务依赖；新增服务在这里追加可选字段，避免构造函数参数膨胀。
struct AgentSessionDependencies
{
    core::ChatModel* model = nullptr;
    ToolRegistry* tools = nullptr;
    core::SystemRegistry* systems = nullptr;
    // 未来：ContextProvider* context；AgentExecutor* executor。
};

/// 一次 Agent 会话：维护 Conversation，并执行单轮 RunTurn。
/// 有 ToolRegistry 时走 AgentRunner 工具循环，否则保持单次模型调用。
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
    ToolRegistry m_empty_tools;
    core::SystemRegistry* m_systems = nullptr;
    core::Conversation m_conversation;
};

} // namespace agent::runtime
