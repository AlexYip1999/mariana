#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "agent/core/chat_model.h"
#include "agent/core/stats_view.h"
#include "agent/core/tracer.h"
#include "agent/plugins/metrics/cost_estimator.h"
#include "agent/plugins/metrics/metrics_snapshot.h"

namespace agent::plugins::metrics
{

/// Tracer + StatsView 插件：采集轮次、步数、token、缓存、速度、上下文占用和估算费用。
class MetricsCollector : public core::Tracer, public core::StatsView
{
  public:
    using SteadyNow = std::function<std::chrono::steady_clock::time_point()>;
    using SystemNow = std::function<std::chrono::system_clock::time_point()>;

    explicit MetricsCollector(std::string provider = "deepseek", std::int64_t context_window_tokens = 1000000,
                              CostEstimator cost_estimator = CostEstimator::DeepSeekCnyDefaults(),
                              SteadyNow steady_now = {}, SystemNow system_now = {});

    void OnTurnStart() override;
    void OnModelRequest(const core::ChatRequest& request) override;
    void OnModelResponse(const core::ChatResponse& response) override;
    void OnToolCall(const core::ToolCall& call) override;
    void OnToolResult(const core::ToolCall& call, const core::ToolResult& result) override;

    MetricsSnapshot Snapshot() const;
    std::string CurrentSessionLine() const override;

  private:
    mutable std::mutex m_mutex;
    void ApplyUsage(const core::ChatResponse& response, double duration_seconds);
    void ApplyCost(const core::ChatUsage& usage);

    // 配置与时钟依赖
    std::string m_provider;
    CostEstimator m_cost_estimator;
    std::int64_t m_context_window_tokens = 1000000;
    SteadyNow m_steady_now;
    SystemNow m_system_now;

    // 累计指标快照
    MetricsSnapshot m_snapshot;

    // 进行中的模型调用
    std::chrono::steady_clock::time_point m_request_start{};
    std::string m_request_model;
    bool m_request_active = false;
};

} // namespace agent::plugins::metrics
