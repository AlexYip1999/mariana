#include "agent/plugins/metrics/metrics_formatter.h"

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace agent::plugins::metrics
{

namespace
{

std::string FormatTokenCount(std::int64_t tokens)
{
    std::ostringstream out;
    if (tokens >= 1000000)
    {
        out << std::fixed << std::setprecision(1) << static_cast<double>(tokens) / 1000000.0 << "M";
    }
    else if (tokens >= 1000)
    {
        out << std::fixed << std::setprecision(1) << static_cast<double>(tokens) / 1000.0 << "K";
    }
    else
    {
        out << tokens;
    }
    return out.str();
}

double CacheHitPercent(const MetricsSnapshot& snapshot)
{
    const std::int64_t cache_total = snapshot.cache_hit_tokens + snapshot.cache_miss_tokens;
    if (cache_total <= 0)
    {
        return 0.0;
    }
    return static_cast<double>(snapshot.cache_hit_tokens) * 100.0 / static_cast<double>(cache_total);
}

std::string CurrencyPrefix(const std::string& currency)
{
    if (currency == "USD")
    {
        return "$";
    }
    if (currency == "CNY")
    {
        return "¥";
    }
    return currency + " ";
}

} // namespace

std::string FormatMetricsLine(const MetricsSnapshot& snapshot)
{
    std::ostringstream out;
    out << "[stats] " << snapshot.turns << " 轮 " << snapshot.steps << " 步 · "
        << static_cast<int>(snapshot.last_tokens_per_second) << " tok/s | " << FormatTokenCount(snapshot.total_tokens)
        << " tok · 缓存 " << std::fixed << std::setprecision(1) << CacheHitPercent(snapshot) << "% | 上下文 "
        << std::fixed << std::setprecision(1) << snapshot.context_usage_percent << "% | ";

    if (snapshot.has_cost)
    {
        out << "≈ " << CurrencyPrefix(snapshot.currency) << std::fixed << std::setprecision(6) << snapshot.total_cost;
    }
    else
    {
        out << "≈ n/a";
    }
    return out.str();
}

} // namespace agent::plugins::metrics
