#include "agent/hosts/cli/cli_options.h"

#include <string>
#include <vector>

namespace agent::hosts::cli
{

namespace
{

bool RequireValue(const std::vector<std::string>& args, std::size_t& index, const char* name, std::string& value,
                  std::string& error)
{
    if (index + 1 >= args.size())
    {
        error = std::string(name) + " requires a value";
        return false;
    }
    value = args[++index];
    if (value.empty())
    {
        error = std::string(name) + " requires a non-empty value";
        return false;
    }
    return true;
}

} // namespace

CliParseResult ParseCliOptions(const std::vector<std::string>& args)
{
    CliParseResult result;

    for (std::size_t index = 0; index < args.size(); ++index)
    {
        const std::string& arg = args[index];
        if (arg == "--help" || arg == "-h")
        {
            result.options.help = true;
            continue;
        }
        if (arg == "--version" || arg == "-v")
        {
            result.options.version = true;
            continue;
        }
        if (arg == "--config")
        {
            if (!RequireValue(args, index, "--config", result.options.config_path, result.error))
            {
                result.ok = false;
                return result;
            }
            continue;
        }
        if (arg == "--memory")
        {
            if (!RequireValue(args, index, "--memory", result.options.memory_path, result.error))
            {
                result.ok = false;
                return result;
            }
            continue;
        }

        result.ok = false;
        result.error = "unknown argument: " + arg +
                       "\n启动只接受 --help/--version/--config/--memory；其他选项在交互模式用命令设置";
        return result;
    }

    return result;
}

} // namespace agent::hosts::cli
