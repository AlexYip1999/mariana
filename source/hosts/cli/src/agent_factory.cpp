#include "agent/hosts/cli/agent_factory.h"

#include <stdexcept>
#include <utility>

#include "agent/core/version.h"
#include "agent/hosts/cli/tool_catalog.h"
#include "agent/runtime/agent_runner.h"

namespace agent::hosts::cli
{

AgentFactory::AgentFactory(CliConfig config, std::shared_ptr<core::ChatModel> model,
                           std::shared_ptr<core::SystemRegistry> systems, std::shared_ptr<ToolCatalog> tool_catalog,
                           std::shared_ptr<core::MemoryStore> memory)
    : m_config(std::move(config))
    , m_model(std::move(model))
    , m_systems(std::move(systems))
    , m_tool_catalog(std::move(tool_catalog))
    , m_memory(std::move(memory))
{}

std::shared_ptr<runtime::AgentRunner> AgentFactory::CreateExecutor(const std::filesystem::path& workspace) const
{
    std::string error;
    auto tools = m_tool_catalog->CreateRegistry(workspace, error);
    if (!tools)
    {
        throw std::runtime_error(error);
    }
    return std::make_shared<runtime::AgentRunner>(MakeRunnerDependencies(tools));
}

runtime::Agent AgentFactory::CreateAgent(std::shared_ptr<runtime::AgentExecutor> executor,
                                         std::shared_ptr<runtime::ServiceRegistry> scope,
                                         const std::string& session_id) const
{
    runtime::AgentDescriptor descriptor;
    descriptor.id.value = "cli";
    descriptor.name = "agent-cli";
    descriptor.version = core::Version();

    runtime::AgentDependencies dependencies;
    dependencies.services = std::move(scope);
    dependencies.executor = std::move(executor);
    return runtime::Agent(std::move(descriptor), MakeProfile(), std::move(dependencies), session_id);
}

runtime::AgentProfile AgentFactory::MakeProfile() const
{
    runtime::AgentProfile profile;
    profile.model = m_config.model;
    profile.system_prompt = m_config.system_prompt;
    profile.temperature = m_config.temperature;
    profile.max_tokens = m_config.max_tokens;
    profile.max_steps = m_config.max_steps;
    profile.turn_timeout_ms = m_config.turn_timeout_ms;
    profile.max_tool_calls = m_config.max_tool_calls;
    profile.max_tool_output_bytes = m_config.max_tool_output_bytes;
    profile.max_context_chars = m_config.max_context_chars;
    profile.tool_names = m_config.tool_names;
    if (m_memory)
    {
        // 记忆开启时每轮自动召回；作用域固定为 CLI 共享 scope，跨 session 复用同一 JSONL。
        profile.memory_policy.enable_recall = true;
        profile.memory_policy.recall_scopes = {CliMemoryScope()};
    }
    return profile;
}

runtime::RunnerDependencies AgentFactory::MakeRunnerDependencies(
    const std::shared_ptr<runtime::ToolRegistry>& tools) const
{
    runtime::RunnerDependencies dependencies;
    dependencies.model = m_model;
    dependencies.tools = tools;
    dependencies.tool_policy = std::make_shared<runtime::AllowAllToolPolicy>();
    dependencies.context_policy = std::make_shared<runtime::KeepRecentTurnsPolicy>();
    dependencies.tool_output_policy = std::make_shared<runtime::TruncateToolOutputPolicy>();
    dependencies.tool_scheduler = std::make_shared<runtime::InlineToolScheduler>();
    dependencies.systems = m_systems;
    dependencies.memory = m_memory;
    return dependencies;
}

} // namespace agent::hosts::cli
