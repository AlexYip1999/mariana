#include "agent/hosts/cli/cli_app.h"

#include <ostream>
#include <string>

#include "agent/plugins/metrics/metrics_formatter.h"

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

void PrintMetrics(const agent::plugins::metrics::MetricsCollector* metrics, std::ostream& err)
{
    if (metrics != nullptr)
    {
        err << agent::plugins::metrics::FormatMetricsLine(metrics->Snapshot()) << "\n";
    }
}

} // namespace

int RunCli(const std::vector<std::string>& args, core::ChatModel& chat_model, const CliConfig& config,
           std::ostream& out, std::ostream& err, agent::plugins::metrics::MetricsCollector* metrics)
{
    if (args.empty())
    {
        err << "usage: agent-cli [--config <path>] <question...>\n";
        return 2;
    }

    core::ChatRequest request;
    request.model = config.model;
    request.temperature = config.temperature;
    request.max_tokens = config.max_tokens;
    if (!config.system_prompt.empty())
    {
        request.messages.push_back({core::ChatRole::System, config.system_prompt});
    }
    request.messages.push_back({core::ChatRole::User, JoinQuestion(args)});

    if (metrics != nullptr)
    {
        metrics->OnTurnStart();
        metrics->OnModelRequest(request);
    }

    const core::ChatResponse response = chat_model.Chat(request);
    if (metrics != nullptr)
    {
        metrics->OnModelResponse(response);
    }
    if (!response.ok)
    {
        err << "error: " << response.error << "\n";
        PrintMetrics(metrics, err);
        return 1;
    }

    out << response.content << "\n";
    PrintMetrics(metrics, err);
    return 0;
}

} // namespace agent::hosts::cli
