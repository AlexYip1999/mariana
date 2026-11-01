#include "agent/hosts/cli/interactive_app.h"

#include <iostream>
#include <memory>
#include <string>

#include "agent/core/stats_view.h"
#include "agent/runtime/agent_session.h"

namespace agent::hosts::cli
{

namespace
{

void PrintHelp(std::ostream& out)
{
    out << "commands:\n"
        << "  /clear  clear conversation history\n"
        << "  /stats  show current session statistics\n"
        << "  /exit   exit\n"
        << "  /quit   exit\n"
        << "  /help   show this help\n";
}

runtime::AgentSessionConfig MakeSessionConfig(const CliConfig& config)
{
    runtime::AgentSessionConfig session_config;
    session_config.model = config.model;
    session_config.system_prompt = config.system_prompt;
    session_config.temperature = config.temperature;
    session_config.max_tokens = config.max_tokens;
    session_config.max_context_chars = config.max_context_chars;
    session_config.tool_names = config.tool_names;
    return session_config;
}

void PrintStats(const core::StatsView* stats, std::ostream& err)
{
    if (stats != nullptr)
    {
        err << stats->CurrentSessionLine() << "\n";
    }
}

void PrintStatsCommand(const core::StatsView* stats, std::ostream& out)
{
    if (stats != nullptr)
    {
        out << stats->CurrentSessionLine() << "\n";
    }
    else
    {
        out << "stats unavailable\n";
    }
}

} // namespace

int RunInteractive(runtime::AgentExecutor& executor, const CliConfig& config, std::istream& in, std::ostream& out,
                   std::ostream& err, runtime::ServiceRegistry& services, bool auto_print_stats)
{
    const std::shared_ptr<core::StatsView> stats = services.Get<core::StatsView>();

    runtime::AgentSession session(MakeSessionConfig(config), executor);

    std::string line;
    while (true)
    {
        out << "> " << std::flush;
        if (!std::getline(in, line))
        {
            out << "\n";
            break;
        }

        if (line == "/exit" || line == "/quit")
        {
            break;
        }
        if (line == "/clear")
        {
            session.ClearHistory();
            out << "history cleared\n";
            continue;
        }
        if (line == "/help")
        {
            PrintHelp(out);
            continue;
        }
        if (line == "/stats")
        {
            PrintStatsCommand(stats.get(), out);
            continue;
        }
        if (line.empty())
        {
            continue;
        }

        const runtime::AgentRunResult result = session.Send(line);
        if (!result.ok)
        {
            err << "error: " << result.error << "\n";
            if (auto_print_stats)
            {
                PrintStats(stats.get(), err);
            }
            continue;
        }

        out << result.content << "\n";
        if (auto_print_stats)
        {
            PrintStats(stats.get(), err);
        }
    }

    return 0;
}

} // namespace agent::hosts::cli
