#include "agent/hosts/cli/host_runner.h"

#include <filesystem>
#include <istream>
#include <memory>
#include <ostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "agent/core/version.h"
#include "agent/hosts/cli/agent_factory.h"
#include "agent/hosts/cli/cli_command.h"
#include "agent/hosts/cli/cli_config.h"
#include "agent/hosts/cli/cli_options.h"
#include "agent/hosts/cli/host_services.h"
#include "agent/hosts/cli/run_loop.h"
#include "agent/hosts/cli/tool_catalog.h"
#include "agent/runtime/execution_id.h"

namespace agent::hosts::cli
{

namespace
{

void PrintUsage(std::ostream& out, const CommandRegistry& registry)
{
    out << "agent-cli " << core::Version() << "\n"
        << "\n"
        << "启动参数：\n"
        << "  agent-cli --help\n"
        << "  agent-cli --version\n"
        << "  agent-cli [--config <path>] [--memory <path>]\n"
        << "\n"
        << "其他选项在交互模式用命令设置。trace 插件默认开启。\n"
        << "API key 从环境变量读取（默认 DEEPSEEK_API_KEY）。\n"
        << "--memory 指定 JSONL 长期记忆文件（跨会话），交互模式用 /memory 查看或写入。\n"
        << "\n"
        << registry.HelpText();
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
    if ((workspace_shared || workspace_local) && overrides_trust_fields)
    {
        err << "refusing workspace config that changes endpoint/api_key_env/model/system_prompt; "
               "use --config <path>\n";
        exit_code = 2;
        return config;
    }

    return loaded.config;
}

std::filesystem::path ResolveWorkspace(const HostEnvironment& environment, std::ostream& err)
{
    std::error_code workspace_error;
    const std::filesystem::path workspace =
        environment.current_path ? environment.current_path() : std::filesystem::current_path(workspace_error);
    if (workspace_error || !std::filesystem::is_directory(workspace, workspace_error))
    {
        err << "invalid current workspace\n";
        return {};
    }
    return workspace;
}

bool ValidateConfiguredTools(const CliConfig& config, const ToolCatalog& catalog, std::ostream& err)
{
    for (const std::string& tool_name : config.tool_names)
    {
        if (catalog.Has(tool_name))
        {
            continue;
        }
        err << "config error: unknown tool '" << tool_name << "' in tool_names; available:";
        for (const std::string& available : catalog.Names())
        {
            err << " " << available;
        }
        err << "\n";
        return false;
    }
    return true;
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

    auto command_registry = std::make_shared<CommandRegistry>();
    RegisterDefaultCommands(*command_registry);

    const CliOptions& options = parsed.options;
    if (options.help)
    {
        PrintUsage(out, *command_registry);
        return 0;
    }
    if (options.version)
    {
        out << core::Version() << "\n";
        return 0;
    }

    int config_exit = 0;
    CliConfig config = LoadConfig(options, environment.executable_path, err, config_exit);
    if (config_exit != 0)
    {
        return config_exit;
    }
    // --memory 覆盖配置文件里的 memory_path（命令行优先）。
    if (!options.memory_path.empty())
    {
        config.memory_path = options.memory_path;
    }

    const std::string api_key = ResolveApiKey(config, environment.get_env);
    if (api_key.empty())
    {
        err << "missing API key: set environment variable " << config.api_key_env << "\n";
        return 2;
    }

    std::shared_ptr<transport::HttpTransport> transport =
        environment.create_transport ? environment.create_transport() : nullptr;
    if (!transport)
    {
        err << "platform HTTP transport is not integrated yet (libcurl pending)\n";
        return 2;
    }

    const std::filesystem::path workspace = ResolveWorkspace(environment, err);
    if (workspace.empty())
    {
        return 2;
    }

    auto tool_catalog = std::make_shared<ToolCatalog>(DefaultToolCatalog());
    if (!ValidateConfiguredTools(config, *tool_catalog, err))
    {
        return 2;
    }

    const std::string session_id = runtime::ExecutionId::NewSession();
    HostServicesConfig services_config;
    services_config.config = config;
    services_config.workspace = workspace;
    services_config.session_id = session_id;
    services_config.api_key = api_key;
    services_config.commands = command_registry;
    services_config.tool_catalog = tool_catalog;
    services_config.transport = transport;

    std::string services_error;
    std::optional<HostServices> services = BuildHostServices(services_config, services_error);
    if (!services)
    {
        err << services_error << "\n";
        return 2;
    }

    auto factory =
        std::make_shared<AgentFactory>(config, services->model, services->systems, tool_catalog, services->memory);
    services->context->create_executor = [factory](const std::filesystem::path& workspace_path)
    {
        return factory->CreateExecutor(workspace_path);
    };

    std::shared_ptr<runtime::AgentRunner> executor;
    try
    {
        executor = factory->CreateExecutor(workspace);
    }
    catch (const std::exception& exception)
    {
        err << "tool registration failed: " << exception.what() << "\n";
        return 2;
    }

    runtime::Agent agent = factory->CreateAgent(executor, services->session_scope, session_id);
    return RunCliLoop(agent, *services->session_scope, in, out, err);
}

} // namespace agent::hosts::cli
