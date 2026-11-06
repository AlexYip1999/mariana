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
    // 身份
    AgentId id;
    std::string name;
    std::string version;

    // 能力声明
    std::vector<std::string> capabilities;
};

/// Agent 私有服务作用域：HostScope 的 child registry，生命周期跟随 Agent。
using AgentScope = ServiceRegistry;

/// 一个 Agent 的独立运行组合。
struct AgentDependencies
{
    // AgentScope；为空时 Agent 创建一个空 scope。
    std::shared_ptr<AgentScope> services;

    // 构造后交给 AgentSession 持有，Agent 不重复持有 executor。
    std::shared_ptr<AgentExecutor> executor;
};

/// Agent = 身份 + 初始配置 + 独立会话 + 执行策略 + Agent 级服务。
class Agent
{
  public:
    Agent(AgentDescriptor descriptor, AgentProfile initial_profile, AgentDependencies dependencies,
          std::string session_id = {});

    AgentRunResult Send(const std::string& user_input, const RunOptions& options = {});
    AgentRunResult Send(const std::string& user_input, std::stop_token stop_token);
    bool ClearHistory();

    const AgentSession& Session() const;
    const std::string& SessionId() const;
    core::Conversation History() const;

    const AgentDescriptor& Descriptor() const;
    const AgentId& Id() const;
    AgentProfile Profile() const;
    AgentDependencies Dependencies() const;

    /// Only Session owns mutable configuration; Descriptor retains immutable creation defaults.
    bool SetProfile(AgentProfile profile);
    bool Reconfigure(AgentProfile profile, std::shared_ptr<AgentExecutor> executor);

  private:
    const AgentDescriptor m_descriptor;
    std::shared_ptr<AgentScope> m_scope;
    AgentSession m_session;
};

} // namespace agent::runtime
