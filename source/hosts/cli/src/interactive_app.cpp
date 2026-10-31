#include "agent/hosts/cli/interactive_app.h"

#include <iostream>
#include <string>
#include <vector>

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
        << "  /exit   exit\n"
        << "  /help   show this help\n";
}

} // namespace

int RunInteractive(agent::core::ModelAdapter& model_adapter, const CliConfig& config, std::istream& in,
                   std::ostream& out, std::ostream& err)
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

        const core::ChatResponse response = model_adapter.Chat(request);
        if (!response.ok)
        {
            history.pop_back();
            err << "error: " << response.error << "\n";
            continue;
        }

        out << response.content << "\n";
        history.push_back({core::ChatRole::Assistant, response.content});
    }

    return 0;
}

} // namespace agent::hosts::cli
