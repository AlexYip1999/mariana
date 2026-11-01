#include "agent/plugins/metrics/metrics_collector.h"
#include "agent/plugins/metrics/metrics_formatter.h"

#include <utility>

namespace agent::plugins::metrics
{

namespace
{

bool HasUsage(const core::ChatUsage& usage)
{
    return usage.prompt_tokens != 0 || usage.completion_tokens != 0 || usage.total_tokens != 0;
}

} // namespace

MetricsCollector::MetricsCollector(std::string provider, std::int64_t context_window_tokens,
                                   CostEstimator cost_estimator, SteadyNow steady_now, SystemNow system_now)
    : m_provider(std::move(provider))
    , m_cost_estimator(std::move(cost_estimator))
    , m_context_window_tokens(context_window_tokens)
    , m_steady_now(std::move(steady_now))
    , m_system_now(std::move(system_now))
{
    if (!m_steady_now)
    {
        m_steady_now = []
        {
            return std::chrono::steady_clock::now();
        };
    }
    if (!m_system_now)
    {
        m_system_now = []
        {
            return std::chrono::system_clock::now();
        };
    }
}

void MetricsCollector::OnTurnStart()
{
    ++m_snapshot.turns;
}

void MetricsCollector::OnModelRequest(const core::ChatRequest& request)
{
    ++m_snapshot.steps;
    m_request_start = m_steady_now();
    m_request_model = request.model;
    m_request_active = true;
}

void MetricsCollector::OnModelResponse(const core::ChatResponse& response)
{
    double duration_seconds = 0.0;
    if (m_request_active)
    {
        const std::chrono::duration<double> elapsed = m_steady_now() - m_request_start;
        duration_seconds = elapsed.count();
        m_request_active = false;
    }

    ApplyUsage(response, duration_seconds);
    ApplyCost(response.usage);
}

void MetricsCollector::OnToolCall(const core::ToolCall& call)
{
    (void)call;
    ++m_snapshot.steps;
}

void MetricsCollector::OnToolResult(const core::ToolCall& call, const core::ToolResult& result)
{
    (void)call;
    (void)result;
}

void MetricsCollector::ApplyUsage(const core::ChatResponse& response, double duration_seconds)
{
    const core::ChatUsage& usage = response.usage;
    if (!HasUsage(usage))
    {
        // 失败/无 usage 的响应不要覆盖上一次的上下文占用。
        m_snapshot.last_tokens_per_second = 0.0;
        return;
    }

    m_snapshot.has_usage = true;
    m_snapshot.prompt_tokens += usage.prompt_tokens;
    m_snapshot.completion_tokens += usage.completion_tokens;
    m_snapshot.total_tokens +=
        usage.total_tokens > 0 ? usage.total_tokens : usage.prompt_tokens + usage.completion_tokens;
    m_snapshot.cache_hit_tokens += usage.prompt_cache_hit_tokens;
    m_snapshot.cache_miss_tokens += usage.prompt_cache_miss_tokens;

    if (duration_seconds > 0.0 && usage.completion_tokens > 0)
    {
        m_snapshot.last_tokens_per_second = static_cast<double>(usage.completion_tokens) / duration_seconds;
    }
    else
    {
        m_snapshot.last_tokens_per_second = 0.0;
    }

    if (m_context_window_tokens > 0 && usage.prompt_tokens > 0)
    {
        m_snapshot.context_usage_percent =
            static_cast<double>(usage.prompt_tokens) * 100.0 / static_cast<double>(m_context_window_tokens);
    }
    else
    {
        m_snapshot.context_usage_percent = 0.0;
    }
}

void MetricsCollector::ApplyCost(const core::ChatUsage& usage)
{
    if (!m_snapshot.has_usage || !HasUsage(usage))
    {
        m_snapshot.has_cost = false;
        m_snapshot.last_cost = 0.0;
        return;
    }

    const CostEstimate estimate = m_cost_estimator.Estimate(m_provider, m_request_model, usage, m_system_now());
    if (!estimate.ok)
    {
        m_snapshot.has_cost = false;
        m_snapshot.last_cost = 0.0;
        return;
    }

    m_snapshot.has_cost = true;
    m_snapshot.currency = estimate.currency;
    m_snapshot.last_cost = estimate.cost;
    m_snapshot.total_cost += estimate.cost;
}

const MetricsSnapshot& MetricsCollector::Snapshot() const
{
    return m_snapshot;
}

std::string MetricsCollector::CurrentSessionLine() const
{
    return FormatMetricsLine(m_snapshot);
}

} // namespace agent::plugins::metrics
