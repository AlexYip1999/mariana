#pragma once

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "agent/core/memory.h"
#include "agent/core/stats_view.h"
#include "agent/core/system_registry.h"
#include "agent/hosts/cli/cli_config.h"
#include "agent/runtime/agent.h"
#include "agent/runtime/tool_registry.h"

namespace agent::hosts::cli
{

class ToolCatalog;

/// 根据 workspace 构造一个 AgentExecutor（/workspace 命令会重新创建）。
using ExecutorFactory = std::function<std::shared_ptr<runtime::AgentExecutor>(const std::filesystem::path&)>;

enum class CommandResult
{
    Handled,
    Exit,
    NotFound,
};

/// 交互命令可以修改/读取的 host 状态。
struct CliContext
{
    // 配置与工作区
    CliConfig config;
    std::filesystem::path workspace;

    // trace
    std::string trace_dir = "traces";
    std::string current_trace_path;

    // executor 工厂
    ExecutorFactory create_executor;

    // 工具目录，/tools 命令使用
    std::shared_ptr<const ToolCatalog> tool_catalog;

    // 长期记忆（空 = 未启用；/memory 命令读写）
    std::shared_ptr<core::MemoryStore> memory;
    std::string memory_path;

    // 观测与展示
    std::shared_ptr<core::StatsView> stats;
    bool auto_stats = false;
};

class CliCommand
{
  public:
    virtual ~CliCommand() = default;

    virtual std::string Name() const = 0;
    virtual std::string Usage() const = 0;
    virtual CommandResult Execute(CliContext& context, runtime::Agent& agent, const std::string& args,
                                  std::ostream& out, std::ostream& err) = 0;
};

class CommandRegistry
{
  public:
    void Add(std::shared_ptr<CliCommand> command);

    CommandResult Execute(const std::string& line, CliContext& context, runtime::Agent& agent, std::ostream& out,
                          std::ostream& err) const;

    std::string HelpText() const;

  private:
    std::vector<std::shared_ptr<CliCommand>> m_commands;
};

void RegisterDefaultCommands(CommandRegistry& registry);

} // namespace agent::hosts::cli
