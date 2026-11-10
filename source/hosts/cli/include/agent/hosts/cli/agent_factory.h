#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "agent/core/chat_model.h"
#include "agent/core/memory.h"
#include "agent/core/system_registry.h"
#include "agent/hosts/cli/cli_config.h"
#include "agent/runtime/agent.h"
#include "agent/runtime/agent_runner.h"
#include "agent/runtime/service_registry.h"
#include "agent/runtime/tool_registry.h"

namespace agent::hosts::cli
{

class ToolCatalog;

/// 从 Host 服务构造 executor 和 Agent；不负责 I/O、参数或循环。
class AgentFactory
{
  public:
    AgentFactory(CliConfig config, std::shared_ptr<core::ChatModel> model,
                 std::shared_ptr<core::SystemRegistry> systems, std::shared_ptr<ToolCatalog> tool_catalog,
                 std::shared_ptr<core::MemoryStore> memory = nullptr);

    std::shared_ptr<runtime::AgentRunner> CreateExecutor(const std::filesystem::path& workspace) const;

    runtime::Agent CreateAgent(std::shared_ptr<runtime::AgentExecutor> executor,
                               std::shared_ptr<runtime::ServiceRegistry> scope, const std::string& session_id) const;

  private:
    runtime::AgentProfile MakeProfile() const;
    runtime::RunnerDependencies MakeRunnerDependencies(const std::shared_ptr<runtime::ToolRegistry>& tools) const;

    // 配置
    CliConfig m_config;

    // 注入的共享依赖
    std::shared_ptr<core::ChatModel> m_model;
    std::shared_ptr<core::SystemRegistry> m_systems;
    std::shared_ptr<ToolCatalog> m_tool_catalog;
    // 长期记忆；非空时 MakeProfile 打开 enable_recall，Runner 获得同一 store
    std::shared_ptr<core::MemoryStore> m_memory;
};

} // namespace agent::hosts::cli
