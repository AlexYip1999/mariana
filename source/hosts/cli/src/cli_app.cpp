#include "agent/hosts/cli/cli_app.h"

#include <ostream>
#include <string>

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

} // namespace

int RunCli(const std::vector<std::string>& args, core::ChatModel& chat_model, const CliConfig& config,
           std::ostream& out, std::ostream& err)
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

    const core::ChatResponse response = chat_model.Chat(request);
    if (!response.ok)
    {
        err << "error: " << response.error << "\n";
        return 1;
    }

    out << response.content << "\n";
    return 0;
}

} // namespace agent::hosts::cli
