#include "agent/hosts/cli/host_runner.h"

#include <filesystem>
#include <istream>
#include <memory>
#include <ostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "agent/core/stats_view.h"
#include "agent/core/system_registry.h"
#include "agent/core/version.h"
#include "agent/hosts/cli/cli_command.h"
#include "agent/hosts/cli/cli_config.h"
#include "agent/hosts/cli/cli_options.h"
#include "agent/hosts/cli/interactive_app.h"
#include "agent/model/openai_chat_model.h"
#include "agent/plugins/metrics/metrics_collector.h"
#include "agent/plugins/tools/read_file_tool.h"
#include "agent/plugins/traces/file_tracer.h"
#include "agent/runtime/agent.h"
#include "agent/runtime/agent_runner.h"
#include "agent/runtime/service_registry.h"
#include "agent/runtime/tool_registry.h"

namespace agent::hosts::cli
{

namespace
{

runtime::AgentProfile MakeAgentProfile(const CliConfig& config)
{
    runtime::AgentProfile profile;
    profile.model = config.model;
    profile.system_prompt = config.system_prompt;
    profile.temperature = config.temperature;
    profile.max_tokens = config.max_tokens;
    profile.max_steps = config.max_steps;
    profile.max_context_chars = config.max_context_chars;
    profile.tool_names = config.tool_names;
    return profile;
}

void PrintUsage(std::ostream& out, const CommandRegistry& registry)
{
    out << "agent-cli " << core::Version() << "\n"
        << "\n"
        << "启动参数：\n"
        << "  agent-cli --help\n"
        << "  agent-cli --version\n"
        << "  agent-cli [--config <path>]\n"
        << "\n"
        << "其他选项在交互模式用命令设置。trace 插件默认开启。\n"
        << "API key 从环境变量读取（默认 DEEPSEEK_API_KEY）。\n"
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

    std::error_code workspace_error;
    const std::filesystem::path workspace =
        environment.current_path ? environment.current_path() : std::filesystem::current_path(workspace_error);
    if (workspace_error || !std::filesystem::is_directory(workspace, workspace_error))
    {
        err << "invalid current workspace\n";
        return 2;
    }

    auto tools = std::make_shared<runtime::ToolRegistry>();
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

    auto metrics = std::make_shared<plugins::metrics::MetricsCollector>(
        config.provider, 1000000,
        config.cost_currency == "USD" ? plugins::metrics::CostEstimator::DeepSeekUsdDefaults()
                                      : plugins::metrics::CostEstimator::DeepSeekCnyDefaults());

    auto tracer = std::make_shared<plugins::traces::FileTracer>("traces");
    if (!tracer->IsOpen())
    {
        err << "cannot open trace directory: traces\n";
        return 2;
    }

    auto systems = std::make_shared<core::SystemRegistry>();
    systems->Add(metrics);
    systems->Add(tracer);

    runtime::ServiceRegistry host_services;
    host_services.Set<core::SystemRegistry>(systems);
    host_services.Set<core::StatsView>(metrics);
    host_services.Set<CommandRegistry>(command_registry);

    auto context = std::make_shared<CliContext>();
    context->config = config;
    context->workspace = workspace;
    context->tools = tools;
    context->systems = systems;
    context->stats = metrics;
    context->trace_dir = "traces";
    context->current_trace_path = tracer->Path().string();
    host_services.Set<CliContext>(context);

    auto agent_registry = std::make_shared<runtime::ServiceRegistry>(&host_services);
    agent_registry->Set<runtime::ToolRegistry>(tools);
    agent_registry->Set<runtime::ToolPolicy>(std::make_shared<runtime::AllowAllToolPolicy>());
    agent_registry->Set<runtime::ContextPolicy>(std::make_shared<runtime::KeepRecentTurnsPolicy>());
    agent_registry->Set<runtime::ToolOutputPolicy>(std::make_shared<runtime::TruncateToolOutputPolicy>());

    auto executor = std::make_shared<runtime::AgentRunner>(model, *agent_registry);

    runtime::AgentDescriptor descriptor;
    descriptor.id.value = "cli";
    descriptor.name = "agent-cli";
    descriptor.version = core::Version();
    descriptor.profile = MakeAgentProfile(config);

    runtime::AgentServices agent_services;
    agent_services.services = agent_registry;
    agent_services.executor = executor;
    runtime::Agent agent(std::move(descriptor), agent_services);

    return RunInteractive(agent, in, out, err, host_services);
}

} // namespace agent::hosts::cli
