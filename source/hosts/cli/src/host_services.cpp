#include "agent/hosts/cli/host_services.h"

#include <memory>
#include <utility>

#include "agent/hosts/cli/tool_catalog.h"
#include "agent/model/openai_chat_model.h"
#include "agent/plugins/metrics/metrics_collector.h"
#include "agent/plugins/traces/file_tracer.h"

namespace agent::hosts::cli
{

std::optional<HostServices> BuildHostServices(const HostServicesConfig& input, std::string& error)
{
    model::OpenAiConfig model_config;
    model_config.endpoint = input.config.endpoint;
    model_config.model = input.config.model;
    model_config.api_key = input.api_key;
    model_config.timeout_ms = input.config.timeout_ms;

    HostServices services;
    services.tool_catalog = input.tool_catalog;
    services.model = std::make_shared<model::OpenAiChatModel>(input.transport, model_config);

    services.metrics = std::make_shared<plugins::metrics::MetricsCollector>(
        input.config.provider, 1000000,
        input.config.cost_currency == "USD" ? plugins::metrics::CostEstimator::DeepSeekUsdDefaults()
                                            : plugins::metrics::CostEstimator::DeepSeekCnyDefaults());

    services.tracer = std::make_shared<plugins::traces::FileTracer>("traces", input.session_id);
    if (!services.tracer->IsOpen())
    {
        error = "cannot open trace directory: traces";
        return std::nullopt;
    }

    services.systems = std::make_shared<core::SystemRegistry>();
    services.systems->Add(services.metrics);
    services.systems->Add(services.tracer);

    services.host_scope = std::make_shared<runtime::ServiceRegistry>();
    services.host_scope->Set<CommandRegistry>(input.commands);

    services.context = std::make_shared<CliContext>();
    services.context->config = input.config;
    services.context->workspace = input.workspace;
    services.context->stats = services.metrics;
    services.context->trace_dir = "traces";
    services.context->current_trace_path = services.tracer->Path().string();
    services.context->tool_catalog = input.tool_catalog;

    // Stateful observers and UI state belong to this session, not the shared host scope.
    services.session_scope = std::make_shared<runtime::ServiceRegistry>(services.host_scope);
    services.session_scope->Set<core::SystemRegistry>(services.systems);
    services.session_scope->Set<core::StatsView>(services.metrics);
    services.session_scope->Set<CliContext>(services.context);
    return services;
}

} // namespace agent::hosts::cli
