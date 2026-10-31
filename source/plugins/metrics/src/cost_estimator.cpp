#include "agent/plugins/metrics/cost_estimator.h"

#include <cstdint>
#include <ctime>

namespace agent::plugins::metrics
{

namespace
{

constexpr double TOKENS_PER_MILLION = 1000000.0;
constexpr double PEAK_MULTIPLIER = 2.0;
constexpr std::int64_t SECONDS_PER_HOUR = 3600;
constexpr std::int64_t BEIJING_UTC_OFFSET_HOURS = 8;

std::int64_t ResolveCacheMissTokens(const agent::core::ChatUsage& usage)
{
    if (usage.prompt_cache_hit_tokens + usage.prompt_cache_miss_tokens == 0 && usage.prompt_tokens > 0)
    {
        // 没有缓存字段时按全部 input 未命中处理，避免低估费用。
        return usage.prompt_tokens;
    }
    if (usage.prompt_cache_miss_tokens == 0 && usage.prompt_tokens > usage.prompt_cache_hit_tokens)
    {
        return usage.prompt_tokens - usage.prompt_cache_hit_tokens;
    }
    return usage.prompt_cache_miss_tokens;
}

bool ToUtcTime(std::chrono::system_clock::time_point time, std::tm& utc_time)
{
    const std::time_t raw_time = std::chrono::system_clock::to_time_t(time);
#if defined(_WIN32)
    return gmtime_s(&utc_time, &raw_time) == 0;
#else
    return gmtime_r(&raw_time, &utc_time) != nullptr;
#endif
}

bool ToBeijingTime(std::chrono::system_clock::time_point time, std::tm& beijing_time)
{
    const std::time_t raw_time = std::chrono::system_clock::to_time_t(time);
    const std::time_t shifted_time = raw_time + static_cast<std::time_t>(BEIJING_UTC_OFFSET_HOURS * SECONDS_PER_HOUR);
#if defined(_WIN32)
    return gmtime_s(&beijing_time, &shifted_time) == 0;
#else
    return gmtime_r(&shifted_time, &beijing_time) != nullptr;
#endif
}

bool IsWeekend(const std::tm& time)
{
    return time.tm_wday == 0 || time.tm_wday == 6;
}

bool IsPeakInUtc(const std::tm& utc_time)
{
    if (IsWeekend(utc_time))
    {
        return false;
    }
    const bool morning_peak = utc_time.tm_hour >= 1 && utc_time.tm_hour < 4;
    const bool afternoon_peak = utc_time.tm_hour >= 6 && utc_time.tm_hour < 10;
    return morning_peak || afternoon_peak;
}

bool IsPeakInBeijing(const std::tm& beijing_time)
{
    if (IsWeekend(beijing_time))
    {
        return false;
    }
    const bool morning_peak = beijing_time.tm_hour >= 9 && beijing_time.tm_hour < 12;
    const bool afternoon_peak = beijing_time.tm_hour >= 14 && beijing_time.tm_hour < 18;
    return morning_peak || afternoon_peak;
}

bool IsPeak(const ModelPricing& pricing, std::chrono::system_clock::time_point time)
{
    std::tm converted_time{};
    switch (pricing.peak_schedule)
    {
    case PeakSchedule::None:
        return false;
    case PeakSchedule::DeepSeekUtc:
        return ToUtcTime(time, converted_time) && IsPeakInUtc(converted_time);
    case PeakSchedule::DeepSeekBeijing:
        return ToBeijingTime(time, converted_time) && IsPeakInBeijing(converted_time);
    }
    return false;
}

} // namespace

CostEstimator CostEstimator::DeepSeekCnyDefaults()
{
    CostEstimator estimator;
    estimator.SetPricing("deepseek", "deepseek-flash",
                         ModelPricing{0.02, 1.0, 4.0, "CNY", PeakSchedule::DeepSeekBeijing});
    estimator.SetPricing("deepseek", "deepseek-v4-pro",
                         ModelPricing{0.15, 4.5, 13.5, "CNY", PeakSchedule::DeepSeekBeijing});
    return estimator;
}

CostEstimator CostEstimator::DeepSeekUsdDefaults()
{
    CostEstimator estimator;
    estimator.SetPricing("deepseek", "deepseek-flash",
                         ModelPricing{0.003, 0.15, 0.6, "USD", PeakSchedule::DeepSeekUtc});
    estimator.SetPricing("deepseek", "deepseek-v4-pro",
                         ModelPricing{0.022, 0.66, 1.98, "USD", PeakSchedule::DeepSeekUtc});
    return estimator;
}

void CostEstimator::SetPricing(const std::string& provider, const std::string& model, const ModelPricing& pricing)
{
    m_pricing[provider][model] = pricing;
}

CostEstimate CostEstimator::Estimate(const std::string& provider, const std::string& model,
                                     const agent::core::ChatUsage& usage,
                                     std::chrono::system_clock::time_point time) const
{
    CostEstimate estimate;
    const auto provider_pricing = m_pricing.find(provider);
    if (provider_pricing == m_pricing.end())
    {
        estimate.error = "no pricing for provider: " + provider;
        return estimate;
    }

    const auto model_pricing = provider_pricing->second.find(model);
    if (model_pricing == provider_pricing->second.end())
    {
        estimate.error = "no pricing for model: " + provider + "/" + model;
        return estimate;
    }

    const ModelPricing& pricing = model_pricing->second;
    const std::int64_t cache_hit_tokens = usage.prompt_cache_hit_tokens;
    const std::int64_t cache_miss_tokens = ResolveCacheMissTokens(usage);
    const bool peak = IsPeak(pricing, time);
    const double multiplier = peak ? PEAK_MULTIPLIER : 1.0;

    const double input_hit_cost = static_cast<double>(cache_hit_tokens) * pricing.cache_hit_input_per_million;
    const double input_miss_cost = static_cast<double>(cache_miss_tokens) * pricing.cache_miss_input_per_million;
    const double output_cost = static_cast<double>(usage.completion_tokens) * pricing.output_per_million;

    estimate.ok = true;
    estimate.currency = pricing.currency;
    estimate.peak = peak;
    estimate.cost = (input_hit_cost + input_miss_cost + output_cost) / TOKENS_PER_MILLION * multiplier;
    return estimate;
}

bool CostEstimator::IsDeepSeekUtcPeakTime(std::chrono::system_clock::time_point time)
{
    std::tm utc_time{};
    return ToUtcTime(time, utc_time) && IsPeakInUtc(utc_time);
}

bool CostEstimator::IsDeepSeekBeijingPeakTime(std::chrono::system_clock::time_point time)
{
    std::tm beijing_time{};
    return ToBeijingTime(time, beijing_time) && IsPeakInBeijing(beijing_time);
}

} // namespace agent::plugins::metrics
