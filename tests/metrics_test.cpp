#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agent/core/chat_model.h"
#include "agent/plugins/metrics/cost_estimator.h"
#include "agent/plugins/metrics/metrics_collector.h"
#include "agent/plugins/metrics/metrics_formatter.h"

namespace
{

std::chrono::system_clock::time_point UtcTime(int year, unsigned month, unsigned day, int hour)
{
    using namespace std::chrono;
    const year_month_day date{std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}};
    return sys_days{date} + hours{hour};
}

agent::core::ChatUsage MakeUsage(std::int64_t prompt_tokens, std::int64_t completion_tokens,
                                 std::int64_t cache_hit_tokens, std::int64_t cache_miss_tokens)
{
    agent::core::ChatUsage usage;
    usage.prompt_tokens = prompt_tokens;
    usage.completion_tokens = completion_tokens;
    usage.total_tokens = prompt_tokens + completion_tokens;
    usage.prompt_cache_hit_tokens = cache_hit_tokens;
    usage.prompt_cache_miss_tokens = cache_miss_tokens;
    return usage;
}

class FakeSteadyClock
{
  public:
    explicit FakeSteadyClock(std::vector<std::chrono::steady_clock::time_point> points) : m_points(std::move(points))
    {
    }

    std::chrono::steady_clock::time_point Now()
    {
        if (m_index < m_points.size())
        {
            return m_points[m_index++];
        }
        return m_points.empty() ? std::chrono::steady_clock::time_point{} : m_points.back();
    }

  private:
    std::vector<std::chrono::steady_clock::time_point> m_points;
    std::size_t m_index = 0;
};

} // namespace

TEST_CASE("estimates deepseek cny cost and cache")
{
    const agent::plugins::metrics::CostEstimator estimator =
        agent::plugins::metrics::CostEstimator::DeepSeekCnyDefaults();
    const agent::core::ChatUsage usage = MakeUsage(2000000, 1000000, 1000000, 1000000);

    // 北京时间 08:00（UTC 00:00），off-peak。
    const agent::plugins::metrics::CostEstimate off_peak =
        estimator.Estimate("deepseek", "deepseek-flash", usage, UtcTime(2026, 11, 2, 0));
    // 北京时间 10:00（UTC 02:00），peak。
    const agent::plugins::metrics::CostEstimate peak =
        estimator.Estimate("deepseek", "deepseek-flash", usage, UtcTime(2026, 11, 2, 2));

    CHECK(off_peak.ok);
    CHECK(off_peak.currency == "CNY");
    CHECK_FALSE(off_peak.peak);
    CHECK(off_peak.cost == doctest::Approx(5.02));

    CHECK(peak.ok);
    CHECK(peak.peak);
    CHECK(peak.cost == doctest::Approx(10.04));
}

TEST_CASE("estimates deepseek usd cost with utc peak")
{
    const agent::plugins::metrics::CostEstimator estimator =
        agent::plugins::metrics::CostEstimator::DeepSeekUsdDefaults();
    const agent::core::ChatUsage usage = MakeUsage(2000000, 1000000, 1000000, 1000000);

    const agent::plugins::metrics::CostEstimate off_peak =
        estimator.Estimate("deepseek", "deepseek-flash", usage, UtcTime(2026, 11, 2, 5));
    const agent::plugins::metrics::CostEstimate peak =
        estimator.Estimate("deepseek", "deepseek-flash", usage, UtcTime(2026, 11, 2, 2));

    CHECK(off_peak.currency == "USD");
    CHECK(off_peak.cost == doctest::Approx(0.753));
    CHECK(peak.peak);
    CHECK(peak.cost == doctest::Approx(1.506));
}

TEST_CASE("treats missing cache fields as cache miss")
{
    const agent::plugins::metrics::CostEstimator estimator =
        agent::plugins::metrics::CostEstimator::DeepSeekCnyDefaults();
    agent::core::ChatUsage usage;
    usage.prompt_tokens = 1000000;
    usage.total_tokens = 1000000;

    const agent::plugins::metrics::CostEstimate estimate =
        estimator.Estimate("deepseek", "deepseek-flash", usage, UtcTime(2026, 11, 2, 0));

    CHECK(estimate.ok);
    CHECK(estimate.cost == doctest::Approx(1.0));
}

TEST_CASE("separates pricing by provider")
{
    agent::plugins::metrics::CostEstimator estimator;
    estimator.SetPricing(
        "provider-a", "model-x",
        agent::plugins::metrics::ModelPricing{1.0, 2.0, 3.0, "USD", agent::plugins::metrics::PeakSchedule::None});
    estimator.SetPricing(
        "provider-b", "model-x",
        agent::plugins::metrics::ModelPricing{4.0, 5.0, 6.0, "USD", agent::plugins::metrics::PeakSchedule::None});

    const agent::core::ChatUsage usage = MakeUsage(1000000, 1000000, 1000000, 0);
    const agent::plugins::metrics::CostEstimate first =
        estimator.Estimate("provider-a", "model-x", usage, UtcTime(2026, 11, 2, 5));
    const agent::plugins::metrics::CostEstimate second =
        estimator.Estimate("provider-b", "model-x", usage, UtcTime(2026, 11, 2, 5));

    CHECK(first.ok);
    CHECK(second.ok);
    CHECK(first.cost == doctest::Approx(4.0));
    CHECK(second.cost == doctest::Approx(10.0));
}

TEST_CASE("rejects unknown provider or model")
{
    const agent::plugins::metrics::CostEstimator estimator =
        agent::plugins::metrics::CostEstimator::DeepSeekCnyDefaults();
    const agent::core::ChatUsage usage = MakeUsage(10, 10, 0, 10);

    const agent::plugins::metrics::CostEstimate unknown_provider =
        estimator.Estimate("unknown-provider", "deepseek-flash", usage, UtcTime(2026, 11, 2, 0));
    const agent::plugins::metrics::CostEstimate unknown_model =
        estimator.Estimate("deepseek", "unknown-model", usage, UtcTime(2026, 11, 2, 0));

    CHECK_FALSE(unknown_provider.ok);
    CHECK(unknown_provider.error.find("provider") != std::string::npos);
    CHECK_FALSE(unknown_model.ok);
    CHECK(unknown_model.error.find("model") != std::string::npos);
}

TEST_CASE("collects usage speed and cost")
{
    std::vector<std::chrono::steady_clock::time_point> points;
    points.push_back(std::chrono::steady_clock::time_point{});
    points.push_back(std::chrono::steady_clock::time_point{} + std::chrono::seconds{2});
    auto clock = std::make_shared<FakeSteadyClock>(std::move(points));

    agent::plugins::metrics::MetricsCollector metrics(
        "deepseek", 1000000, agent::plugins::metrics::CostEstimator::DeepSeekCnyDefaults(),
        [clock]()
        {
            return clock->Now();
        },
        []
        {
            return UtcTime(2026, 11, 2, 0);
        });

    agent::core::ChatRequest request;
    request.model = "deepseek-flash";

    agent::core::ChatResponse response;
    response.ok = true;
    response.content = "hello";
    response.usage = MakeUsage(2000, 400, 1800, 200);

    metrics.OnTurnStart();
    metrics.OnModelRequest(request);
    metrics.OnModelResponse(response);

    const agent::plugins::metrics::MetricsSnapshot& snapshot = metrics.Snapshot();
    CHECK(snapshot.turns == 1);
    CHECK(snapshot.steps == 1);
    CHECK(snapshot.total_tokens == 2400);
    CHECK(snapshot.last_tokens_per_second == doctest::Approx(200.0));
    CHECK(snapshot.context_usage_percent == doctest::Approx(0.2));
    CHECK(snapshot.has_usage);
    CHECK(snapshot.has_cost);
    CHECK(snapshot.currency == "CNY");
    CHECK(snapshot.total_cost > 0.0);

    const std::string line = agent::plugins::metrics::FormatMetricsLine(snapshot);
    CHECK(line.find("1 轮 1 步") != std::string::npos);
    CHECK(line.find("200 tok/s") != std::string::npos);
    CHECK(line.find("缓存 90.0%") != std::string::npos);
    CHECK(line.find("上下文 0.2%") != std::string::npos);
    CHECK(line.find("≈ ¥") != std::string::npos);
}
