#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agent/runtime/agent_executor.h"
#include "agent/runtime/agent_run.h"
#include "agent/runtime/agent_session.h"
#include "agent/runtime/service_registry.h"

namespace agent::runtime
{

struct AgentId
{
    std::string value;

    bool Empty() const
    {
        return value.empty();
    }
};

struct AgentDescriptor
{
    AgentId id;
    std::string name;
    std::string version;
    AgentProfile profile;
    std::vector<std::string> capabilities;
};

/// 一个 Agent 的独立运行组合。
struct AgentServices
{
    std::shared_ptr<ServiceRegistry> services;
    std::shared_ptr<AgentExecutor> executor;
};

/// Agent = 身份/配置 + 独立会话 + 执行策略 + Agent 级服务。
class Agent
{
  public:
    Agent(AgentDescriptor descriptor, AgentServices services);

    AgentRunResult Send(const std::string& user_input);
    void ClearHistory();

    AgentSession& Session();
    const AgentSession& Session() const;
    const core::Conversation& History() const;

    const AgentDescriptor& Descriptor() const;
    const AgentId& Id() const;
    const AgentProfile& Profile() const;
    const AgentServices& Services() const;

    /// 同步 descriptor 中的初始 profile，保持 Agent 状态一致。
    void SetProfile(AgentProfile profile);

  private:
    AgentDescriptor m_descriptor;
    AgentServices m_services;
    AgentSession m_session;
};

} // namespace agent::runtime
