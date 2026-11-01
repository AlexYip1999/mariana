#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// clang-format off
#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif
// clang-format on

#include "agent/core/chat_model.h"
#include "agent/core/stats_view.h"
#include "agent/core/system_registry.h"
#include "agent/core/version.h"
#include "agent/hosts/cli/cli_app.h"
#include "agent/hosts/cli/cli_config.h"
#include "agent/hosts/cli/cli_options.h"
#include "agent/hosts/cli/interactive_app.h"
#include "agent/hosts/cli/platform_transport.h"
#include "agent/model/openai_chat_model.h"
#include "agent/plugins/metrics/metrics_collector.h"
#include "agent/plugins/tools/read_file_tool.h"
#include "agent/runtime/agent_runner.h"
#include "agent/runtime/service_registry.h"
#include "agent/runtime/tool_registry.h"
#include "agent/transport/http_transport.h"

namespace
{

std::vector<std::string> ReadUtf8Args(int argc, char** argv)
{
#if defined(_WIN32)
    int wide_count = 0;
    LPWSTR* wide_args = CommandLineToArgvW(GetCommandLineW(), &wide_count);
    if (wide_args != nullptr)
    {
        std::vector<std::string> args;
        args.reserve(static_cast<std::size_t>(wide_count));
        for (int i = 0; i < wide_count; ++i)
        {
            const int size = WideCharToMultiByte(CP_UTF8, 0, wide_args[i], -1, nullptr, 0, nullptr, nullptr);
            if (size <= 0)
            {
                continue;
            }

            std::string value(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide_args[i], -1, value.data(), size, nullptr, nullptr);
            value.resize(static_cast<std::size_t>(size - 1));
            args.push_back(std::move(value));
        }
        LocalFree(wide_args);
        return args;
    }
#endif

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i)
    {
        args.emplace_back(argv[i]);
    }
    return args;
}

void PrintUsage()
{
    std::cout << "agent-cli " << agent::core::Version() << "\n"
              << "\n"
              << "用法:\n"
              << "  agent-cli --version\n"
              << "  agent-cli --help\n"
              << "  agent-cli --init-config [--interactive] [--config <path>]\n"
              << "  agent-cli --interactive [--stats] [--config <path>] [--workspace <path>] [--no-tools]\n"
              << "  agent-cli [--stats] [--config <path>] [--workspace <path>] [--no-tools] <question...>\n"
              << "\n"
              << "--workspace 工具可访问的根目录（默认当前工作目录）。\n"
              << "--no-tools 不注册任何工具。\n"
              << "--trust-workspace-config 允许当前目录配置覆盖 endpoint/api_key_env/model/system_prompt。\n"
              << "--init-config 写入默认配置；与 --interactive 组合时先写配置再进入交互。\n"
              << "--interactive 进入终端聊天模式（输入 /help 查看命令，/stats 查看统计）。\n"
              << "--stats 在交互模式下每轮自动打印 token/速度/缓存/上下文/预计费用；单轮模式打印一次。\n"
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
#if defined(_WIN32)
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif

    try
    {
        const std::vector<std::string> all_args = ReadUtf8Args(argc, argv);
        const std::string executable_path = all_args.empty() ? std::string() : all_args.front();

        std::vector<std::string> raw_args;
        if (all_args.size() > 1)
        {
            raw_args.assign(all_args.begin() + 1, all_args.end());
        }

        const agent::hosts::cli::CliParseResult parsed = agent::hosts::cli::ParseCliOptions(raw_args);
        if (!parsed.ok)
        {
            std::cerr << parsed.error << "\n";
            return 2;
        }

        const agent::hosts::cli::CliOptions& options = parsed.options;

        if (options.mode == agent::hosts::cli::CliMode::Help)
        {
            PrintUsage();
            return 0;
        }
        if (options.mode == agent::hosts::cli::CliMode::Version)
        {
            std::cout << agent::core::Version() << "\n";
            return 0;
        }

        std::string config_path = options.config_path;
        if (options.init_config)
        {
            const std::string target = config_path.empty() ? "config.local.json" : config_path;
            std::error_code exists_error;
            const bool exists = std::filesystem::exists(target, exists_error);
            if (exists_error)
            {
                std::cerr << "init config failed: cannot inspect " << target << ": " << exists_error.message() << "\n";
                return 2;
            }

            if (exists)
            {
                std::cout << "config already exists: " << target << "\n";
            }
            else
            {
                const agent::hosts::cli::ConfigInitResult result = agent::hosts::cli::WriteDefaultCliConfigFile(target);
                if (!result.ok)
                {
                    std::cerr << "init config failed: " << result.error << "\n";
                    return 2;
                }
                std::cout << "created " << result.path << "\n";
            }

            if (options.mode == agent::hosts::cli::CliMode::InitConfig)
            {
                return 0;
            }
        }

        agent::hosts::cli::CliConfig config;
        if (!config_path.empty())
        {
            const agent::hosts::cli::ConfigLoadResult loaded = agent::hosts::cli::LoadCliConfigFile(config_path);
            if (!loaded.ok)
            {
                std::cerr << "config error: " << loaded.error << "\n";
                return 2;
            }
            config = loaded.config;
        }
        else
        {
            const agent::hosts::cli::DefaultConfigResult loaded =
                agent::hosts::cli::LoadDefaultCliConfig(executable_path);
            if (!loaded.ok)
            {
                std::cerr << "config error (" << loaded.path << "): " << loaded.error << "\n";
                return 2;
            }

            const agent::hosts::cli::CliConfig defaults;
            const bool workspace_shared =
                !loaded.shared_path.empty() && !std::filesystem::path(loaded.shared_path).is_absolute();
            const bool workspace_local =
                !loaded.local_path.empty() && !std::filesystem::path(loaded.local_path).is_absolute();
            const bool overrides_trust_fields =
                loaded.config.endpoint != defaults.endpoint || loaded.config.api_key_env != defaults.api_key_env ||
                loaded.config.model != defaults.model || loaded.config.system_prompt != defaults.system_prompt;
            if ((workspace_shared || workspace_local) && overrides_trust_fields && !options.trust_workspace_config)
            {
                std::cerr << "refusing workspace config that changes endpoint/api_key_env/model/system_prompt; "
                             "use --config <path> or --trust-workspace-config\n";
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

        agent::model::OpenAiChatModel model(*transport, model_config);

        const bool interactive = options.mode == agent::hosts::cli::CliMode::Interactive;
        const bool observability_enabled = interactive || options.stats;

        agent::runtime::ServiceRegistry services;

        std::error_code workspace_error;
        std::filesystem::path workspace;
        if (options.workspace_root.empty())
        {
            workspace = std::filesystem::current_path(workspace_error);
        }
        else
        {
            workspace = std::filesystem::weakly_canonical(options.workspace_root, workspace_error);
        }
        if (workspace_error || !std::filesystem::is_directory(workspace, workspace_error))
        {
            std::cerr << "invalid workspace: " << options.workspace_root << "\n";
            return 2;
        }

        std::shared_ptr<agent::runtime::ToolRegistry> tools;
        if (!options.no_tools)
        {
            tools = std::make_shared<agent::runtime::ToolRegistry>();
            std::string tool_error;
            if (!tools->Register(std::make_shared<agent::plugins::tools::ReadFileTool>(workspace), tool_error))
            {
                std::cerr << "tool registration failed: " << tool_error << "\n";
                return 2;
            }
            services.Set<agent::runtime::ToolRegistry>(tools);
        }

        std::shared_ptr<agent::core::SystemRegistry> systems;
        if (observability_enabled)
        {
            systems = std::make_shared<agent::core::SystemRegistry>();
            {
                const agent::plugins::metrics::CostEstimator cost_estimator =
                    config.cost_currency == "USD" ? agent::plugins::metrics::CostEstimator::DeepSeekUsdDefaults()
                                                  : agent::plugins::metrics::CostEstimator::DeepSeekCnyDefaults();
                const auto metrics = std::make_shared<agent::plugins::metrics::MetricsCollector>(
                    config.provider, 1000000, cost_estimator);

                systems->Add(metrics);
                services.Set<agent::core::StatsView>(metrics);
            }
            services.Set<agent::core::SystemRegistry>(systems);
        }

        agent::runtime::AgentRunner executor(model, tools.get(), systems.get());

        if (interactive)
        {
            return agent::hosts::cli::RunInteractive(executor, config, std::cin, std::cout, std::cerr, services,
                                                     options.stats);
        }

        return agent::hosts::cli::RunCli(options.question, executor, config, std::cout, std::cerr, services);
    }
    catch (const std::exception& error)
    {
        std::cerr << "fatal: " << error.what() << "\n";
        return 2;
    }
    catch (...)
    {
        std::cerr << "fatal: unknown error\n";
        return 2;
    }
}
