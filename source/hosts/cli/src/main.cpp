#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "agent/core/model_adapter.h"
#include "agent/core/version.h"
#include "agent/hosts/cli/cli_app.h"
#include "agent/hosts/cli/cli_config.h"
#include "agent/hosts/cli/cli_options.h"
#include "agent/hosts/cli/interactive_app.h"
#include "agent/hosts/cli/platform_transport.h"
#include "agent/model/openai_model_adapter.h"
#include "agent/transport/http_transport.h"

namespace
{

void PrintUsage()
{
    std::cout << "agent-cli " << agent::core::Version() << "\n"
              << "\n"
              << "用法:\n"
              << "  agent-cli --version\n"
              << "  agent-cli --help\n"
              << "  agent-cli --init-config [--config <path>]\n"
              << "  agent-cli --interactive [--config <path>]\n"
              << "  agent-cli [--config <path>] <question...>\n"
              << "\n"
              << "--interactive 进入终端聊天模式（输入 /help 查看命令）。\n"
              << "\n"
              << "未指定 --config 时加载：\n"
              << "  共享配置 config.json（当前目录，其次 exe 同目录）\n"
              << "  本地覆盖 config.local.json（当前目录，其次 exe 同目录）\n"
              << "  local 字段级覆盖 shared，都没有时使用内置默认值。\n"
              << "\n"
              << "API key 从环境变量读取（默认 DEEPSEEK_API_KEY）。\n";
}

} // namespace

int main(int argc, char** argv)
{
    const std::string executable_path = (argc > 0) ? argv[0] : std::string();

    std::vector<std::string> raw_args;
    for (int i = 1; i < argc; ++i)
    {
        raw_args.emplace_back(argv[i]);
    }

    const agent::hosts::cli::CliParseResult parsed = agent::hosts::cli::ParseCliOptions(raw_args);
    if (!parsed.ok)
    {
        std::cerr << parsed.error << "\n";
        return 2;
    }

    const agent::hosts::cli::CliOptions& options = parsed.options;
    switch (options.mode)
    {
    case agent::hosts::cli::CliMode::Help:
        PrintUsage();
        return 0;
    case agent::hosts::cli::CliMode::Version:
        std::cout << agent::core::Version() << "\n";
        return 0;
    case agent::hosts::cli::CliMode::InitConfig:
    {
        const std::string target = options.config_path.empty() ? "config.local.json" : options.config_path;
        const agent::hosts::cli::ConfigInitResult result = agent::hosts::cli::WriteDefaultCliConfigFile(target);
        if (!result.ok)
        {
            std::cerr << "init config failed: " << result.error << "\n";
            return 2;
        }
        std::cout << "created " << result.path << "\n";
        return 0;
    }
    case agent::hosts::cli::CliMode::Interactive:
    case agent::hosts::cli::CliMode::Chat:
        break;
    }

    agent::hosts::cli::CliConfig config;
    if (!options.config_path.empty())
    {
        const agent::hosts::cli::ConfigLoadResult loaded = agent::hosts::cli::LoadCliConfigFile(options.config_path);
        if (!loaded.ok)
        {
            std::cerr << "config error: " << loaded.error << "\n";
            return 2;
        }
        config = loaded.config;
    }
    else
    {
        const agent::hosts::cli::DefaultConfigResult loaded = agent::hosts::cli::LoadDefaultCliConfig(executable_path);
        if (!loaded.ok)
        {
            std::cerr << "config error (" << loaded.path << "): " << loaded.error << "\n";
            return 2;
        }
        config = loaded.config;
    }

    auto fallback = [](const std::string& name)
    {
        const char* value = std::getenv(name.c_str());
        if (value == nullptr)
        {
            return std::string();
        }
        return std::string(value);
    };

    const std::string api_key = agent::hosts::cli::ResolveApiKey(config, fallback);
    if (api_key.empty())
    {
        std::cerr << "missing API key: set environment variable " << config.api_key_env << "\n";
        return 2;
    }

    std::unique_ptr<agent::transport::HttpTransport> transport = agent::hosts::cli::CreatePlatformTransport();
    if (!transport)
    {
        std::cerr << "platform HTTP transport is not integrated yet (libcurl pending)\n";
        return 2;
    }

    agent::model::OpenAiConfig model_config;
    model_config.endpoint = config.endpoint;
    model_config.model = config.model;
    model_config.api_key = api_key;
    model_config.timeout_ms = config.timeout_ms;
    agent::model::OpenAiModelAdapter adapter(*transport, model_config);

    if (options.mode == agent::hosts::cli::CliMode::Interactive)
    {
        return agent::hosts::cli::RunInteractive(adapter, config, std::cin, std::cout, std::cerr);
    }

    return agent::hosts::cli::RunCli(options.question, adapter, config, std::cout, std::cerr);
}
