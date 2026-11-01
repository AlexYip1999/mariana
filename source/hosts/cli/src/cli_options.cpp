#include "agent/hosts/cli/cli_options.h"

namespace agent::hosts::cli
{

CliParseResult ParseCliOptions(const std::vector<std::string>& args)
{
    CliParseResult result;

    bool init_config = false;
    bool interactive = false;
    bool version = false;
    bool help = false;
    bool end_of_options = false;

    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const std::string& arg = args[i];
        if (!end_of_options && arg == "--")
        {
            end_of_options = true;
            continue;
        }
        if (!end_of_options && arg == "--config")
        {
            if (i + 1 >= args.size())
            {
                result.ok = false;
                result.error = "--config requires a path";
                return result;
            }
            result.options.config_path = args[++i];
            continue;
        }
        if (!end_of_options && arg == "--workspace")
        {
            if (i + 1 >= args.size())
            {
                result.ok = false;
                result.error = "--workspace requires a path";
                return result;
            }
            result.options.workspace_root = args[++i];
            continue;
        }
        if (!end_of_options && arg == "--no-tools")
        {
            result.options.no_tools = true;
            continue;
        }
        if (!end_of_options && arg == "--trace")
        {
            result.options.trace = true;
            continue;
        }
        if (!end_of_options && arg == "--trace-dir")
        {
            if (i + 1 >= args.size())
            {
                result.ok = false;
                result.error = "--trace-dir requires a path";
                return result;
            }
            result.options.trace = true;
            result.options.trace_dir = args[++i];
            continue;
        }
        if (!end_of_options && arg == "--trust-workspace-config")
        {
            result.options.trust_workspace_config = true;
            continue;
        }
        if (!end_of_options && arg == "--init-config")
        {
            init_config = true;
            continue;
        }
        if (!end_of_options && arg == "--stats")
        {
            result.options.stats = true;
            continue;
        }
        if (!end_of_options && (arg == "--interactive" || arg == "-i"))
        {
            interactive = true;
            continue;
        }
        if (!end_of_options && (arg == "--version" || arg == "-v"))
        {
            version = true;
            continue;
        }
        if (!end_of_options && (arg == "--help" || arg == "-h"))
        {
            help = true;
            continue;
        }
        if (!end_of_options && !arg.empty() && arg[0] == '-')
        {
            result.ok = false;
            result.error = "unknown option: " + arg;
            return result;
        }
        result.options.question.push_back(arg);
    }

    if (help)
    {
        result.options.mode = CliMode::Help;
        return result;
    }
    if (version)
    {
        result.options.mode = CliMode::Version;
        return result;
    }
    if (init_config && !result.options.question.empty())
    {
        result.ok = false;
        result.error = "--init-config cannot be combined with a question";
        return result;
    }
    if (interactive && !result.options.question.empty())
    {
        result.ok = false;
        result.error = "--interactive cannot be combined with a question";
        return result;
    }

    if (init_config)
    {
        result.options.init_config = true;
    }
    if (init_config && interactive)
    {
        result.options.mode = CliMode::Interactive;
    }
    else if (init_config)
    {
        result.options.mode = CliMode::InitConfig;
    }
    else if (interactive)
    {
        result.options.mode = CliMode::Interactive;
    }
    return result;
}

} // namespace agent::hosts::cli
