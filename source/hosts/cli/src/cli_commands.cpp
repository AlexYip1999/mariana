#include "agent/hosts/cli/cli_command.h"

#include <filesystem>
#include <memory>
#include <ostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "agent/core/version.h"
#include "agent/plugins/tools/read_file_tool.h"
#include "agent/plugins/traces/trace_usage.h"
#include "agent/plugins/traces/trace_viewer.h"

namespace agent::hosts::cli
{

namespace
{

std::string Trim(std::string value)
{
    const std::size_t first = value.find_first_not_of(" \t");
    if (first == std::string::npos)
    {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t");
    return value.substr(first, last - first + 1);
}

std::pair<std::string, std::string> SplitCommand(const std::string& line)
{
    const std::size_t separator = line.find(' ');
    if (separator == std::string::npos)
    {
        return {line, {}};
    }
    return {line.substr(0, separator), Trim(line.substr(separator + 1))};
}

bool RebuildTools(CliContext& context, runtime::Agent& agent, std::ostream& err)
{
    context.tools->Clear();
    if (context.tools_enabled)
    {
        std::string tool_error;
        if (!context.tools->Register(std::make_shared<plugins::tools::ReadFileTool>(context.workspace), tool_error))
        {
            err << "tool registration failed: " << tool_error << "\n";
            return false;
        }
    }

    runtime::AgentProfile profile = agent.Profile();
    profile.tool_names = context.tools_enabled ? std::vector<std::string>{"read_file"} : std::vector<std::string>{};
    agent.SetProfile(std::move(profile));
    return true;
}

std::filesystem::path ResolveTracePath(const CliContext& context, const std::string& value)
{
    if (value.empty())
    {
        return context.current_trace_path;
    }

    const std::filesystem::path path(value);
    if (std::filesystem::is_regular_file(path))
    {
        return path;
    }

    const std::filesystem::path in_trace_dir = std::filesystem::path(context.trace_dir) / value;
    if (std::filesystem::is_regular_file(in_trace_dir))
    {
        return in_trace_dir;
    }
    return path;
}

void PrintConfig(std::ostream& out, const CliConfig& config)
{
    out << "config:\n"
        << "  provider=" << config.provider << "\n"
        << "  endpoint=" << config.endpoint << "\n"
        << "  model=" << config.model << "\n"
        << "  api_key_env=" << config.api_key_env << "\n"
        << "  max_tokens=" << config.max_tokens << "\n"
        << "  max_steps=" << config.max_steps << "\n"
        << "  max_context_chars=" << config.max_context_chars << "\n"
        << "  temperature=" << config.temperature << "\n"
        << "  cost_currency=" << config.cost_currency << "\n";
}

class HelpCommand final : public CliCommand
{
  public:
    explicit HelpCommand(const CommandRegistry* registry)
        : m_registry(registry)
    {}

    std::string Name() const override
    {
        return "help";
    }

    std::string Usage() const override
    {
        return "/help";
    }

    CommandResult Execute(CliContext&, runtime::Agent&, const std::string&, std::ostream& out, std::ostream&) override
    {
        out << m_registry->HelpText();
        return CommandResult::Handled;
    }

  private:
    const CommandRegistry* m_registry;
};

class VersionCommand final : public CliCommand
{
  public:
    std::string Name() const override
    {
        return "version";
    }

    std::string Usage() const override
    {
        return "/version";
    }

    CommandResult Execute(CliContext&, runtime::Agent&, const std::string&, std::ostream& out, std::ostream&) override
    {
        out << core::Version() << "\n";
        return CommandResult::Handled;
    }
};

class ConfigCommand final : public CliCommand
{
  public:
    std::string Name() const override
    {
        return "config";
    }

    std::string Usage() const override
    {
        return "/config show|init";
    }

    CommandResult Execute(CliContext& context, runtime::Agent&, const std::string& args, std::ostream& out,
                          std::ostream& err) override
    {
        if (args == "show")
        {
            PrintConfig(out, context.config);
            return CommandResult::Handled;
        }
        if (args == "init")
        {
            const ConfigInitResult result = WriteDefaultCliConfigFile("config.local.json");
            if (!result.ok)
            {
                err << "init config failed: " << result.error << "\n";
            }
            else
            {
                out << "created " << result.path << "\n";
            }
            return CommandResult::Handled;
        }
        out << "usage: /config show|init\n";
        return CommandResult::Handled;
    }
};

class WorkspaceCommand final : public CliCommand
{
  public:
    std::string Name() const override
    {
        return "workspace";
    }

    std::string Usage() const override
    {
        return "/workspace <path>";
    }

    CommandResult Execute(CliContext& context, runtime::Agent& agent, const std::string& args, std::ostream& out,
                          std::ostream& err) override
    {
        if (args.empty())
        {
            out << "workspace: " << context.workspace.string() << "\n";
            return CommandResult::Handled;
        }

        std::error_code error;
        const std::filesystem::path workspace = std::filesystem::weakly_canonical(args, error);
        if (error || !std::filesystem::is_directory(workspace, error))
        {
            err << "invalid workspace: " << args << "\n";
            return CommandResult::Handled;
        }
        context.workspace = workspace;
        if (RebuildTools(context, agent, err))
        {
            out << "workspace: " << workspace.string() << "\n";
        }
        return CommandResult::Handled;
    }
};

class ToolsCommand final : public CliCommand
{
  public:
    std::string Name() const override
    {
        return "tools";
    }

    std::string Usage() const override
    {
        return "/tools on|off";
    }

    CommandResult Execute(CliContext& context, runtime::Agent& agent, const std::string& args, std::ostream& out,
                          std::ostream& err) override
    {
        if (args != "on" && args != "off")
        {
            out << "usage: /tools on|off\n";
            return CommandResult::Handled;
        }
        context.tools_enabled = args == "on";
        if (RebuildTools(context, agent, err))
        {
            out << "tools: " << (context.tools_enabled ? "on" : "off") << "\n";
        }
        return CommandResult::Handled;
    }
};

class TraceCommand final : public CliCommand
{
  public:
    std::string Name() const override
    {
        return "trace";
    }

    std::string Usage() const override
    {
        return "/trace [show [file]]";
    }

    CommandResult Execute(CliContext& context, runtime::Agent&, const std::string& args, std::ostream& out,
                          std::ostream& err) override
    {
        const std::pair<std::string, std::string> trace_command = SplitCommand(args);
        std::string requested_path;
        if (args.empty())
        {
            requested_path = context.current_trace_path;
        }
        else if (trace_command.first == "show")
        {
            requested_path = trace_command.second;
        }
        else
        {
            out << "usage: " << Usage() << "\n";
            return CommandResult::Handled;
        }

        std::string error;
        if (!plugins::traces::RenderTraceFile(ResolveTracePath(context, requested_path), out, error))
        {
            err << error << "\n";
        }
        return CommandResult::Handled;
    }
};

class StatsCommand final : public CliCommand
{
  public:
    std::string Name() const override
    {
        return "stats";
    }

    std::string Usage() const override
    {
        return "/stats [auto on|off | today|all]";
    }

    CommandResult Execute(CliContext& context, runtime::Agent&, const std::string& args, std::ostream& out,
                          std::ostream& err) override
    {
        if (args == "auto on" || args == "auto off")
        {
            context.auto_stats = args == "auto on";
            out << "stats auto: " << (context.auto_stats ? "on" : "off") << "\n";
            return CommandResult::Handled;
        }
        if (args == "today" || args == "all")
        {
            out << plugins::traces::FormatUsageSummary(plugins::traces::ScanTraceUsage(context.trace_dir, args), args)
                << "\n";
            return CommandResult::Handled;
        }
        if (args.empty())
        {
            if (context.stats)
            {
                out << context.stats->CurrentSessionLine() << "\n";
            }
            else
            {
                out << "stats unavailable\n";
            }
            return CommandResult::Handled;
        }

        err << "usage: " << Usage() << "\n";
        return CommandResult::Handled;
    }
};

class ClearCommand final : public CliCommand
{
  public:
    std::string Name() const override
    {
        return "clear";
    }

    std::string Usage() const override
    {
        return "/clear";
    }

    CommandResult Execute(CliContext&, runtime::Agent& agent, const std::string&, std::ostream& out,
                          std::ostream&) override
    {
        agent.ClearHistory();
        out << "history cleared\n";
        return CommandResult::Handled;
    }
};

class ExitCommand final : public CliCommand
{
  public:
    std::string Name() const override
    {
        return "exit";
    }

    std::string Usage() const override
    {
        return "/exit | /quit";
    }

    CommandResult Execute(CliContext&, runtime::Agent&, const std::string&, std::ostream&, std::ostream&) override
    {
        return CommandResult::Exit;
    }
};

} // namespace

void CommandRegistry::Add(std::shared_ptr<CliCommand> command)
{
    m_commands.push_back(std::move(command));
}

CommandResult CommandRegistry::Execute(const std::string& line, CliContext& context, runtime::Agent& agent,
                                       std::ostream& out, std::ostream& err) const
{
    if (line.empty() || line[0] != '/')
    {
        return CommandResult::NotFound;
    }

    const std::pair<std::string, std::string> command = SplitCommand(line);
    const std::string name = command.first.substr(1);
    if (name == "quit")
    {
        return CommandResult::Exit;
    }
    for (const std::shared_ptr<CliCommand>& candidate : m_commands)
    {
        if (candidate->Name() == name)
        {
            return candidate->Execute(context, agent, command.second, out, err);
        }
    }
    return CommandResult::NotFound;
}

std::string CommandRegistry::HelpText() const
{
    std::string text = "commands:\n";
    for (const std::shared_ptr<CliCommand>& command : m_commands)
    {
        text += "  " + command->Usage() + "\n";
    }
    return text;
}

void RegisterDefaultCommands(CommandRegistry& registry)
{
    registry.Add(std::make_shared<HelpCommand>(&registry));
    registry.Add(std::make_shared<VersionCommand>());
    registry.Add(std::make_shared<ConfigCommand>());
    registry.Add(std::make_shared<WorkspaceCommand>());
    registry.Add(std::make_shared<ToolsCommand>());
    registry.Add(std::make_shared<TraceCommand>());
    registry.Add(std::make_shared<StatsCommand>());
    registry.Add(std::make_shared<ClearCommand>());
    registry.Add(std::make_shared<ExitCommand>());
}

} // namespace agent::hosts::cli
