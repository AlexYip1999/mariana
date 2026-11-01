#include "agent/hosts/cli/cli_app.h"

#include <memory>
#include <ostream>
#include <string>

#include "agent/core/stats_view.h"
#include "agent/runtime/agent_session.h"
#include "agent/runtime/tool_registry.h"

namespace agent::hosts::cli
{

namespace
{

std::string JoinQuestion(const std::vector<std::string>& args)
{
    std::string question;
    for (const std::string& arg : args)
    {
        if (!question.empty())
        {
            question += " ";
        }
        question += arg;
    }
    return question;
}

runtime::AgentSessionConfig MakeSessionConfig(const CliConfig& config)
{
    runtime::AgentSessionConfig session_config;
    session_config.model = config.model;
    session_config.system_prompt = config.system_prompt;
    session_config.temperature = config.temperature;
    session_config.max_tokens = config.max_tokens;
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

} // namespace

int RunCli(const std::vector<std::string>& args, core::ChatModel& chat_model, const CliConfig& config,
           std::ostream& out, std::ostream& err, runtime::ServiceRegistry& services)
{
    if (args.empty())
    {
        err << "usage: agent-cli [--config <path>] <question...>\n";
        return 2;
    }

    const std::shared_ptr<core::StatsView> stats = services.Get<core::StatsView>();
    const std::shared_ptr<runtime::ToolRegistry> tools = services.Get<runtime::ToolRegistry>();
    const std::shared_ptr<core::SystemRegistry> systems = services.Get<core::SystemRegistry>();

    runtime::AgentSessionDependencies dependencies;
    dependencies.model = &chat_model;
    dependencies.tools = tools.get();
    dependencies.systems = systems.get();
    runtime::AgentSession session(MakeSessionConfig(config), dependencies);

    const runtime::AgentRunResult result = session.RunTurn(JoinQuestion(args));
    if (!result.ok)
    {
        err << "error: " << result.error << "\n";
        PrintStats(stats.get(), err);
        return 1;
    }

    out << result.content << "\n";
    PrintStats(stats.get(), err);
    return 0;
}

} // namespace agent::hosts::cli
