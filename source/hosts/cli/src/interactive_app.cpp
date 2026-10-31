#include "agent/hosts/cli/interactive_app.h"

#include <iostream>
#include <string>
#include <vector>

#include "agent/plugins/metrics/metrics_formatter.h"

namespace agent::hosts::cli
{

namespace
{

void AppendSystemPrompt(std::vector<core::ChatMessage>& history, const CliConfig& config)
{
    if (!config.system_prompt.empty())
    {
        history.push_back({core::ChatRole::System, config.system_prompt});
    }
}

void PrintHelp(std::ostream& out)
{
    out << "commands:\n"
        << "  /clear  clear conversation history\n"
        << "  /stats  show current session statistics\n"
        << "  /exit   exit\n"
        << "  /help   show this help\n";
}

void PrintMetrics(const agent::plugins::metrics::MetricsCollector* metrics, std::ostream& err)
{
    if (metrics != nullptr)
    {
        err << agent::plugins::metrics::FormatMetricsLine(metrics->Snapshot()) << "\n";
    }
}

void PrintStatsCommand(const agent::plugins::metrics::MetricsCollector* metrics, std::ostream& out)
{
    if (metrics != nullptr)
    {
        out << agent::plugins::metrics::FormatMetricsLine(metrics->Snapshot()) << "\n";
    }
    else
    {
        out << "stats unavailable\n";
    }
}

} // namespace

int RunInteractive(agent::core::ChatModel& chat_model, const CliConfig& config, std::istream& in, std::ostream& out,
                   std::ostream& err, agent::plugins::metrics::MetricsCollector* metrics, bool auto_print_stats)
{
    std::vector<core::ChatMessage> history;
    AppendSystemPrompt(history, config);

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
            history.clear();
            AppendSystemPrompt(history, config);
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
            PrintStatsCommand(metrics, out);
            continue;
        }
        if (line.empty())
        {
            continue;
        }

        history.push_back({core::ChatRole::User, line});

        core::ChatRequest request;
        request.model = config.model;
        request.temperature = config.temperature;
        request.max_tokens = config.max_tokens;
        request.messages = history;

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
            history.pop_back();
            err << "error: " << response.error << "\n";
            if (auto_print_stats)
            {
                PrintMetrics(metrics, err);
            }
            continue;
        }

        out << response.content << "\n";
        history.push_back({core::ChatRole::Assistant, response.content});
        if (auto_print_stats)
        {
            PrintMetrics(metrics, err);
        }
    }

    return 0;
}

} // namespace agent::hosts::cli
