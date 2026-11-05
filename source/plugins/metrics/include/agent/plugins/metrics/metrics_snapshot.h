#pragma once

#include <cstdint>
#include <string>

namespace agent::plugins::metrics
{

/// 一次会话/进程内累计的性能与费用数据。
struct MetricsSnapshot
{
    // 轮次与步数
    int turns = 0;
    int steps = 0;

    // token 用量
    std::int64_t prompt_tokens = 0;
    std::int64_t completion_tokens = 0;
    std::int64_t total_tokens = 0;
    std::int64_t cache_hit_tokens = 0;
    std::int64_t cache_miss_tokens = 0;

    // 速度、上下文与费用
    double last_tokens_per_second = 0.0;
    double context_usage_percent = 0.0;
    double last_cost = 0.0;
    double total_cost = 0.0;
    std::string currency = "USD";

    // 数据有效性
    bool has_usage = false;
    bool has_cost = false;
};

} // namespace agent::plugins::metrics
