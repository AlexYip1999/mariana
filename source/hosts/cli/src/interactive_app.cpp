#include "agent/hosts/cli/interactive_app.h"

#include <iostream>
#include <memory>
#include <string>

#include "agent/core/stats_view.h"

namespace agent::hosts::cli
{

namespace
{

std::string Trim(const std::string& text)
{
    const std::size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos)
    {
        return {};
    }
    const std::size_t last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

void PrintStats(const core::StatsView* stats, std::ostream& err)
{
    if (stats != nullptr)
    {
        err << stats->CurrentSessionLine() << "\n";
    }
}

} // namespace

int RunInteractive(runtime::Agent& agent, std::istream& in, std::ostream& out, std::ostream& err,
                   runtime::ServiceRegistry& services, bool auto_print_stats)
{
    const std::shared_ptr<core::StatsView> stats = services.Get<core::StatsView>();
    const std::shared_ptr<CommandRegistry> commands = services.Get<CommandRegistry>();
    const std::shared_ptr<CliContext> context = services.Get<CliContext>();
    if (!commands || !context)
    {
        err << "interactive commands unavailable: CommandRegistry/CliContext not registered\n";
        return 2;
    }

    const auto auto_stats_enabled = [&]()
    {
        return auto_print_stats || context->auto_stats;
    };

    std::string line;
    while (true)
    {
        out << "> " << std::flush;
        if (!std::getline(in, line))
        {
            out << "\n";
            break;
        }

        const std::string command_line = Trim(line);
        if (command_line.empty())
        {
            continue;
        }

        if (command_line.rfind("/", 0) == 0)
        {
            const CommandResult result = commands->Execute(command_line, *context, agent, out, err);
            if (result == CommandResult::Exit)
            {
                break;
            }
            if (result == CommandResult::NotFound)
            {
                out << "unknown command: " << command_line << "\n";
            }
            continue;
        }

        const runtime::AgentRunResult result = agent.Send(command_line);
        if (!result.ok)
        {
            err << "error: " << result.error << "\n";
            if (auto_stats_enabled())
            {
                PrintStats(stats.get(), err);
            }
            continue;
        }

        out << result.content << "\n";
        if (auto_stats_enabled())
        {
            PrintStats(stats.get(), err);
        }
    }

    return 0;
}

} // namespace agent::hosts::cli
