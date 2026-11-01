#pragma once

#include <chrono>
#include <string>
#include <unordered_map>

#include "agent/core/chat_model.h"

namespace agent::plugins::metrics
{

enum class PeakSchedule
{
    None,
    DeepSeekUtc,
    DeepSeekBeijing,
};

/// 单位：货币 / 1M tokens，默认是 off-peak 价格。
struct ModelPricing
{
    double cache_hit_input_per_million = 0.0;
    double cache_miss_input_per_million = 0.0;
    double output_per_million = 0.0;
    std::string currency = "CNY";
    PeakSchedule peak_schedule = PeakSchedule::None;
};

struct CostEstimate
{
    bool ok = false;
    double cost = 0.0;
    std::string currency = "CNY";
    bool peak = false;
    std::string error;
};

/// 按 (provider, model) 查找价格；不同供应商可以有不同价格、币种和 peak 规则。
class CostEstimator
{
  public:
    /// 中国区官方价格（CNY / 1M tokens，off-peak），provider = deepseek。
    static CostEstimator DeepSeekCnyDefaults();

    /// 国际区官方价格（USD / 1M tokens，off-peak），provider = deepseek。
    static CostEstimator DeepSeekUsdDefaults();

    void SetPricing(const std::string& provider, const std::string& model, const ModelPricing& pricing);

    CostEstimate Estimate(const std::string& provider, const std::string& model, const core::ChatUsage& usage,
                          std::chrono::system_clock::time_point time) const;

    /// DeepSeek 国际区 peak：UTC 01:00-04:00、06:00-10:00，周一至周五。
    static bool IsDeepSeekUtcPeakTime(std::chrono::system_clock::time_point time);

    /// DeepSeek 中国区 peak：北京时间 09:00-12:00、14:00-18:00，周一至周五。
    static bool IsDeepSeekBeijingPeakTime(std::chrono::system_clock::time_point time);

  private:
    std::unordered_map<std::string, std::unordered_map<std::string, ModelPricing>> m_pricing;
};

} // namespace agent::plugins::metrics
