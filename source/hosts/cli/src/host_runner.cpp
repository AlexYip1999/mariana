#include "agent/hosts/cli/host_runner.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <istream>
#include <ostream>
#include <string>
#include <system_error>
#include <utility>

#include "agent/core/stats_view.h"
#include "agent/core/system_registry.h"
#include "agent/core/version.h"
#include "agent/hosts/cli/cli_app.h"
#include "agent/hosts/cli/cli_config.h"
#include "agent/hosts/cli/cli_options.h"
#include "agent/hosts/cli/interactive_app.h"
#include "agent/model/openai_chat_model.h"
#include "agent/plugins/metrics/metrics_collector.h"
#include "agent/plugins/tools/read_file_tool.h"
#include "agent/runtime/agent_runner.h"
#include "agent/runtime/service_registry.h"
#include "agent/runtime/tool_registry.h"

namespace agent::hosts::cli
{

namespace
{

void PrintUsage(std::ostream& out)
{
    out << "agent-cli " << core::Version() << "\n"
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

int RunInitConfig(const CliOptions& options, const std::string& config_path, std::ostream& out, std::ostream& err)
{
    const std::string target = config_path.empty() ? "config.local.json" : config_path;
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(target, exists_error);
    if (exists_error)
    {
        err << "init config failed: cannot inspect " << target << ": " << exists_error.message() << "\n";
        return 2;
    }

    if (exists)
    {
        out << "config already exists: " << target << "\n";
    }
    else
    {
        const ConfigInitResult result = WriteDefaultCliConfigFile(target);
        if (!result.ok)
        {
            err << "init config failed: " << result.error << "\n";
            return 2;
        }
        out << "created " << result.path << "\n";
    }

    if (options.mode == CliMode::InitConfig)
    {
        return 0;
    }
    return -1;
}

CliConfig LoadConfig(const CliOptions& options, const std::string& executable_path, std::ostream& err, int& exit_code)
{
    CliConfig config;
    if (!options.config_path.empty())
    {
        const ConfigLoadResult loaded = LoadCliConfigFile(options.config_path);
        if (!loaded.ok)
        {
            err << "config error: " << loaded.error << "\n";
            exit_code = 2;
            return config;
        }
        return loaded.config;
    }

    const DefaultConfigResult loaded = LoadDefaultCliConfig(executable_path);
    if (!loaded.ok)
    {
        err << "config error (" << loaded.path << "): " << loaded.error << "\n";
        exit_code = 2;
        return config;
    }

    const CliConfig defaults;
    const bool workspace_shared =
        !loaded.shared_path.empty() && !std::filesystem::path(loaded.shared_path).is_absolute();
    const bool workspace_local = !loaded.local_path.empty() && !std::filesystem::path(loaded.local_path).is_absolute();
    const bool overrides_trust_fields =
        loaded.config.endpoint != defaults.endpoint || loaded.config.api_key_env != defaults.api_key_env ||
        loaded.config.model != defaults.model || loaded.config.system_prompt != defaults.system_prompt;
    if ((workspace_shared || workspace_local) && overrides_trust_fields && !options.trust_workspace_config)
    {
        err << "refusing workspace config that changes endpoint/api_key_env/model/system_prompt; "
               "use --config <path> or --trust-workspace-config\n";
        exit_code = 2;
        return config;
    }

    return loaded.config;
}

std::filesystem::path ResolveWorkspace(const CliOptions& options, const HostEnvironment& environment, std::ostream& err,
                                       std::error_code& error)
{
    std::error_code local_error;
    std::filesystem::path workspace;
    if (options.workspace_root.empty())
    {
        workspace = environment.current_path ? environment.current_path() : std::filesystem::current_path(local_error);
    }
    else
    {
        workspace = std::filesystem::weakly_canonical(options.workspace_root, local_error);
    }

    if (local_error || !std::filesystem::is_directory(workspace, local_error))
    {
        err << "invalid workspace: " << options.workspace_root << "\n";
        error = local_error;
        return {};
    }
    return workspace;
}

} // namespace

int RunHost(const std::vector<std::string>& args, const HostEnvironment& environment, std::istream& in,
            std::ostream& out, std::ostream& err)
{
    const CliParseResult parsed = ParseCliOptions(args);
    if (!parsed.ok)
    {
        err << parsed.error << "\n";
        return 2;
    }

    const CliOptions& options = parsed.options;
    if (options.mode == CliMode::Help)
    {
        PrintUsage(out);
        return 0;
    }
    if (options.mode == CliMode::Version)
    {
        out << core::Version() << "\n";
        return 0;
    }

    const std::string config_path = options.config_path;
    if (options.init_config)
    {
        const int init_exit = RunInitConfig(options, config_path, out, err);
        if (init_exit >= 0)
        {
            return init_exit;
        }
    }

    int config_exit = 0;
    const CliConfig config = LoadConfig(options, environment.executable_path, err, config_exit);
    if (config_exit != 0)
    {
        return config_exit;
    }

    const std::string api_key = ResolveApiKey(config, environment.get_env);
    if (api_key.empty())
    {
        err << "missing API key: set environment variable " << config.api_key_env << "\n";
        return 2;
    }

    std::unique_ptr<agent::transport::HttpTransport> transport =
        environment.create_transport ? environment.create_transport() : nullptr;
    if (!transport)
    {
        err << "platform HTTP transport is not integrated yet (libcurl pending)\n";
        return 2;
    }

    model::OpenAiConfig model_config;
    model_config.endpoint = config.endpoint;
    model_config.model = config.model;
    model_config.api_key = api_key;
    model_config.timeout_ms = config.timeout_ms;

    model::OpenAiChatModel model(*transport, model_config);

    const bool interactive = options.mode == CliMode::Interactive;
    const bool observability_enabled = interactive || options.stats;

    runtime::ServiceRegistry services;

    std::error_code workspace_error;
    const std::filesystem::path workspace = ResolveWorkspace(options, environment, err, workspace_error);
    if (workspace_error || workspace.empty())
    {
        return 2;
    }

    std::shared_ptr<runtime::ToolRegistry> tools;
    if (!options.no_tools)
    {
        tools = std::make_shared<runtime::ToolRegistry>();
        std::string tool_error;
        if (!tools->Register(std::make_shared<plugins::tools::ReadFileTool>(workspace), tool_error))
        {
            err << "tool registration failed: " << tool_error << "\n";
            return 2;
        }

        for (const std::string& tool_name : config.tool_names)
        {
            if (!tools->Has(tool_name))
            {
                err << "config error: unknown tool '" << tool_name << "' in tool_names; registered: read_file\n";
                return 2;
            }
        }
        services.Set<runtime::ToolRegistry>(tools);
    }

    std::shared_ptr<core::SystemRegistry> systems;
    if (observability_enabled)
    {
        systems = std::make_shared<core::SystemRegistry>();
        const plugins::metrics::CostEstimator cost_estimator =
            config.cost_currency == "USD" ? plugins::metrics::CostEstimator::DeepSeekUsdDefaults()
                                          : plugins::metrics::CostEstimator::DeepSeekCnyDefaults();
        const auto metrics =
            std::make_shared<plugins::metrics::MetricsCollector>(config.provider, 1000000, cost_estimator);

        systems->Add(metrics);
        services.Set<core::StatsView>(metrics);
        services.Set<core::SystemRegistry>(systems);
    }

    runtime::AgentRunner executor(model, tools.get(), systems.get());
    if (interactive)
    {
        return RunInteractive(executor, config, in, out, err, services, options.stats);
    }

    return RunCli(options.question, executor, config, out, err, services);
}

} // namespace agent::hosts::cli
