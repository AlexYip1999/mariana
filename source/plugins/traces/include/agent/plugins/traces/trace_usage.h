#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace agent::plugins::traces
{

struct UsageSummary
{
    // 调用计数
    int sessions = 0;
    int turns = 0;
    int model_calls = 0;

    // token 用量
    std::uint64_t prompt_tokens = 0;
    std::uint64_t completion_tokens = 0;
    std::uint64_t total_tokens = 0;
    std::uint64_t cache_hit_tokens = 0;
    std::uint64_t cache_miss_tokens = 0;
};

/// range: "today" 或 "all"。
UsageSummary ScanTraceUsage(const std::filesystem::path& trace_directory, const std::string& range);

std::string FormatUsageSummary(const UsageSummary& summary, const std::string& range);

} // namespace agent::plugins::traces
