#pragma once

#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "agent/core/stats_view.h"
#include "agent/core/system_registry.h"
#include "agent/hosts/cli/cli_config.h"
#include "agent/runtime/agent.h"
#include "agent/runtime/tool_registry.h"

namespace agent::hosts::cli
{

enum class CommandResult
{
    Handled,
    Exit,
    NotFound,
};

/// 交互命令可以修改/读取的 host 状态。
struct CliContext
{
    CliConfig config;
    std::filesystem::path workspace;
    bool tools_enabled = true;
    std::string trace_dir = "traces";
    std::string current_trace_path;
    std::shared_ptr<runtime::ToolRegistry> tools;
    std::shared_ptr<core::SystemRegistry> systems;
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
